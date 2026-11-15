#ifndef SENSORS_H
#define SENSORS_H

typedef struct {
    const char *root, *cpu_file, *gpu_file;
    unsigned gpu_index;
} SensorConfig;

typedef struct { int cpu, gpu; } Reading; /* -1 means unavailable; Celsius */
Reading sensors_read(const SensorConfig *config);
void sensors_close(void);
#endif
