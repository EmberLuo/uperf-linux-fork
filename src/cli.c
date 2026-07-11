#define _GNU_SOURCE
#include "log.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/stat.h>

static const char *RUN_DIR = "/run/uperf-linux";
static const char *CONFIG_DIR = "/etc/uperf-linux";

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s <command> [args]\n"
        "\n"
        "Commands:\n"
        "  status              Show current scheduler status\n"
        "  mode <name>         Set power mode (balance|powersave|performance)\n"
        "  game-list           List detected game processes\n"
        "  log-level <n>       Set log level (0=debug..4=fatal)\n"
        "  detect              Scan hardware and print baseline JSON config\n"
        "  calibrate           Run power model calibration (TODO)\n"
        "  set-freq <cluster> <freq_hz>\n"
        "                      Manually set CPU/GPU frequency\n"
        "                      cluster: -1=GPU, 0=Prime, 1=Perf, 2=Eff\n"
        "                      freq_hz: target in Hz (0 = release override)\n"
        "  help                Show this help\n",
        prog);
}

static int cmd_status(void) {
    /* Read current power mode from switch inode */
    char mode_path[MAX_PATH_LEN];
    snprintf(mode_path, sizeof(mode_path), "%s/cur_powermode", RUN_DIR);

    FILE *fp = fopen(mode_path, "r");
    if (fp) {
        char mode[32];
        if (fgets(mode, sizeof(mode), fp)) {
            mode[strcspn(mode, "\n")] = '\0';
            printf("Power mode: %s\n", mode);
        }
        fclose(fp);
    } else {
        printf("Power mode: (not set — using default)\n");
    }

    /* Try to read current CPU frequencies */
    printf("\nCPU frequencies:\n");
    for (int cpu = 0; cpu < 8; cpu++) {
        char path[MAX_PATH_LEN];
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu);
        FILE *f = fopen(path, "r");
        if (f) {
            int freq;
            if (fscanf(f, "%d", &freq) == 1)
                printf("  cpu%d: %d kHz (%.2f MHz)\n", cpu, freq, freq / 1000.0);
            fclose(f);
        }
    }

    /* Check for running games */
    printf("\nKnown game processes:\n");
    DIR *proc = opendir("/proc");
    if (proc) {
        struct dirent *ent;
        int count = 0;
        while ((ent = readdir(proc)) && count < 16) {
            if (!isdigit(ent->d_name[0])) continue;
            char comm_path[MAX_PATH_LEN];
            snprintf(comm_path, sizeof(comm_path), "/proc/%s/comm", ent->d_name);
            FILE *cf = fopen(comm_path, "r");
            if (cf) {
                char comm[64];
                if (fgets(comm, sizeof(comm), cf)) {
                    comm[strcspn(comm, "\n")] = '\0';
                    /* Simple heuristic: game processes tend to have longer names */
                    if (strlen(comm) > 4) {
                        printf("  PID %s: %s\n", ent->d_name, comm);
                        count++;
                    }
                }
                fclose(cf);
            }
        }
        if (count == 0)
            printf("  (none detected)\n");
        closedir(proc);
    }

    return 0;
}

static int cmd_mode(const char *mode_name) {
    struct stat st;
    if (stat(RUN_DIR, &st) != 0) {
        mkdir(RUN_DIR, 0755);
    }

    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/cur_powermode", RUN_DIR);

    FILE *fp = fopen(path, "w");
    if (!fp) {
        fprintf(stderr, "Error: cannot write to %s: %s\n", path, strerror(errno));
        fprintf(stderr, "Hint: run as root, or create %s with proper permissions\n", RUN_DIR);
        return 1;
    }

    fprintf(fp, "%s\n", mode_name);
    fclose(fp);

    printf("Power mode set to: %s\n", mode_name);
    return 0;
}

static int cmd_game_list(void) {
    printf("Scanning for game processes...\n\n");

    DIR *proc = opendir("/proc");
    if (!proc) {
        perror("opendir /proc");
        return 1;
    }

    struct dirent *ent;
    int count = 0;
    while ((ent = readdir(proc)) && count < MAX_GAMES) {
        if (!isdigit(ent->d_name[0])) continue;

        pid_t pid = atoi(ent->d_name);
        if (pid < 2) continue;

        /* Read comm */
        char comm_path[MAX_PATH_LEN];
        char comm[64];
        snprintf(comm_path, sizeof(comm_path), "/proc/%d/comm", pid);
        FILE *fp = fopen(comm_path, "r");
        if (!fp) continue;
        fgets(comm, sizeof(comm), fp);
        comm[strcspn(comm, "\n")] = '\0';
        fclose(fp);

        /* Read cmdline */
        char cmdline[512] = {0};
        snprintf(comm_path, sizeof(comm_path), "/proc/%d/cmdline", pid);
        fp = fopen(comm_path, "r");
        if (fp) {
            size_t n = fread(cmdline, 1, sizeof(cmdline) - 1, fp);
            for (size_t i = 0; i < n; i++)
                if (cmdline[i] == '\0') cmdline[i] = ' ';
            fclose(fp);
        }

        /* Check against game patterns */
        if (game_scanner_match(comm, cmdline)) {
            printf("  [%d] PID %-6d  %-16s  %s\n",
                   ++count, pid, comm, cmdline);
        }
    }
    closedir(proc);

    if (count == 0)
        printf("  (no game processes detected)\n");

    printf("\nTotal: %d game process(es)\n", count);
    return 0;
}

