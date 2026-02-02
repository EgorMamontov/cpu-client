#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>

#define DEFAULT_PORT 1234
#define DEFAULT_IP "127.0.0.1"
#define UPDATE_INTERVAL 1
#define MAX_CPUS_INIT 16
#define BUFFER_INIT_SIZE 1024
#define MAX_RETRIES 3

typedef struct __attribute__((packed)) {
    unsigned long user, nice, system, idle, iowait, irq, softirq, steal;
} cpu_times_t;

typedef struct {
    cpu_times_t* prev;
    cpu_times_t* curr;
    int num_cpus;
    int capacity;
} cpu_stats_t;

volatile sig_atomic_t stop = 0;

void handle_signal(int sig) {
    (void)sig;
    stop = 1;
}

int read_cpu_times_from_line(const char* line, cpu_times_t* times) {
    const char* p = line;

    // Пропускаем "cpu" или "cpuX"
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;

    // Парсим числа с помощью strtoul (быстрее чем sscanf)
    char* endptr;

    times->user = strtoul(p, &endptr, 10);
    if (*endptr == '\0') return -1;
    p = endptr;

    times->nice = strtoul(p, &endptr, 10);
    if (*endptr == '\0') return -1;
    p = endptr;

    times->system = strtoul(p, &endptr, 10);
    if (*endptr == '\0') return -1;
    p = endptr;

    times->idle = strtoul(p, &endptr, 10);
    if (*endptr == '\0') return -1;
    p = endptr;

    times->iowait = strtoul(p, &endptr, 10);
    if (*endptr == '\0') return -1;
    p = endptr;

    times->irq = strtoul(p, &endptr, 10);
    if (*endptr == '\0') return -1;
    p = endptr;

    times->softirq = strtoul(p, &endptr, 10);
    if (*endptr == '\0') return -1;
    p = endptr;

    times->steal = strtoul(p, &endptr, 10);

    return 0;
}

int read_cpu_stats(cpu_stats_t* stats, int* new_num_cpus) {
    FILE* f = fopen("/proc/stat", "r");
    if (!f) {
        perror("Cannot open /proc/stat");
        return -1;
    }

    char* line = NULL;
    size_t len = 0;
    ssize_t read;
    int count = 0;

    // Читаем все строки, начинающиеся с cpu
    while ((read = getline(&line, &len, f)) != -1) {
        if (strncmp(line, "cpu", 3) == 0) {
            if (line[3] == ' ' || (line[3] >= '0' && line[3] <= '9')) {
                // Проверяем, нужно ли увеличить массив
                if (count >= stats->capacity) {
                    int new_capacity = stats->capacity * 2;
                    cpu_times_t* new_curr = realloc(stats->curr, new_capacity * sizeof(cpu_times_t));
                    cpu_times_t* new_prev = realloc(stats->prev, new_capacity * sizeof(cpu_times_t));

                    if (!new_curr || !new_prev) {
                        free(new_curr);
                        free(new_prev);
                        fprintf(stderr, "Memory allocation failed\n");
                        free(line);
                        fclose(f);
                        return -1;
                    }

                    stats->curr = new_curr;
                    stats->prev = new_prev;
                    stats->capacity = new_capacity;
                }

                if (read_cpu_times_from_line(line, &stats->curr[count]) == 0) {
                    count++;
                }
            }
        }
    }

    free(line);
    fclose(f);

    if (count == 0) {
        fprintf(stderr, "Error: No valid 'cpu' lines found in /proc/stat\n");
        return -1;
    }

    *new_num_cpus = count;
    return 0;
}

double calculate_cpu_usage(const cpu_times_t* prev, const cpu_times_t* curr) {
    unsigned long prev_idle = prev->idle + prev->iowait;
    unsigned long curr_idle = curr->idle + curr->iowait;

    unsigned long prev_total = prev->user + prev->nice + prev->system +
                               prev->idle + prev->iowait + prev->irq +
                               prev->softirq + prev->steal;
    unsigned long curr_total = curr->user + curr->nice + curr->system +
                               curr->idle + curr->iowait + curr->irq +
                               curr->softirq + curr->steal;

    unsigned long total_diff, idle_diff;

    // Обработка переполнения счетчиков
    if (curr_total >= prev_total) {
        total_diff = curr_total - prev_total;
        idle_diff = curr_idle - prev_idle;
    } else {
        // Счетчики переполнились
        total_diff = (ULONG_MAX - prev_total) + curr_total + 1;
        idle_diff = (ULONG_MAX - prev_idle) + curr_idle + 1;
    }

    if (total_diff == 0) return 0.0;
    if (idle_diff > total_diff) idle_diff = total_diff;

    double usage = (1.0 - (double)idle_diff / (double)total_diff) * 100.0;

    // Ограничиваем значения
    if (usage < 0.0) usage = 0.0;
    if (usage > 100.0) usage = 100.0;

    return usage;
}

