# cpu-client

# Функции
1. Сбор и обработка данных о поядерной загрузке CPU в Linux из /proc/stat 
2. Частота опроса /proc/stat – 1 Гц.
3. Выходные данные (поядерной загрузке CPU) отправляться на порт UDP localhost:1234

# Формат отправки
Total: <float>%
Core <N>: <float>%
Core <N+1>: <float>%
...
Core <M>: <float>%

# Сборка
```bash
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build .
./cpu-client
```
или
```bash
gcc -o cpu_client main.c
./cpu-client
```

# Улучшение
Оптимизирован код, максимально испульзуется динамическое выделение памяти, и др. Потребление CPU на сбор, обработку и отправку данных на сервер, по отдельным ядрам CPU сократился в 2-3 раза.
!(./assets/Screenshot_20260202_110159.png)
