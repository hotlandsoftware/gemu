/*
 * GEMU monitor - the QMP blockdev-* commands.
 *
 * QEMU separates a drive's tray from the medium in it, and media from the
 * block nodes that back them:
 *
 *   blockdev-add            create a named node for an image file
 *   blockdev-open-tray      open the tray; the medium stays in it
 *   blockdev-remove-medium  take the medium out of the open tray
 *   blockdev-insert-medium  put a node into the open tray
 *   blockdev-close-tray     close the tray (the guest sees the medium)
 *   blockdev-change-medium  all of the above in one go, from a file name
 *   blockdev-del            delete a node that no drive is using
 *
 * GEMU's media devices only know "loaded" and "empty", so the tray lives
 * here: opening it ejects the medium from the device while remembering it,
 * and closing it loads whatever is in the tray.  Everything runs on the
 * emulator thread through mon_block_run, like any other monitor command.
 */
#include "monitor_priv.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void fail(MonBlockReq *r, const char *klass, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->desc, sizeof(r->desc), fmt, ap);
    va_end(ap);
    r->klass = klass;
}

/* The device named by "device" or "id"; QEMU's DeviceNotFound otherwise. */
static int find_device(GemuMonitor *mon, MonBlockReq *r) {
    for (int i = 0; i < mon->n_media; i++)
        if (strcasecmp(mon->media[i].name, r->device) == 0)
            return i;
    fail(r, "DeviceNotFound", "Device '%s' not found", r->device);
    return -1;
}

static int find_node(GemuMonitor *mon, const char *name) {
    for (int i = 0; i < BLOCK_NODE_MAX; i++)
        if (mon->nodes[i].used && strcmp(mon->nodes[i].name, name) == 0)
            return i;
    return -1;
}

static bool removable(GemuMediaDevice *dev, MonBlockReq *r) {
    if (dev->eject && dev->change) return true;
    fail(r, "GenericError", "Device '%s' is not removable", dev->name);
    return false;
}

/* The drive no longer holds the node that backed its medium. */
static void detach_node(GemuMonitor *mon, int i) {
    MonTray *t = &mon->tray[i];
    if (t->node) mon->nodes[t->node - 1].attached = 0;
    t->node = 0;
}

static GemuMonCmd result_cmd(GemuMediaResult res) {
    return res == GEMU_MEDIA_OK_RESET ? GEMU_MON_RESET : GEMU_MON_NONE;
}

static GemuMonCmd open_tray(GemuMonitor *mon, MonBlockReq *r, int i) {
    GemuMediaDevice *dev = &mon->media[i];
    MonTray *t = &mon->tray[i];
    if (!removable(dev, r) || t->open) return GEMU_MON_NONE;
    GemuMediaResult res = GEMU_MEDIA_OK;
    if (dev->file[0]) {
        char file[sizeof(t->file)], err[256] = "";
        snprintf(file, sizeof(file), "%s", dev->file);   /* eject may clear it */
        res = dev->eject(dev->ud, err, sizeof(err));
        if (res == GEMU_MEDIA_ERR) {
            fail(r, "GenericError", "%s", err[0] ? err : "could not open the tray");
            return GEMU_MON_NONE;
        }
        snprintf(t->file, sizeof(t->file), "%s", file);
        dev->file[0] = '\0';
    }
    t->open = true;
    return result_cmd(res);
}

static GemuMonCmd close_tray(GemuMonitor *mon, MonBlockReq *r, int i) {
    GemuMediaDevice *dev = &mon->media[i];
    MonTray *t = &mon->tray[i];
    if (!removable(dev, r) || !t->open) return GEMU_MON_NONE;
    GemuMediaResult res = GEMU_MEDIA_OK;
    if (t->file[0]) {
        char err[256] = "";
        res = dev->change(dev->ud, t->file, err, sizeof(err));
        if (res == GEMU_MEDIA_ERR) {   /* the tray stays open, medium in it */
            fail(r, "GenericError", "%s", err[0] ? err : "could not load the medium");
            return GEMU_MON_NONE;
        }
        snprintf(dev->file, sizeof(dev->file), "%s", t->file);
        t->file[0] = '\0';
    }
    t->open = false;
    return result_cmd(res);
}

static bool tray_is_open(GemuMonitor *mon, MonBlockReq *r, int i) {
    if (mon->tray[i].open) return true;
    fail(r, "GenericError", "Tray of device '%s' is not open", mon->media[i].name);
    return false;
}

static void remove_medium(GemuMonitor *mon, int i) {
    mon->tray[i].file[0] = '\0';
    detach_node(mon, i);
}

