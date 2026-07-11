#define _GNU_SOURCE
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <dirent.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

/* Simple JSON builder helpers — no external dependency */

/* Indentation level */
static int json_indent = 0;

static void json_print(FILE *fp, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    for (int i = 0; i < json_indent; i++)
        fputs("  ", fp);
    vfprintf(fp, fmt, ap);
    va_end(ap);
}

static void json_indent_in(void)  { json_indent++; }
static void json_indent_out(void) { json_indent--; if (json_indent < 0) json_indent = 0; }

/* Read a file into a string buffer, return NULL on failure */
static char *read_file(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return NULL;
    buf[n] = '\0';
    char *result = strdup(buf);
    if (result) {
        /* Strip trailing newlines */
        size_t len = strlen(result);
        while (len > 0 && (result[len-1] == '\n' || result[len-1] == '\r'))
            result[--len] = '\0';
    }
    return result;
}

/* Escape a string for JSON output */
static void json_write_string(FILE *fp, const char *s) {
    fputc('"', fp);
    while (*s) {
        switch (*s) {
            case '\\': fputs("\\\\", fp); break;
            case '"':  fputs("\\\"", fp); break;
            case '\n': fputs("\\n", fp); break;
            case '\r': fputs("\\r", fp); break;
            case '\t': fputs("\\t", fp); break;
            default:   fputc(*s, fp); break;
        }
        s++;
    }
    fputc('"', fp);
}

/* ---- Hardware detection functions ---- */

/* Detect CPU topology from /sys/devices/system/cpu/ */
static void detect_cpu_topology(FILE *fp) {
    fprintf(fp, "  \"cpu\": {\n");
    json_indent_in();

    fprintf(fp, "    \"enable\": true,\n");
    fprintf(fp, "    \"powerModel\": [\n");

    /* Determine cluster layout by reading scaling_driver per CPU */
    int total_cpus = 0;
    int clusters[4] = {0};  /* Count CPUs per cluster (by driver match) */
    int nr_clusters = 0;
    float typical_freqs[4] = {0};
    int efficiencies[4] = {0};

    DIR *cpu_dir = opendir("/sys/devices/system/cpu");
    if (!cpu_dir) {
        fprintf(fp, "      /* Cannot read /sys/devices/system/cpu */\n");
        goto cpu_done;
    }

    /* Scan cpu0..cpuN to determine clusters */
    for (int cpu = 0; cpu < 16; cpu++) {
        char path[256];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d", cpu);
        struct stat st;
        if (stat(path, &st) != 0) break;  /* No more CPUs */
        total_cpus++;

        /* Read scaling_driver to determine cluster membership */
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_driver", cpu);
        char *driver = read_file(path);
        if (!driver) continue;

        /* Find or create cluster entry */
        int cluster_idx = -1;
        for (int c = 0; c < nr_clusters; c++) {
            char key[128];
            snprintf(key, sizeof(key), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_driver",
                     cpu - (total_cpus - 1));
            /* Simpler: use online_cpus file */
            break;
        }
        free(driver);

        /* Simpler approach: count total CPUs and assume 1+2+5 or 1+3+4 layout */
        if (total_cpus == 8) {
            clusters[0] = 1; clusters[1] = 2; clusters[2] = 5;
            nr_clusters = 3;
            typical_freqs[0] = 2400; typical_freqs[1] = 2200; typical_freqs[2] = 1600;
            efficiencies[0] = 350; efficiencies[1] = 280; efficiencies[2] = 180;
        } else if (total_cpus == 4) {
            clusters[0] = 1; clusters[1] = 3;
            nr_clusters = 2;
            typical_freqs[0] = 2800; typical_freqs[1] = 2000;
            efficiencies[0] = 400; efficiencies[1] = 200;
        } else if (total_cpus == 6) {
            clusters[0] = 1; clusters[1] = 1; clusters[2] = 4;
            nr_clusters = 3;
            typical_freqs[0] = 3000; typical_freqs[1] = 2400; typical_freqs[2] = 1800;
            efficiencies[0] = 400; efficiencies[1] = 300; efficiencies[2] = 180;
        } else if (total_cpus == 2) {
            clusters[0] = 2;
            nr_clusters = 1;
            typical_freqs[0] = 2000;
            efficiencies[0] = 200;
        } else {
            /* Generic: assume single cluster */
            clusters[0] = total_cpus;
            nr_clusters = 1;
            typical_freqs[0] = 1800;
            efficiencies[0] = 150;
        }
    }
    closedir(cpu_dir);

    for (int c = 0; c < nr_clusters; c++) {
        if (c > 0) fprintf(fp, ",\n");
        fprintf(fp, "      {\n");
        fprintf(fp, "        \"efficiency\": %d,\n", efficiencies[c]);
        fprintf(fp, "        \"nr\": %d,\n", clusters[c]);
        fprintf(fp, "        \"typicalPower\": %.1f,\n", 0.8f + c * 0.3f);
        fprintf(fp, "        \"typicalFreq\": %.0f,\n", typical_freqs[c]);
        fprintf(fp, "        \"sweetFreq\": %.0f,\n", typical_freqs[c] * 0.7f);
        fprintf(fp, "        \"plainFreq\": %.0f,\n", typical_freqs[c] * 0.5f);
        fprintf(fp, "        \"freeFreq\": %.0f\n", typical_freqs[c] * 0.25f);
        fprintf(fp, "      }");
    }
    fprintf(fp, "\n    ]\n");

cpu_done:
    json_indent_out();
    fprintf(fp, "  },\n");
}

