/*
 * GEMU's 86Box layer: I/O handlers (<86box/io.h>) on the machine's "io"
 * space - one SHARED mapping per port, so 86Box's stacking (all writes,
 * ANDed reads) is the space's.
 */
#include "compat_priv.h"
#include <86box/86box.h>
#include <86box/io.h>
#include "gemu/dev/space.h"
#include <stdlib.h>

typedef uint8_t  (*InB)(uint16_t, void *);
typedef uint16_t (*InW)(uint16_t, void *);
typedef uint32_t (*InL)(uint16_t, void *);
typedef void     (*OutB)(uint16_t, uint8_t, void *);
typedef void     (*OutW)(uint16_t, uint16_t, void *);
typedef void     (*OutL)(uint16_t, uint32_t, void *);

struct Gemu86BoxIo {
    Gemu86BoxDev *owner;
    uint16_t      base, size;     /* ports base..base+size-1, one mapping */
    InB inb; InW inw; InL inl; OutB outb; OutW outw; OutL outl;
    void         *priv;
    GemuMapping  *mp;
    Gemu86BoxIo  *next;          /* the owner's */
};

#define PORT(h, off) ((uint16_t)((h)->base + (off)))

static uint8_t r8(void *ud, uint64_t off) {
    Gemu86BoxIo *h = ud;
    Gemu86BoxDev *prev = gemu_86box_enter(h->owner);
    uint8_t v = h->inb(PORT(h, off), h->priv);
    gemu_86box_leave(prev);
    return v;
}
static uint16_t r16(void *ud, uint64_t off) {
    Gemu86BoxIo *h = ud;
    Gemu86BoxDev *prev = gemu_86box_enter(h->owner);
    uint16_t v = h->inw(PORT(h, off), h->priv);
    gemu_86box_leave(prev);
    return v;
}
static uint32_t r32(void *ud, uint64_t off) {
    Gemu86BoxIo *h = ud;
    Gemu86BoxDev *prev = gemu_86box_enter(h->owner);
    uint32_t v = h->inl(PORT(h, off), h->priv);
    gemu_86box_leave(prev);
    return v;
}
static void w8(void *ud, uint64_t off, uint8_t v) {
    Gemu86BoxIo *h = ud;
    Gemu86BoxDev *prev = gemu_86box_enter(h->owner);
    h->outb(PORT(h, off), v, h->priv);
    gemu_86box_leave(prev);
}
static void w16(void *ud, uint64_t off, uint16_t v) {
    Gemu86BoxIo *h = ud;
    Gemu86BoxDev *prev = gemu_86box_enter(h->owner);
    h->outw(PORT(h, off), v, h->priv);
    gemu_86box_leave(prev);
}
static void w32(void *ud, uint64_t off, uint32_t v) {
    Gemu86BoxIo *h = ud;
    Gemu86BoxDev *prev = gemu_86box_enter(h->owner);
    h->outl(PORT(h, off), v, h->priv);
    gemu_86box_leave(prev);
}

static GemuSpace *io_space(Gemu86BoxDev *b) {
    GemuSpace *s = gemu_space_find(b->dev->machine, "io");
    if (!s)
        fatal("the machine has no \"io\" space for its 86Box devices\n");
    return s;
}

/* Handlers on ports base..base+size-1 (size > 0), as one mapping. */
static void add(Gemu86BoxDev *b, uint16_t base, uint16_t size, InB inb, InW inw, InL inl,
                OutB outb, OutW outw, OutL outl, void *priv) {
    Gemu86BoxIo *h = calloc(1, sizeof *h);
    if (!h)
        fatal("io_sethandler: out of memory\n");
    *h = (Gemu86BoxIo){ b, base, size, inb, inw, inl, outb, outw, outl, priv, NULL, b->io };
    GemuHandlers hd = {
        .r8 = inb ? r8 : NULL, .r16 = inw ? r16 : NULL, .r32 = inl ? r32 : NULL,
        .w8 = outb ? w8 : NULL, .w16 = outw ? w16 : NULL, .w32 = outl ? w32 : NULL,
    };
    if (!(h->mp = gemu_space_map(io_space(b), base, size, &hd, h, 0, GEMU_MAP_SHARED)))
        fatal("io_sethandler: ports %04X-%04X aren't in the io space\n", base,
              (unsigned)(base + size - 1));
    gemu_mapping_set_name(h->mp, b->dev->path);
    b->io = h;
}

