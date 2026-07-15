#define _GNU_SOURCE
#include "game_scanner.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#include <ctype.h>

/* Known game patterns */
static const char *DEFAULT_PATTERNS[] = {
    "UnityMain", "GameThread", "RenderThread", "GLThread",
    "dolphin", "ppsspp", "retroarch", "wine", "proton", "miHoYo",
    "minecraft", "gameloft", "king", "supercell",
    "niantic", "puzzle", "quest", "legends",
    "arena", "blaze", "rovio", "ea.games",
    "playdead", "half-life", "steam", "epic",
    "gta", "pubg", "fortnite", "cod",
    "genshin", "honkai", "zzz", "arknights",
    NULL
};

/* Internal game scanner state */
struct GameScanner {
    char *patterns[MAX_PATTERNS];
    int nr_patterns;
    GameProcess entries[MAX_GAMES];
    int nr_entries;
    char app_modes[MAX_GAMES][MAX_NAME_LEN]; /* per-app mode storage */
};

static void copy_truncated(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0) return;
    size_t n = src ? strnlen(src, dst_size - 1) : 0;
    if (n > 0) memcpy(dst, src, n);
    dst[n] = '\0';
}

static bool matches_pattern(GameScanner *gs, const char *text) {
    if (!text) return false;
    for (int i = 0; i < gs->nr_patterns; i++) {
        if (strstr(text, gs->patterns[i])) return true;
    }
    return false;
}

bool game_scanner_match(const char *comm, const char *cmdline) {
    /* Default match without scanner instance */
    GameScanner dummy;
    dummy.nr_patterns = 0;
    for (int i = 0; DEFAULT_PATTERNS[i]; i++) {
        dummy.patterns[dummy.nr_patterns++] = (char *)DEFAULT_PATTERNS[i];
        if (dummy.nr_patterns >= MAX_PATTERNS) break;
    }
    bool r = matches_pattern(&dummy, comm) || matches_pattern(&dummy, cmdline);
    return r;
}

GameScanner *game_scanner_new(void) {
    GameScanner *gs = calloc(1, sizeof(*gs));
    if (!gs) return NULL;

    /* Load default patterns */
    for (int i = 0; DEFAULT_PATTERNS[i]; i++) {
        if (gs->nr_patterns >= MAX_PATTERNS) break;
        gs->patterns[gs->nr_patterns++] = strdup(DEFAULT_PATTERNS[i]);
    }

    log_debug("GameScanner created with %d patterns", gs->nr_patterns);
    return gs;
}

void game_scanner_free(GameScanner *gs) {
    if (!gs) return;
    for (int i = 0; i < gs->nr_patterns; i++) {
        free(gs->patterns[i]);
    }
    free(gs);
    log_debug("GameScanner destroyed");
}

int game_scanner_add_pattern(GameScanner *gs, const char *pattern) {
    if (!gs || !pattern) return -1;
    if (gs->nr_patterns >= MAX_PATTERNS) return -1;
    gs->patterns[gs->nr_patterns++] = strdup(pattern);
    return 0;
}

int game_scanner_pattern_count(const GameScanner *gs) {
    return gs ? gs->nr_patterns : 0;
}

int game_scanner_scan(GameScanner *gs) {
    if (!gs) return -1;

    DIR *proc = opendir("/proc");
    if (!proc) {
        log_error("game_scanner: cannot open /proc: %s", strerror(errno));
        return -1;
    }

    struct dirent *ent;
    int count = 0;

    while ((ent = readdir(proc)) && count < MAX_GAMES) {
        if (!isdigit((unsigned char)ent->d_name[0])) continue;

        pid_t pid = atoi(ent->d_name);
        if (pid < 2) continue;

        /* Read comm */
        char comm_path[64];
        char comm[64];
        snprintf(comm_path, sizeof(comm_path), "/proc/%d/comm", pid);
        FILE *fp = fopen(comm_path, "r");
        if (!fp) continue;
        if (!fgets(comm, sizeof(comm), fp)) {
            fclose(fp);
            continue;
        }
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

        if (matches_pattern(gs, comm) || matches_pattern(gs, cmdline)) {
            GameProcess *p = &gs->entries[count];
            p->pid = pid;
            copy_truncated(p->comm, sizeof(p->comm), comm);
            copy_truncated(p->cmdline, sizeof(p->cmdline), cmdline);
            p->package[0] = '\0';
            p->is_game = true;
            gs->app_modes[count][0] = '\0';
            count++;
        }
    }

    closedir(proc);
    gs->nr_entries = count;
    log_debug("game_scanner: found %d game process(es)", count);
    return count;
}

int game_scanner_get_results(GameScanner *gs, GameProcess *out, int max_entries) {
    if (!gs || !out) return -1;
    int n = gs->nr_entries < max_entries ? gs->nr_entries : max_entries;
    for (int i = 0; i < n; i++) {
        out[i] = gs->entries[i];
    }
    return n;
}

int game_scanner_perapp_scan(GameScanner *gs, const char *perapp_file) {
    if (!gs || !perapp_file) return -1;

    /* Read per-app mode assignments from file */
    FILE *fp = fopen(perapp_file, "r");
    if (!fp) {
        log_warn("perapp config not found: %s", perapp_file);
        return 0;
    }

    char line[256];
    int count = 0;
    while (fgets(line, sizeof(line), fp) && count < MAX_GAMES) {
        /* Format: comm mode */
        char comm[64], mode[32];
        if (sscanf(line, "%63s %31s", comm, mode) == 2) {
            /* Find matching entry */
            for (int i = 0; i < gs->nr_entries; i++) {
                if (strcmp(gs->entries[i].comm, comm) == 0) {
                    strncpy(gs->app_modes[i], mode, MAX_NAME_LEN - 1);
                    gs->app_modes[i][MAX_NAME_LEN - 1] = '\0';
                    count++;
                    break;
                }
            }
        }
    }
    fclose(fp);
    log_debug("perapp_scan: assigned modes to %d game(s)", count);
    return count;
}

const char *game_scanner_get_app_mode(const GameScanner *gs, const char *comm) {
    if (!gs || !comm) return "balance";
    for (int i = 0; i < gs->nr_entries; i++) {
        if (strcmp(gs->entries[i].comm, comm) == 0 && gs->app_modes[i][0] != '\0') {
            return gs->app_modes[i];
        }
    }
    return "balance";
}