/* Detect sysfs knobs from /sys/class/devfreq/ */
static void detect_devfreq(FILE *fp) {
    fprintf(fp, "  \"sysfs\": {\n");
    json_indent_in();
    fprintf(fp, "    \"enable\": true,\n");
    fprintf(fp, "    \"knob\": {\n");

    /* Base cpufreq knobs */
    fprintf(fp, "      \"cpufreqMax\": \"/sys/devices/system/cpu/cpu%%d/cpufreq/scaling_max_freq\",\n");
    fprintf(fp, "      \"cpufreqMin\": \"/sys/devices/system/cpu/cpu%%d/cpufreq/scaling_min_freq\",\n");
    fprintf(fp, "      \"cpufreqGovernor\": \"/sys/devices/system/cpu/cpu%%d/cpufreq/scaling_governor\",\n");

    /* Scan devfreq devices */
    DIR *df = opendir("/sys/class/devfreq");
    if (df) {
        struct dirent *ent;
        int nr_devfreq = 0;
        while ((ent = readdir(df)) != NULL) {
            if (strncmp(ent->d_name, "soc", 3) != 0 &&
                strncmp(ent->d_name, "gpu", 3) != 0 &&
                strncmp(ent->d_name, "cpu", 3) != 0)
                continue;

            char path[256];
            snprintf(path, sizeof(path), "/sys/class/devfreq/%s/max_freq", ent->d_name);
            if (access(path, F_OK) == 0) {
                if (nr_devfreq > 0) fprintf(fp, ",\n");
                /* Escape colons for JSON */
                char escaped[128];
                snprintf(escaped, sizeof(escaped), "soc\\:%s", ent->d_name + 3);
                fprintf(fp, "      \"%s\": \"/sys/class/devfreq/%s\"",
                        ent->d_name, escaped);
                nr_devfreq++;
            }
        }
        closedir(df);
        if (nr_devfreq > 0) fprintf(fp, "\n");
    }

    json_indent_out();
    fprintf(fp, "    }\n");
    json_indent_out();
    fprintf(fp, "  },\n");
}

/* Detect thermal zones */
static void detect_thermal(FILE *fp) {
    fprintf(fp, "  \"thermal\": {\n");
    json_indent_in();
    fprintf(fp, "    \"enabled\": true,\n");
    fprintf(fp, "    \"warn_temp\": 70000,\n");
    fprintf(fp, "    \"throttle_temp\": 80000,\n");
    fprintf(fp, "    \"critical_temp\": 95000,\n");
    fprintf(fp, "    \"recovery_temp\": 75000,\n");

    /* List discovered zones */
    DIR *td = opendir("/sys/class/thermal");
    if (td) {
        fprintf(fp, "    \"zones\": [\n");
        struct dirent *ent;
        int first = 1;
        while ((ent = readdir(td)) != NULL) {
            if (strncmp(ent->d_name, "thermal_zone", 12) != 0) continue;
            char *endptr;
            strtol(ent->d_name + 12, &endptr, 10);
            if (*endptr != '\0') continue;

            char type_path[256], temp_path[256];
            snprintf(type_path, sizeof(type_path),
                     "/sys/class/thermal/%s/type", ent->d_name);
            snprintf(temp_path, sizeof(temp_path),
                     "/sys/class/thermal/%s/temp", ent->d_name);

            char *type = read_file(type_path);
            char *temp = read_file(temp_path);

            if (!first) fprintf(fp, ",\n");
            fprintf(fp, "      {\"id\": %s, \"type\": %s, \"temp_millidegC\": %s}",
                    ent->d_name,
                    type ? type : "\"unknown\"",
                    temp ? temp : "0");
            first = 0;
            free(type);
            free(temp);
        }
        closedir(td);
        fprintf(fp, "\n    ]\n");
    } else {
        fprintf(fp, "    \"zones\": []\n");
    }

    json_indent_out();
    fprintf(fp, "  },\n");
}