/* Forward declare game_scanner_match — we need to include the header */
#include "game_scanner.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0) {
        print_usage(argv[0]);
        return 0;
    }

    if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0) {
        printf("uperfctl v0.1.0\n");
        return 0;
    }

    if (strcmp(cmd, "status") == 0) {
        return cmd_status();
    }

    if (strcmp(cmd, "mode") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: mode requires an argument\n");
            fprintf(stderr, "Usage: %s mode <balance|powersave|performance>\n", argv[0]);
            return 1;
        }
        return cmd_mode(argv[2]);
    }

    if (strcmp(cmd, "game-list") == 0) {
        return cmd_game_list();
    }

    if (strcmp(cmd, "detect") == 0) {
        /* Delegate to the config wizard */
        static const char *wizard_path = "/usr/local/bin/uperf-wizard";
        char cmd_buf[512];
        snprintf(cmd_buf, sizeof(cmd_buf), "exec %s detect 2>&1", wizard_path);
        int ret = system(cmd_buf);
        return ret >> 8;
    }

    if (strcmp(cmd, "calibrate") == 0) {
        fprintf(stderr, "Calibration not yet implemented. Use uperf-wizard calibrate.\n");
        return 1;
    }

    if (strcmp(cmd, "set-freq") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Error: set-freq requires <cluster> and <freq_hz>\n");
            fprintf(stderr, "Usage: %s set-freq <cluster> <freq_hz>\n", argv[0]);
            fprintf(stderr, "  cluster: -1=GPU, 0=Prime, 1=Perf, 2=Eff\n");
            fprintf(stderr, "  freq_hz: target frequency in Hz (0 = release override)\n");
            return 1;
        }
        int cluster = atoi(argv[2]);
        long long freq_hz = atoll(argv[3]);
        if (cluster < -1 || cluster > 2) {
            fprintf(stderr, "Error: invalid cluster %d (must be -1..2)\n", cluster);
            return 1;
        }
        if (freq_hz < 0) {
            fprintf(stderr, "Error: frequency must be >= 0\n");
            return 1;
        }

        /* Direct sysfs write (no Qt/DBus dependency for CLI) */
        char path[MAX_PATH_LEN];
        char value[32];
        snprintf(value, sizeof(value), "%lld", freq_hz);

        FILE *fp = NULL;
        snprintf(value, sizeof(value), "%lld", freq_hz);

        if (cluster == -1) {
            /* GPU: try common devfreq paths */
            const char *gpu_paths[] = {
                "/sys/class/devfreq/soc:qcom:gpu/max_freq",
                "/sys/class/devfreq/soc\:qcom\:gpu/max_freq",
                NULL
            };
            int found = 0;
            for (int p = 0; gpu_paths[p]; p++) {
                snprintf(path, sizeof(path), "%s", gpu_paths[p]);
                fp = fopen(path, "w");
                if (fp) { found = 1; break; }
            }
            if (!found) {
                fprintf(stderr, "Error: GPU devfreq node not found\n");
                return 1;
            }
        } else {
            /* CPU: write to cpufreq scaling_max_freq */
            snprintf(path, sizeof(path),
                     "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq",
                     cluster);
        }

        FILE *fp = fopen(path, "w");
        if (!fp) {
            fprintf(stderr, "Error: cannot write to %s: %s\n", path, strerror(errno));
            fprintf(stderr, "Hint: run as root\n");
            return 1;
        }
        fprintf(fp, "%s", value);
        fclose(fp);

        printf("Manual frequency set: cluster=%s(%d) = %s Hz\n",
               cluster == -1 ? "GPU" : "CPU", cluster, value);
        return 0;
    }

    if (strcmp(cmd, "show-freqs") == 0) {
        printf("Current CPU frequencies:\n");
        for (int cpu = 0; cpu < 8; cpu++) {
            char path[MAX_PATH_LEN];
            snprintf(path, sizeof(path),
                     "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu);
            FILE *f = fopen(path, "r");
            if (f) {
                long long freq;
                if (fscanf(f, "%lld", &freq) == 1)
                    printf("  cpu%d: %lld Hz (%.2f MHz)\n", cpu, freq, freq / 1e6);
                fclose(f);
            }
        }

        printf("\nGPU frequency:\n");
        char gpu_path[MAX_PATH_LEN];
        snprintf(gpu_path, sizeof(gpu_path),
                 "/sys/class/devfreq/soc:qcom:gpu/msm_dvfsc_score");
        /* Try cur_freq first */
        snprintf(gpu_path, sizeof(gpu_path),
                 "/sys/class/devfreq/soc:qcom:gpu/cur_freq");
        FILE *f = fopen(gpu_path, "r");
        if (f) {
            long long freq;
            if (fscanf(f, "%lld", &freq) == 1)
                printf("  GPU: %lld Hz (%.2f MHz)\n", freq, freq / 1e6);
            fclose(f);
        } else {
            printf("  GPU: (cannot read — devfreq node not found)\n");
        }
        return 0;
    }

    fprintf(stderr, "Unknown command: %s\n", cmd);
    print_usage(argv[0]);
    return 1;
}
