/*
 * 86Box v6.0's device.h for GEMU's 86Box layer (compat/86box).
 *
 * The configuration constants, flags and structures are v6.0's, so drivers'
 * device_t and device_config_t tables compile unchanged.  The functions are
 * the ones the layer implements: a port's GemuDeviceType (gemu_86box.h)
 * runs device_t.init/close/reset, and the device_get_config_*() calls read
 * the GEMU device's options, which are made from the config table (an
 * option per entry).  device_add() and the rest of the device manager are
 * not here yet: a driver that adds devices of its own doesn't compile.
 *
 * From 86Box (src/include/86box/device.h), GPLv2 or later:
 *   Copyright 2017-2019 Fred N. van Kempen.  Copyright 2016-2019 Miran Grca.
 *   Copyright 2008-2019 Sarah Walker.  Copyright 2021 Andreas J. Reichel.
 *   Copyright 2021-2025 Jasmine Iwanek.
 */
#ifndef EMU_DEVICE_H
#define EMU_DEVICE_H
#include <stdint.h>

#define CONFIG_END         -1                          /* N/A */

#define CONFIG_SHIFT        4

#define CONFIG_TYPE_INT     (0 << CONFIG_SHIFT)
#define CONFIG_TYPE_STRING  (1 << CONFIG_SHIFT)
#define CONFIG_TYPE_HEX16   (2 << CONFIG_SHIFT)
#define CONFIG_TYPE_HEX20   (3 << CONFIG_SHIFT)
#define CONFIG_TYPE_MAC     (4 << CONFIG_SHIFT)

#define CONFIG_INT          (0 | CONFIG_TYPE_INT)      /* config_get_int() */
#define CONFIG_BINARY       (1 | CONFIG_TYPE_INT)      /* config_get_int() */
#define CONFIG_SELECTION    (2 | CONFIG_TYPE_INT)      /* config_get_int() */
#define CONFIG_MIDI_OUT     (3 | CONFIG_TYPE_INT)      /* config_get_int() */
#define CONFIG_SPINNER      (4 | CONFIG_TYPE_INT)      /* config_get_int() */
#define CONFIG_MIDI_IN      (5 | CONFIG_TYPE_INT)      /* config_get_int() */
#define CONFIG_MEMORY       (6 | CONFIG_TYPE_INT)      /* config_get_int() */

#define CONFIG_STRING       (0 | CONFIG_TYPE_STRING)     /* config_get_string() */
#define CONFIG_FNAME        (1 | CONFIG_TYPE_STRING)     /* config_get_string() */
#define CONFIG_SERPORT      (2 | CONFIG_TYPE_STRING)     /* config_get_string() */
#define CONFIG_BIOS         (3 | CONFIG_TYPE_STRING)     /* config_get_string() */
#define CONFIG_VARIANT      CONFIG_BIOS

#define CONFIG_HEX16        (0 | CONFIG_TYPE_HEX16)      /* config_get_hex16() */

#define CONFIG_HEX20        (0 | CONFIG_TYPE_HEX20)      /* config_get_hex20() */

#define CONFIG_MAC          (0 | CONFIG_TYPE_MAC)        /* N/A */

#define CONFIG_SUBTYPE_MASK (CONFIG_IS_STRING - 1)

#define CONFIG_DEP          (16 << CONFIG_SHIFT)
#define CONFIG_TYPE_MASK    (CONFIG_DEP - 1)

// #define CONFIG_ONBOARD    256      /* only avaialble on the on-board variant */
// #define CONFIG_STANDALONE 257      /* not available on the on-board variant */

enum {
    DEVICE_CASETTE    = 1,          /* requires a Casette Port */
    DEVICE_SIDECAR    = 2,          /* requires an IBM PCjr */
    DEVICE_ISA        = 4,          /* requires the ISA bus */
    DEVICE_XT_KBC     = 8,          /* requires an XT-compatible keyboard controller */
    DEVICE_CBUS       = 0x10,       /* requires the C-BUS bus */
    DEVICE_ISA16      = 0x20,       /* requires an AT-compatible system */
    DEVICE_AT_KBC     = 0x40,       /* requires an AT-compatible keyboard controller */
    DEVICE_MCA        = 0x80,       /* requires the MCA bus */
    DEVICE_MCA32      = 0x100,      /* requires the MCA bus */
    DEVICE_PS2_KBC    = 0x200,      /* requires a PS/1 or PS/2 system */
    DEVICE_PCMCIA     = 0x400,      /* requires the PCMCIA bus */
    DEVICE_HIL        = 0x800,      /* requires the HP HIL bus */
    DEVICE_EISA       = 0x1000,     /* requires the EISA bus */
    DEVICE_AT32       = 0x2000,     /* requires the Mylex AT/32 local bus */
    DEVICE_OLB        = 0x4000,     /* requires the OPTi local bus */
    DEVICE_VLB        = 0x8000,     /* requires the VLB bus */
    DEVICE_PCI        = 0x10000,    /* requires the PCI bus */
    DEVICE_CARDBUS    = 0x20000,    /* requires the CardBus bus */
    DEVICE_USB        = 0x40000,    /* requires the USB bus */
    DEVICE_AGP        = 0x80000,    /* requires the AGP bus */
    DEVICE_AC97       = 0x100000,   /* requires the AC'97 bus */
    DEVICE_BUS        = 0x1fffff,   /* requires a machine bus */