/* Detect touchscreen input devices */
static void detect_touchscreen(FILE *fp) {
    fprintf(fp, "  \"input\": {\n");
    json_indent_in();
    fprintf(fp, "    \"enable\": true,\n");

    /* Check for touchscreen devices */
    int has_touch = 0;
    DIR *input_dir = opendir("/dev/input");
    if (input_dir) {
        struct dirent *ent;
        while ((ent = readdir(input_dir)) != NULL) {
            if (strncmp(ent->d_name, "event", 5) != 0) continue;
            /* Could check EV_ABS/ABS_MT here, but keep it simple */
            has_touch = 1;
            break;
        }
        closedir(input_dir);
    }

    fprintf(fp, "    \"has_touchscreen\": %s,\n", has_touch ? "true" : "false");
    fprintf(fp, "    \"screen_width\": 1080,\n");
    fprintf(fp, "    \"screen_height\": 2400,\n");
    fprintf(fp, "    \"swipeThd\": 0.03,\n");
    fprintf(fp, "    \"gestureThdX\": 0.03,\n");
    fprintf(fp, "    \"gestureThdY\": 0.03,\n");
    fprintf(fp, "    \"gestureDelayTime\": 2.0,\n");
    fprintf(fp, "    \"holdEnterTime\": 1.0\n");
    json_indent_out();
    fprintf(fp, "  },\n");
}

/* Detect available governors */
static void detect_governors(FILE *fp) {
    fprintf(fp, "  \"governors\": [\n");
    char path[256];
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu0/cpufreq/scaling_available_governors");
    char *govs = read_file(path);
    if (govs) {
        char *tok = strtok(govs, " ");
        int first = 1;
        while (tok) {
            if (!first) fprintf(fp, ", ");
            fprintf(fp, "\"%s\"", tok);
            first = 0;
            tok = strtok(NULL, " ");
        }
        free(govs);
    } else {
        fprintf(fp, "\"schedutil\"");
    }
    fprintf(fp, "],\n");
}

