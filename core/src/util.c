#include "gemu/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <direct.h>
#  define gemu_mkdir(p) _mkdir(p)
#else
#  include <time.h>
#  include <sys/stat.h>
#  define gemu_mkdir(p) mkdir((p), 0755)
#endif

void gemu_sleep_ms(unsigned ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

void gemu_config_path(char *out, size_t len, const char *file) {
#ifdef _WIN32
    const char *base = getenv("LOCALAPPDATA");
    if (!base || !base[0]) base = getenv("APPDATA");
    if (!base || !base[0]) base = "C:\\Users\\Default\\AppData\\Local";
    snprintf(out, len, "%s\\gemu\\%s", base, file);
#else
    const char *home = getenv("HOME");
    if (!home || !home[0]) home = "/tmp";
    snprintf(out, len, "%s/.gemu/%s", home, file);
#endif
}

void gemu_ensure_parent_dir(const char *path) {
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", path);
    char *sep = strrchr(dir, '/');
#ifdef _WIN32
    char *sep2 = strrchr(dir, '\\');
    if (sep2 > sep) sep = sep2;
#endif
    if (sep) { *sep = '\0'; gemu_mkdir(dir); }
}
