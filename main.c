#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MAX_CPUS 256

typedef struct {
    unsigned long user, nice, system, idle, iowait, irq, softirq, steal;
} cpu_times_t;

int read_cpu_times_from_line(const char* line, cpu_times_t* times) {
    char cpu_name[16];
    unsigned long user, nice, system, idle, iowait, irq, softirq, steal;

    int n = sscanf(line,
        "%15s %lu %lu %lu %lu %lu %lu %lu %lu",
        cpu_name,
        &user, &nice, &system, &idle,
        &iowait, &irq, &softirq, &steal
    );

    if (n == 9) {
        times->user = user;
        times->nice = nice;
        times->system = system;
        times->idle = idle;
        times->iowait = iowait;
        times->irq = irq;
        times->softirq = softirq;
        times->steal = steal;
        return 0;
    }
    return -1;
}

int read_cpu_stats(cpu_times_t* cpus, int* num_cpus) {
    FILE* f = fopen("/proc/stat", "r");
    if (!f) {
        perror("Cannot open /proc/stat");
        return -1;
    }

    char line[512];
    int count = 0;

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "cpu", 3) == 0) {
            if (line[3] == ' ' || (line[3] >= '0' && line[3] <= '9')) {
                if (read_cpu_times_from_line(line, &cpus[count]) == 0) {
                    count++;
                    if (count >= MAX_CPUS) break;
                }
            }
        }
    }

    fclose(f);

    if (count == 0) {
        fprintf(stderr, "Error: No valid 'cpu' lines found in /proc/stat\n");
        return -1;
    }

    *num_cpus = count;
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

    if (curr_total <= prev_total) return 0.0;

    unsigned long total_diff = curr_total - prev_total;
    unsigned long idle_diff = curr_idle - prev_idle;

    if (idle_diff > total_diff) idle_diff = total_diff;

    double usage = (1.0 - (double)idle_diff / (double)total_diff) * 100.0;
    if (usage < 0.0) usage = 0.0;
    if (usage > 100.0) usage = 100.0;
    return usage;
}

int main() {
    cpu_times_t prev[MAX_CPUS] = {0};
    cpu_times_t curr[MAX_CPUS] = {0};
    int num_cpus = 0;

    if (read_cpu_stats(prev, &num_cpus) != 0) {
        return EXIT_FAILURE;
    }

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("Cannot create UDP socket");
        return EXIT_FAILURE;
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(1234);
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Cannot connect to UDP server");
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("UDP connection successful. Sending CPU info via UDP...\n");

    while (1) {
        sleep(1);

        if (read_cpu_stats(curr, &num_cpus) != 0) {
            break;
        }

        char buffer[2048];
        int offset = 0;

        double total = calculate_cpu_usage(&prev[0], &curr[0]);
        offset += snprintf(buffer + offset, sizeof(buffer) - offset, "Total: %.1f%%\n", total);

        for (int i = 1; i < num_cpus; i++) {
            double core = calculate_cpu_usage(&prev[i], &curr[i]);
            offset += snprintf(buffer + offset, sizeof(buffer) - offset, "Core %d: %.1f%%\n", i - 1, core);
        }

        sendto(sockfd, buffer, strlen(buffer), 0,
               (struct sockaddr*)&server_addr, sizeof(server_addr));

        memcpy(prev, curr, sizeof(cpu_times_t) * num_cpus);
    }

    close(sockfd);
    return EXIT_SUCCESS;
}