static GemuMonCmd do_add(GemuMonitor *mon, MonBlockReq *r) {
    if (find_node(mon, r->node) >= 0) {
        fail(r, "GenericError", "Duplicate nodes with node-name='%s'", r->node);
        return GEMU_MON_NONE;
    }
    FILE *f = fopen(r->file, r->read_only ? "rb" : "r+b");
    if (!f && !r->read_only) f = fopen(r->file, "rb");
    if (!f) {
        fail(r, "GenericError", "Could not open '%s': No such file or directory", r->file);
        return GEMU_MON_NONE;
    }
    fclose(f);
    for (int i = 0; i < BLOCK_NODE_MAX; i++) {
        MonBlockNode *n = &mon->nodes[i];
        if (n->used) continue;
        memset(n, 0, sizeof(*n));
        n->used = true;
        n->read_only = r->read_only;
        snprintf(n->name, sizeof(n->name), "%s", r->node);
        snprintf(n->file, sizeof(n->file), "%s", r->file);
        return GEMU_MON_NONE;
    }
    fail(r, "GenericError", "Too many block nodes (at most %d)", BLOCK_NODE_MAX);
    return GEMU_MON_NONE;
}

static GemuMonCmd do_del(GemuMonitor *mon, MonBlockReq *r) {
    int n = find_node(mon, r->node);
    if (n < 0)
        fail(r, "GenericError", "Failed to find node with node-name='%s'", r->node);
    else if (mon->nodes[n].attached)
        fail(r, "GenericError", "Node '%s' is busy: block device is in use by '%s'",
             r->node, mon->media[mon->nodes[n].attached - 1].name);
    else
        mon->nodes[n].used = false;
    return GEMU_MON_NONE;
}

static GemuMonCmd do_insert(GemuMonitor *mon, MonBlockReq *r, int i) {
    int n = find_node(mon, r->node);
    if (n < 0) {
        fail(r, "GenericError", "Node '%s' not found", r->node);
        return GEMU_MON_NONE;
    }
    if (!removable(&mon->media[i], r) || !tray_is_open(mon, r, i)) return GEMU_MON_NONE;
    if (mon->nodes[n].attached) {
        fail(r, "GenericError", "Node '%s' is already in use", r->node);
        return GEMU_MON_NONE;
    }
    MonTray *t = &mon->tray[i];
    if (t->file[0]) {
        fail(r, "GenericError", "There already is a medium in device '%s'", mon->media[i].name);
        return GEMU_MON_NONE;
    }
    snprintf(t->file, sizeof(t->file), "%s", mon->nodes[n].file);
    t->node = n + 1;
    mon->nodes[n].attached = i + 1;
    return GEMU_MON_NONE;
}

/* Open, remove, insert the file, close - like QEMU, but a closed tray is
 * changed in one step so the machine sees a single media change. */
static GemuMonCmd do_change(GemuMonitor *mon, MonBlockReq *r, int i) {
    GemuMediaDevice *dev = &mon->media[i];
    MonTray *t = &mon->tray[i];
    if (!removable(dev, r)) return GEMU_MON_NONE;
    if (t->open) {
        remove_medium(mon, i);
        snprintf(t->file, sizeof(t->file), "%s", r->file);
        GemuMonCmd cmd = close_tray(mon, r, i);
        if (r->klass) t->file[0] = '\0';   /* nothing left in the open tray */
        return cmd;
    }
    char err[256] = "";
    GemuMediaResult res = dev->change(dev->ud, r->file, err, sizeof(err));
    if (res == GEMU_MEDIA_ERR) {
        fail(r, "GenericError", "%s", err[0] ? err : "could not load the medium");
        return GEMU_MON_NONE;
    }
    detach_node(mon, i);
    snprintf(dev->file, sizeof(dev->file), "%s", r->file);
    return result_cmd(res);
}

GemuMonCmd mon_block_run(GemuMonitor *mon, void *ud) {
    MonBlockReq *r = ud;
    r->klass = NULL;
    switch (r->op) {
    case MON_BLOCK_ADD: return do_add(mon, r);
    case MON_BLOCK_DEL: return do_del(mon, r);
    default:            break;
    }
    int i = find_device(mon, r);
    if (i < 0) return GEMU_MON_NONE;
    switch (r->op) {
    case MON_BLOCK_OPEN_TRAY:  return open_tray(mon, r, i);
    case MON_BLOCK_CLOSE_TRAY: return close_tray(mon, r, i);
    case MON_BLOCK_REMOVE_MEDIUM:
        if (removable(&mon->media[i], r) && tray_is_open(mon, r, i))
            remove_medium(mon, i);
        return GEMU_MON_NONE;
    case MON_BLOCK_INSERT_MEDIUM: return do_insert(mon, r, i);
    case MON_BLOCK_CHANGE_MEDIUM: return do_change(mon, r, i);
    default:                      return GEMU_MON_NONE;
    }
}

void mon_block_media_changed(GemuMonitor *mon, GemuMediaDevice *dev, bool ejected) {
    int i = (int)(dev - mon->media);
    if (i < 0 || i >= mon->n_media) return;
    remove_medium(mon, i);
    mon->tray[i].open = ejected;
}
