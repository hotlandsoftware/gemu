#include "gemu/drive.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

bool gemu_parse_drive_spec(const char *arg, GemuDriveSpec *out) {
    *out = (GemuDriveSpec){
        .if_type  = GEMU_DRIVE_IF_IDE,
        .index    = 0,
        .media    = GEMU_DRIVE_MEDIA_DISK,
        .readonly = false,
    };
    bool have_file = false;
    bool readonly_explicit = false;

    char buf[1024];
    if (snprintf(buf, sizeof(buf), "%s", arg) >= (int)sizeof(buf)) {
        fprintf(stderr, "gemu: -drive: argument too long\n");
        return false;
    }

    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq) {
            fprintf(stderr, "gemu: -drive: expected key=value, got '%s'\n", tok);
            return false;
        }
        *eq = '\0';
        const char *key = tok, *val = eq + 1;

        if (strcmp(key, "file") == 0) {
            if (snprintf(out->file, sizeof(out->file), "%s", val) >= (int)sizeof(out->file)) {
                fprintf(stderr, "gemu: -drive: file path too long\n");
                return false;
            }
            have_file = true;
        } else if (strcmp(key, "if") == 0) {
            if      (strcmp(val, "ide")    == 0) out->if_type = GEMU_DRIVE_IF_IDE;
            else if (strcmp(val, "floppy") == 0) out->if_type = GEMU_DRIVE_IF_FLOPPY;
            else {
                fprintf(stderr, "gemu: -drive: unsupported if=%s (use ide or floppy)\n", val);
                return false;
            }
        } else if (strcmp(key, "index") == 0) {
            char *end;
            long v = strtol(val, &end, 10);
            if (*end || v < 0) {
                fprintf(stderr, "gemu: -drive: invalid index '%s'\n", val);
                return false;
            }
            out->index = (int)v;
        } else if (strcmp(key, "media") == 0) {
            if      (strcmp(val, "disk")  == 0) out->media = GEMU_DRIVE_MEDIA_DISK;
            else if (strcmp(val, "cdrom") == 0) out->media = GEMU_DRIVE_MEDIA_CDROM;
            else {
                fprintf(stderr, "gemu: -drive: unsupported media=%s (use disk or cdrom)\n", val);
                return false;
            }
        } else if (strcmp(key, "readonly") == 0) {
            if      (strcmp(val, "on")  == 0) out->readonly = true;
            else if (strcmp(val, "off") == 0) out->readonly = false;
            else {
                fprintf(stderr, "gemu: -drive: readonly must be on or off\n");
                return false;
            }
            readonly_explicit = true;
        } else if (strcmp(key, "format") == 0) {
            if (strcmp(val, "raw") != 0) {
                fprintf(stderr, "gemu: -drive: unsupported format=%s (only raw images are supported)\n", val);
                return false;
            }
        } else {
            fprintf(stderr, "gemu: -drive: unknown key '%s'\n", key);
            return false;
        }
    }

    if (!have_file) {
        fprintf(stderr, "gemu: -drive: missing file=PATH\n");
        return false;
    }
    /* CD-ROM media is always read-only hardware, same as -cdrom always was. */
    if (out->media == GEMU_DRIVE_MEDIA_CDROM && !readonly_explicit)
        out->readonly = true;
    return true;
}