    DEVICE_COM        = 0x200000,   /* requires a serial port */
    DEVICE_LPT        = 0x400000,   /* requires a parallel port */

    DEVICE_KBC        = 0x800000,   /* is a keyboard controller */
    DEVICE_SOFTRESET  = 0x1000000,  /* requires to be reset on soft reset */
    DEVICE_HOTPLUG_IN = 0x2000000,  /* can be safely added without a hard reset */
    DEVICE_HOTPLUG_OUT= 0x4000000,  /* can be safely closed without a hard reset */
    DEVICE_HOTPLUG    = DEVICE_HOTPLUG_IN | DEVICE_HOTPLUG_OUT,

    DEVICE_BIOS_ALIAS = 0x8000000,  /* use only BIOS names for aliases */

    DEVICE_ONBOARD    = 0x40000000, /* is on-board */
    DEVICE_PIT        = 0x80000000, /* device is a PIT */

    DEVICE_ALL        = 0xffffffff  /* match all devices */
};

#define BIOS_NORMAL                      0
#define BIOS_INTERLEAVED                 1
#define BIOS_INTERLEAVED_SINGLEFILE      2
#define BIOS_INTERLEAVED_QUAD            3
#define BIOS_INTERLEAVED_QUAD_SINGLEFILE 4
#define BIOS_INTEL_AMI                   5
#define BIOS_INTERLEAVED_INVERT          8
#define BIOS_HIGH_BIT_INVERT             16

#define BIOS_LIMIT_MIN_MEMORY            0x0100000000000000
#define BIOS_LIMIT_MAX_MEMORY            0x0200000000000000

typedef struct device_config_selection_t {
    const char *description;
    int         value;
} device_config_selection_t;

typedef struct device_config_spinner_t {
    int16_t min;
    int16_t max;
    int16_t step;
} device_config_spinner_t;

typedef struct device_config_bios_t {
    const char *name;
    const char *internal_name;
    uint8_t     bios_type;
    int8_t      files_no;
    uint32_t    local;
    uint32_t    size;
    uint64_t    flags;
    void       *dev[2];
    const char *files[9];
} device_config_bios_t;

typedef struct _device_config_ {
    const char                      *name;
    const char                      *description;
    int                              type;
    const char                      *default_string;
    int                              default_int;
    const char                      *file_filter;
    const device_config_spinner_t    spinner;
    const device_config_selection_t  selection[64];
    const device_config_bios_t       bios[32];
} device_config_t;

typedef struct _device_ {
    const char *name;
    const char *internal_name;
    uint32_t    flags; /* system flags */
    uintptr_t   local; /* flags local to device */

    union {
        void *(*init)(const struct _device_ *);
        void *(*init_ext)(const struct _device_ *, void*);
    };
    void (*close)(void *priv);
    void (*reset)(void *priv);
    int  (*available)(void);
    void (*speed_changed)(void *priv);
    void (*force_redraw)(void *priv);

    const char *alias;
    const char *machine;
    const device_config_t *config;
} device_t;

typedef struct device_context_t {
    const device_t *dev;
    char            name[2048];
    int             instance;
} device_context_t;

#ifdef __cplusplus
extern "C" {
#endif

/* The current device's option of that name (in its init, reset, close or
 * a callback of its own); 0 (or def, or "") if its config has no such
 * entry.  Selections and hex values come back as their numbers. */
extern int         device_get_config_int(const char *name);
extern int         device_get_config_int_ex(const char *str, int def);
extern int         device_get_config_hex16(const char *name);
extern int         device_get_config_hex20(const char *name);
extern const char *device_get_config_string(const char *name);
#define device_get_config_bios device_get_config_string

#ifdef __cplusplus
}
#endif

#endif /*EMU_DEVICE_H*/