/* step 1: one mapping for the range, so a word access to a port reaches
 * its inw/outw; step > 1 (interleaved registers): a mapping per port. */
void io_sethandler_common(uint16_t base, uint16_t size, InB inb, InW inw, InL inl,
                          OutB outb, OutW outw, OutL outl, void *priv, uint8_t step) {
    Gemu86BoxDev *b = gemu_86box_need("io_sethandler");
    if (!size)
        return;
    if (step <= 1) {
        add(b, base, size, inb, inw, inl, outb, outw, outl, priv);
        return;
    }
    for (uint32_t c = 0; c < size; c += step)
        add(b, (uint16_t)(base + c), 1, inb, inw, inl, outb, outw, outl, priv);
}

/* On port, the first handler with exactly these functions and priv comes
 * off: its range is split around the port. */
static void remove_one(Gemu86BoxDev *b, uint16_t port, InB inb, InW inw, InL inl,
                       OutB outb, OutW outw, OutL outl, void *priv) {
    for (Gemu86BoxIo **pp = &b->io; *pp; pp = &(*pp)->next) {
        Gemu86BoxIo *h = *pp;
        if (port < h->base || port - h->base >= h->size || h->inb != inb || h->inw != inw ||
            h->inl != inl || h->outb != outb || h->outw != outw || h->outl != outl ||
            h->priv != priv)
            continue;
        uint16_t lo = h->base, hi = (uint16_t)(h->base + h->size - 1);
        *pp = h->next;
        gemu_mapping_remove(h->mp);
        free(h);
        if (port > lo)
            add(b, lo, (uint16_t)(port - lo), inb, inw, inl, outb, outw, outl, priv);
        if (port < hi)
            add(b, (uint16_t)(port + 1), (uint16_t)(hi - port), inb, inw, inl, outb, outw, outl,
                priv);
        return;
    }
}

void io_removehandler_common(uint16_t base, uint16_t size, InB inb, InW inw, InL inl,
                             OutB outb, OutW outw, OutL outl, void *priv, uint8_t step) {
    Gemu86BoxDev *b = gemu_86box_need("io_removehandler");
    if (!step)
        step = 1;
    for (uint32_t c = 0; c < size; c += step)
        remove_one(b, (uint16_t)(base + c), inb, inw, inl, outb, outw, outl, priv);
}

void io_handler_common(uint8_t set, uint16_t base, uint16_t size, InB inb, InW inw, InL inl,
                       OutB outb, OutW outw, OutL outl, void *priv, uint8_t step) {
    if (set)
        io_sethandler_common(base, size, inb, inw, inl, outb, outw, outl, priv, step);
    else
        io_removehandler_common(base, size, inb, inw, inl, outb, outw, outl, priv, step);
}

void io_sethandler(uint16_t base, uint16_t size, InB inb, InW inw, InL inl,
                   OutB outb, OutW outw, OutL outl, void *priv) {
    io_sethandler_common(base, size, inb, inw, inl, outb, outw, outl, priv, 1);
}

void io_removehandler(uint16_t base, uint16_t size, InB inb, InW inw, InL inl,
                      OutB outb, OutW outw, OutL outl, void *priv) {
    io_removehandler_common(base, size, inb, inw, inl, outb, outw, outl, priv, 1);
}

void io_handler(uint8_t set, uint16_t base, uint16_t size, InB inb, InW inw, InL inl,
                OutB outb, OutW outw, OutL outl, void *priv) {
    io_handler_common(set, base, size, inb, inw, inl, outb, outw, outl, priv, 1);
}

void gemu_86box_io_free(Gemu86BoxDev *b) {
    while (b->io) {
        Gemu86BoxIo *h = b->io;
        b->io = h->next;
        gemu_mapping_remove(h->mp);
        free(h);
    }
}
