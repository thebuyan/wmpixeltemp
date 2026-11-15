#define _POSIX_C_SOURCE 200809L
#include "sensors.h"
#include <dlfcn.h>
#include <glob.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Minimal public NVML ABI: optional at runtime, no CUDA SDK needed. */
typedef struct nvmlDevice_st *Device;
static void *nvml;
static int ready;
static int (*shutdown_nvml)(void);
static int (*get_device)(unsigned, Device *);
static int (*get_temperature)(Device, unsigned, unsigned *);

static int read_text(const char *path, char *text, size_t cap)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int ok = fgets(text, (int)cap, f) != NULL;
    fclose(f);
    if (ok) text[strcspn(text, "\r\n")] = 0;
    return ok;
}

static int temperature(const char *path)
{
    char text[128], *end;
    if (!read_text(path, text, sizeof text)) return -1;
    long value = strtol(text, &end, 10);
    while (*end == ' ' || *end == '\t') ++end;
    if (end == text || *end || value < 0 || value > 150000) return -1;
    return (int)((value + 500) / 1000);
}

static int hottest(const char *pattern)
{
    glob_t paths = {0};
    int result = -1;
    if (glob(pattern, 0, NULL, &paths) == 0) {
        for (size_t i = 0; i < paths.gl_pathc; ++i) {
            int t = temperature(paths.gl_pathv[i]);
            if (t > result) result = t;
        }
    }
    globfree(&paths);
    return result;
}

static int cpu_temperature(const SensorConfig *c)
{
    if (c->cpu_file) return temperature(c->cpu_file);
    char path[PATH_MAX], name[128];
    glob_t dirs = {0};
    int result = -1;
    snprintf(path, sizeof path, "%s/class/hwmon/hwmon*", c->root);
    if (glob(path, 0, NULL, &dirs) == 0) {
        for (size_t i = 0; i < dirs.gl_pathc; ++i) {
            snprintf(path, sizeof path, "%s/name", dirs.gl_pathv[i]);
            if (!read_text(path, name, sizeof name)) continue;
            if (strcmp(name, "coretemp") && strcmp(name, "k10temp") &&
                strcmp(name, "zenpower") && strcmp(name, "cpu_thermal") &&
                strcmp(name, "peci_cputemp")) continue;
            snprintf(path, sizeof path, "%s/temp*_input", dirs.gl_pathv[i]);
            int t = hottest(path);
            if (t > result) result = t;
        }
    }
    globfree(&dirs);
    if (result >= 0) return result;
    memset(&dirs, 0, sizeof dirs);
    snprintf(path, sizeof path, "%s/class/thermal/thermal_zone*", c->root);
    if (glob(path, 0, NULL, &dirs) == 0) {
        for (size_t i = 0; i < dirs.gl_pathc; ++i) {
            snprintf(path, sizeof path, "%s/type", dirs.gl_pathv[i]);
            if (!read_text(path, name, sizeof name)) continue;
            if (strcmp(name, "x86_pkg_temp") && strcmp(name, "cpu-thermal") &&
                strcmp(name, "cpu_thermal")) continue;
            snprintf(path, sizeof path, "%s/temp", dirs.gl_pathv[i]);
            int t = temperature(path);
            if (t > result) result = t;
        }
    }
    globfree(&dirs);
    return result;
}

static int nvidia_temperature(unsigned index)
{
    if (!nvml) {
        nvml = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!nvml) return -1;
        int (*init)(void);
        /* POSIX dlsym returns a function address through void *. */
        *(void **)(&init) = dlsym(nvml, "nvmlInit_v2");
        *(void **)(&shutdown_nvml) = dlsym(nvml, "nvmlShutdown");
        *(void **)(&get_device) = dlsym(nvml, "nvmlDeviceGetHandleByIndex_v2");
        *(void **)(&get_temperature) = dlsym(nvml, "nvmlDeviceGetTemperature");
        if (!init || !shutdown_nvml || !get_device || !get_temperature) {
            dlclose(nvml); nvml = NULL; return -1;
        }
        ready = init() == 0;
    } else if (!ready) {
        int (*init)(void);
        *(void **)(&init) = dlsym(nvml, "nvmlInit_v2");
        ready = init && init() == 0;
    }
    if (!ready) return -1;
    Device dev;
    unsigned t;
    if (get_device(index, &dev) || get_temperature(dev, 0, &t) || t > 150)
        return -1;
    return (int)t;
}

Reading sensors_read(const SensorConfig *c)
{
    Reading r = {cpu_temperature(c), -1};
    if (c->gpu_file) r.gpu = temperature(c->gpu_file);
    else {
        /* A custom root is also useful for offline diagnostics and tests. */
        if (!strcmp(c->root, "/sys")) r.gpu = nvidia_temperature(c->gpu_index);
        if (r.gpu < 0) {
            char pattern[PATH_MAX];
            snprintf(pattern, sizeof pattern,
                     "%s/class/drm/card%u/device/hwmon/hwmon*/temp1_input",
                     c->root, c->gpu_index);
            r.gpu = hottest(pattern);
        }
    }
    return r;
}

void sensors_close(void)
{
    if (ready) shutdown_nvml();
    if (nvml) dlclose(nvml);
    nvml = NULL; ready = 0;
}