int init_udp_connection(const char* ip, int port, int* sockfd) {
    *sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (*sockfd < 0) {
        perror("Cannot create UDP socket");
        return -1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0) {
        perror("Invalid IP address");
        close(*sockfd);
        return -1;
    }

    if (connect(*sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Cannot connect to UDP server");
        close(*sockfd);
        return -1;
    }

    return 0;
}

char* format_cpu_stats(cpu_stats_t* stats, int* buffer_size) {
    // Вычисляем необходимый размер буфера
    // Формат: "Total: XX.X%\n" для каждого ядра + нуль-терминатор
    int estimated_size = (stats->num_cpus * 32) + 1; // ~32 байт на CPU

    // Выделяем или перераспределяем буфер
    static char* buffer = NULL;
    static int current_size = 0;

    if (estimated_size > current_size) {
        free(buffer);
        buffer = malloc(estimated_size);
        if (!buffer) {
            fprintf(stderr, "Failed to allocate buffer\n");
            return NULL;
        }
        current_size = estimated_size;
    }

    int offset = 0;

    // Форматируем общую загрузку CPU
    double total = calculate_cpu_usage(&stats->prev[0], &stats->curr[0]);
    offset += snprintf(buffer + offset, current_size - offset,
                      "Total: %.1f%%\n", total);

    // Форматируем загрузку по ядрам
    for (int i = 1; i < stats->num_cpus; i++) {
        double core = calculate_cpu_usage(&stats->prev[i], &stats->curr[i]);
        offset += snprintf(buffer + offset, current_size - offset,
                          "Core %d: %.1f%%\n", i - 1, core);
    }

    *buffer_size = offset;
    return buffer;
}

void free_cpu_stats(cpu_stats_t* stats) {
    if (stats) {
        free(stats->prev);
        free(stats->curr);
        stats->prev = NULL;
        stats->curr = NULL;
        stats->num_cpus = 0;
        stats->capacity = 0;
    }
}

int main() {
    // Устанавливаем обработчики сигналов
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    // Инициализация структуры статистики CPU
    cpu_stats_t stats = {0};
    stats.capacity = MAX_CPUS_INIT;

    stats.prev = malloc(stats.capacity * sizeof(cpu_times_t));
    stats.curr = malloc(stats.capacity * sizeof(cpu_times_t));

    if (!stats.prev || !stats.curr) {
        fprintf(stderr, "Failed to allocate memory for CPU stats\n");
        free(stats.prev);
        free(stats.curr);
        return EXIT_FAILURE;
    }

    // Первое чтение статистики
    int retries = MAX_RETRIES;
    while (retries-- > 0) {
        if (read_cpu_stats(&stats, &stats.num_cpus) == 0) {
            break;
        }
        sleep(1);
    }

    if (retries < 0) {
        fprintf(stderr, "Failed to read CPU stats after %d attempts\n", MAX_RETRIES);
        free_cpu_stats(&stats);
        return EXIT_FAILURE;
    }

    // Инициализация UDP соединения
    int sockfd;
    if (init_udp_connection(DEFAULT_IP, DEFAULT_PORT, &sockfd) != 0) {
        free_cpu_stats(&stats);
        return EXIT_FAILURE;
    }

    printf("UDP connection successful. Monitoring %d CPU(s). Press Ctrl+C to stop.\n",
           stats.num_cpus - 1); // -1 потому что первый элемент - общая статистика

    // Основной цикл
    while (!stop) {
        sleep(UPDATE_INTERVAL);

        int new_num_cpus;
        if (read_cpu_stats(&stats, &new_num_cpus) != 0) {
            continue; // Пропускаем эту итерацию при ошибке
        }

        // Если количество CPU изменилось, обновляем размеры
        if (new_num_cpus != stats.num_cpus) {
            printf("CPU count changed from %d to %d\n",
                   stats.num_cpus - 1, new_num_cpus - 1);
            stats.num_cpus = new_num_cpus;
        }

        // Форматируем и отправляем статистику
        int buffer_size;
        char* buffer = format_cpu_stats(&stats, &buffer_size);

        if (buffer) {
            ssize_t sent = send(sockfd, buffer, buffer_size, 0);
            if (sent < 0) {
                if (errno == ECONNREFUSED || errno == ENETUNREACH) {
                    fprintf(stderr, "Network error: %s\n", strerror(errno));
                    // Можно добавить логику повторного подключения
                } else {
                    perror("Failed to send data");
                }
            }
        }

        // Обмен указателями вместо копирования
        cpu_times_t* temp = stats.prev;
        stats.prev = stats.curr;
        stats.curr = temp;
    }

    // Очистка ресурсов
    printf("\nShutting down...\n");
    close(sockfd);
    free_cpu_stats(&stats);

    // Освобождаем статический буфер
    static char* format_buffer = NULL;
    free(format_buffer);

    return EXIT_SUCCESS;
}
