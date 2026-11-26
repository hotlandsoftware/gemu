/*
 * GEMU's 86Box layer: 86Box (v6.0) device drivers as core devices.
 * (.work/modular-devices, section 3; README.md here)
 *
 * A port (compat/86box/ports/<name>.c) includes the driver's source from
 * third_party/86box/v6.0 unchanged and makes a GemuDeviceType of its
 * device_t:
 *
 *     #include "../../../third_party/86box/v6.0/src/device/novell_cardkey.c"
 *     #include "gemu_86box.h"
 *     GEMU_86BOX_PORT(gemu_86box_novellkeycard_type, novell_keycard_device,
 *                     "novellkeycard", NULL, NULL, true);
 *
 * plus a data/device XML naming the symbol, like any device type.  Because
 * the port is the driver's translation unit, its state hook can see the
 * driver's private structure and register its fields (gemu/dev/state.h);
 * the device's timers (timer_add) register themselves.  A driver whose
 * private data is all set up from its options passes stateless = true.
 *
 * The device:
 *   - options: one per entry of the driver's config table (int, bool for
 *     CONFIG_BINARY, a choice of the values for selections - "0x220",
 *     "5" - and of the BIOS names, strings and files), with its defaults;
 *   - init: device_t.init(), with the device's options behind
 *     device_get_config_*(); then the port's state hook;
 *   - reset: device_t.reset() on a power-on or hard reset (a soft one only
 *     for DEVICE_SOFTRESET devices); destroy: device_t.close(), and its
 *     I/O handlers come off;
 *   - output lines irq0..irq15 (pic.h), which the machine connects to its
 *     PIC (gemu_86box_connect_irqs);
 *   - its I/O handlers go in the machine's "io" space (io.h);
 *   - its timers count in clocks of the device's clock (gemu_device_clock:
 *     the bus's), which is 86Box's tsc.  All 86Box devices in a machine
 *     share one: tsc and TIMER_USEC are globals in 86Box.
 *   - device_add() from its init (a sub-device: the AdLib's OPL) makes a
 *     child core device of the device_t's port, started at once;
 *   - its sound and music handlers are core streams (<86box/sound.h>),
 *     brought up to date before a save so what they generate lazily (an
 *     OPL's chip) is saved at the right moment.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "gemu/dev/device.h"
#include "gemu/dev/line.h"
#include "gemu/dev/option.h"

typedef struct _device_ device_t;           /* <86box/device.h> */
typedef struct Gemu86BoxIo Gemu86BoxIo;
typedef struct Gemu86BoxTimer Gemu86BoxTimer;
typedef struct Gemu86BoxSound Gemu86BoxSound;

#define GEMU_86BOX_MAX_OPTIONS 32

typedef struct Gemu86BoxPort {
    const device_t *dev;
    const char     *type_name;              /* = dev->internal_name (checked) */
    /* Register the driver's state (priv: what its init returned). */
    bool          (*state)(GemuDevice *d, void *priv);
    /* After a load: what the state implies (moved ports, ...). */
    void          (*post_load)(GemuDevice *d, void *priv);
    GemuOptionDef  *options;                /* GEMU_86BOX_MAX_OPTIONS + 1, filled at startup */
    const GemuDeviceType *type;
} Gemu86BoxPort;

/* Every port, found by its device_t (device_add); linked at startup. */
typedef struct Gemu86BoxPortLink {
    const Gemu86BoxPort      *port;
    struct Gemu86BoxPortLink *next;
} Gemu86BoxPortLink;

/* A port device's priv. */
typedef struct Gemu86BoxDev {
    const Gemu86BoxPort *port;
    GemuDevice     *dev;
    void           *priv;                   /* the driver's */
    GemuLine       *irq[16];
    Gemu86BoxIo    *io;                     /* its handlers */
    Gemu86BoxTimer *timers;
    unsigned        n_timers;
    bool            in_init;
    struct Gemu86BoxDev *parent86;          /* the device whose device_add made it */
    Gemu86BoxSound *sound;                  /* its sound/music handlers (sound.h) */
    device_t       *init_dev;               /* device_add_params(): the device_t it got */
} Gemu86BoxDev;

bool gemu_86box_init(GemuDevice *d, const Gemu86BoxPort *p);
void gemu_86box_reset(GemuDevice *d, GemuResetKind kind);
void gemu_86box_destroy(GemuDevice *d);
void gemu_86box_describe(GemuDevice *d, char *buf, size_t len);
void gemu_86box_post_load(GemuDevice *d);
void gemu_86box_pre_save(GemuDevice *d);
void gemu_86box_make_options(const Gemu86BoxPort *p);
void gemu_86box_register(Gemu86BoxPortLink *l);

/* Connect d's irq<n> to the input pattern with n in it ("board/pic:ir%u"),
 * for n < count.  False (the machine's error says why) on a failure. */
bool gemu_86box_connect_irqs(GemuDevice *d, const char *in_fmt, unsigned count);

/* The driver's private data, for a machine or a test that looks inside. */
void *gemu_86box_priv(GemuDevice *d);

#if defined(__GNUC__) || defined(__clang__)
#  define GEMU_86BOX_CTOR __attribute__((constructor))
#else
#  error "GEMU's 86Box layer needs constructor functions (GCC or Clang)"
#endif

#define GEMU_86BOX_PORT(sym, devt, tname, state_fn, post_load_fn, is_stateless)          \
    extern const GemuDeviceType sym;                                                    \
    static GemuOptionDef sym##_options[GEMU_86BOX_MAX_OPTIONS + 1];                     \
    static const Gemu86BoxPort sym##_port = {                                           \
        &(devt), (tname), (state_fn), (post_load_fn), sym##_options, &sym };            \
    static Gemu86BoxPortLink sym##_link = { &sym##_port, 0 };                           \
    static bool sym##_init(GemuDevice *d) { return gemu_86box_init(d, &sym##_port); }   \
    GEMU_86BOX_CTOR static void sym##_ctor(void) {                                      \
        gemu_86box_make_options(&sym##_port);                                           \
        gemu_86box_register(&sym##_link);                                               \
    }                                                                                   \
    const GemuDeviceType sym = {                                                        \
        .name = (tname), .desc = "86Box v6.0 device", .size = sizeof(Gemu86BoxDev),     \
        .options = sym##_options, .stateless = (is_stateless),                          \
        .init = sym##_init, .reset = gemu_86box_reset, .destroy = gemu_86box_destroy,   \
        .describe = gemu_86box_describe, .pre_save = gemu_86box_pre_save,               \
        .post_load = gemu_86box_post_load,                                              \
    }
