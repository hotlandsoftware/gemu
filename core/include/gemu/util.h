#pragma once
#include <stddef.h>

/* Portable millisecond sleep (no SDL dependency). */
void gemu_sleep_ms(unsigned ms);

/*
 * Build the path of a file inside gemu's per-user config directory:
 *   POSIX:   $HOME/.gemu/<file>            (falls back to /tmp)
 *   Windows: %LOCALAPPDATA%\gemu\<file>    (falls back to %APPDATA%)
 */
void gemu_config_path(char *out, size_t len, const char *file);

/* Best-effort creation of the directory containing path. */
void gemu_ensure_parent_dir(const char *path);