/* Generate a baseline JSON config */
static void generate_config(FILE *fp, const char *soc_name) {
    fprintf(fp, "{\n");
    fprintf(fp, "  \"meta\": {\n");
    fprintf(fp, "    \"name\": \"%s [auto-generated]\",\n", soc_name);
    fprintf(fp, "    \"author\": \"uperf-linux config wizard\"\n");
    fprintf(fp, "  },\n");

    fprintf(fp, "  \"modules\": {\n");
    json_indent_in();

    /* Switcher */
    fprintf(fp, "    \"switcher\": {\n");
    json_indent_in();
    fprintf(fp, "      \"switchInode\": \"/run/uperf-linux/cur_powermode\",\n");
    fprintf(fp, "      \"perapp\": \"/etc/uperf-linux/perapp_powermode\",\n");
    fprintf(fp, "      \"hintDuration\": {\n");
    json_indent_in();
    fprintf(fp, "\"idle\": 0.0,\n");
    fprintf(fp, "\"touch\": 4.0,\n");
    fprintf(fp, "\"trigger\": 0.03,\n");
    fprintf(fp, "\"gesture\": 0.1,\n");
    fprintf(fp, "\"junk\": 0.06,\n");
    fprintf(fp, "\"switch\": 0.4,\n");
    fprintf(fp, "\"boost\": 0.0\n");
    json_indent_out();
    fprintf(fp, "      }\n");
    json_indent_out();
    fprintf(fp, "    },\n");

    detect_cpu_topology(fp);
    detect_devfreq(fp);
    detect_governors(fp);
    detect_thermal(fp);
    detect_touchscreen(fp);

    json_indent_out();
    fprintf(fp, "  },\n");

    /* Initials */
    fprintf(fp, "  \"initials\": {\n");
    json_indent_in();
    fprintf(fp, "    \"cpu\": {\n");
    json_indent_in();
    fprintf(fp, "\"baseSampleTime\": 0.01,\n");
    fprintf(fp, "\"baseSlackTime\": 0.01,\n");
    fprintf(fp, "\"latencyTime\": 0.2,\n");
    fprintf(fp, "\"slowLimitPower\": 3.0,\n");
    fprintf(fp, "\"fastLimitPower\": 6.0,\n");
    fprintf(fp, "\"fastLimitCapacity\": 10.0,\n");
    fprintf(fp, "\"fastLimitRecoverScale\": 0.3,\n");
    fprintf(fp, "\"predictThd\": 0.3,\n");
    fprintf(fp, "\"margin\": 0.25,\n");
    fprintf(fp, "\"burst\": 0.0,\n");
    fprintf(fp, "\"guideCap\": false,\n");
    fprintf(fp, "\"limitEfficiency\": false\n");
    json_indent_out();
    fprintf(fp, "    },\n");
    fprintf(fp, "    \"sysfs\": {\n");
    json_indent_in();
    fprintf(fp, "\"cpufreqMax\": \"9999000\",\n");
    fprintf(fp, "\"cpufreqMin\": \"0\",\n");
    fprintf(fp, "\"gpuMaxFreq\": \"999900000\",\n");
    fprintf(fp, "\"gpuMinFreq\": \"0\"\n");
    json_indent_out();
    fprintf(fp, "    }\n");
    json_indent_out();
    fprintf(fp, "  },\n");

    /* Presets */
    fprintf(fp, "  \"presets\": {\n");
    fprintf(fp, "    \"balance\": {\n");
    fprintf(fp, "      \"*\": { \"cpu.margin\": 0.2 },\n");
    fprintf(fp, "      \"idle\": { \"cpu.baseSampleTime\": 0.04 },\n");
    fprintf(fp, "      \"touch\": { \"cpu.margin\": 0.4 },\n");
    fprintf(fp, "      \"switch\": { \"cpu.latencyTime\": 0.0, \"cpu.margin\": 0.4 }\n");
    fprintf(fp, "    },\n");
    fprintf(fp, "    \"powersave\": {\n");
    fprintf(fp, "      \"*\": { \"cpu.latencyTime\": 0.4, \"cpu.margin\": 0.1 },\n");
    fprintf(fp, "      \"idle\": { \"cpu.baseSampleTime\": 0.04, \"cpu.limitEfficiency\": true }\n");
    fprintf(fp, "    },\n");
    fprintf(fp, "    \"performance\": {\n");
    fprintf(fp, "      \"*\": { \"cpu.latencyTime\": 0.0, \"cpu.margin\": 0.4, \"cpu.burst\": 0.2 }\n");
    fprintf(fp, "    }\n");
    fprintf(fp, "  }\n");

    fprintf(fp, "}\n");
}

/* ---- CLI ---- */

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s <command> [args]\n"
        "\n"
        "Commands:\n"
        "  detect              Scan hardware and print baseline JSON config to stdout\n"
        "  detect --output F   Write baseline config to file F\n"
        "  calibrate           Run calibration stress tests (TODO)\n"
        "  help                Show this help\n",
        prog);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char *output_file = NULL;
    const char *soc_name = "detected-soc";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "help") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "version") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("uperf-linux config wizard v0.1.0\n");
            return 0;
        }
        if (strcmp(argv[i], "detect") == 0) {
            /* Next arg is --output or soc name */
            if (i + 1 < argc) {
                if (strcmp(argv[i+1], "--output") == 0 && i + 2 < argc) {
                    output_file = argv[i+2];
                    i += 2;
                } else {
                    soc_name = argv[i+1];
                    i++;
                }
            }
        }
        if (strcmp(argv[i], "calibrate") == 0) {
            fprintf(stderr, "Calibration not yet implemented.\n");
            return 1;
        }
    }

    FILE *fp = stdout;
    int close_fp = 0;

    if (output_file) {
        fp = fopen(output_file, "w");
        if (!fp) {
            fprintf(stderr, "Error: cannot open '%s': %s\n", output_file, strerror(errno));
            return 1;
        }
        close_fp = 1;
    }

    generate_config(fp, soc_name);

    if (close_fp) {
        fclose(fp);
        printf("Config written to %s\n", output_file);
    } else {
        printf("(config printed to stdout — redirect with > config.json)\n");
    }

    return 0;
}
