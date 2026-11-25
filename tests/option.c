/* Device options (core/src/dev/option.c).  Run: ninja check-option */
#include "gemu/dev/device.h"
#include "gemu/dev/option.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

static const char *const chips[] = { "opl2", "opl3", NULL };
static const GemuOptionDef card_opts[] = {
    { "port",   "base port",     GEMU_OPT_INT,    "0x388", 0x100, 0x3FF, NULL },
    { "irq",    "interrupt",     GEMU_OPT_INT,    "5", 2, 15, NULL },
    { "dma",    "DMA channel",   GEMU_OPT_INT,    NULL, 0, 0, NULL },
    { "stereo", "two channels",  GEMU_OPT_BOOL,   NULL, 0, 0, NULL },
    { "chip",   "which OPL",     GEMU_OPT_CHOICE, NULL, 0, 0, chips },
    { "rom",    "option ROM",    GEMU_OPT_FILE,   NULL, 0, 0, NULL },
    { "label",  "a name",        GEMU_OPT_STRING, "card", 0, 0, NULL },
    { "mem",    "memory",        GEMU_OPT_SIZE,   "64K", 0, 16 << 20, NULL },
    { NULL, NULL, 0, NULL, 0, 0, NULL },
};
static const GemuDeviceType card = { .name = "card", .options = card_opts, .stateless = true };
static const GemuDeviceType plain = { .name = "plain", .stateless = true };

/* types whose own defaults are wrong */
static const GemuOptionDef bad_def_opts[] = {
    { "irq", "", GEMU_OPT_INT, "99", 2, 15, NULL }, { NULL, NULL, 0, NULL, 0, 0, NULL } };
static const GemuDeviceType bad_def = { .name = "bad-def", .options = bad_def_opts, .stateless = true };
static const GemuOptionDef no_def_opts[] = {
    { "irq", "", GEMU_OPT_INT, NULL, 2, 15, NULL }, { NULL, NULL, 0, NULL, 0, 0, NULL } };
static const GemuDeviceType no_def = { .name = "no-def", .options = no_def_opts, .stateless = true };

static GemuMachine *m;
static int n;
static GemuDevice *add(const char *opts) {
    char name[16];
    snprintf(name, sizeof name, "c%d", n++);
    return gemu_device_add(m, &card, name, NULL, opts);
}
#define FAILS(opts, msg) do { \
    CHECK(!add(opts)); \
    if (!strstr(gemu_machine_error(m), msg)) \
        fprintf(stderr, "error was: %s\n", gemu_machine_error(m)); \
    CHECK(strstr(gemu_machine_error(m), msg)); \
} while (0)

int main(void) {
    m = gemu_machine_new("opts");

    /* defaults */
    GemuDevice *d = add(NULL);
    CHECK(d);
    CHECK(gemu_opt_int(d, "port") == 0x388 && gemu_opt_int(d, "irq") == 5 && gemu_opt_int(d, "dma") == 0);
    CHECK(!gemu_opt_bool(d, "stereo"));
    CHECK(gemu_opt_choice(d, "chip") == 0 && !strcmp(gemu_opt_str(d, "chip"), "opl2"));
    CHECK(!strcmp(gemu_opt_str(d, "rom"), "") && !strcmp(gemu_opt_str(d, "label"), "card"));
    CHECK(gemu_opt_size(d, "mem") == 65536);
    CHECK(!gemu_opt_given(d, "port") && !gemu_opt_given(d, "nope"));

    /* given */
    d = add("port=0x220,irq=7,stereo,chip=OPL3,rom=roms/sb.bin,label=a,,b,mem=2M,dma=-0");
    CHECK(d);
    CHECK(gemu_opt_int(d, "port") == 0x220 && gemu_opt_int(d, "irq") == 7);
    CHECK(gemu_opt_bool(d, "stereo") && gemu_opt_choice(d, "chip") == 1);
    CHECK(!strcmp(gemu_opt_str(d, "chip"), "opl3"));
    CHECK(!strcmp(gemu_opt_str(d, "rom"), "roms/sb.bin"));
    CHECK(!strcmp(gemu_opt_str(d, "label"), "a,b"));                  /* ",," */
    CHECK(gemu_opt_size(d, "mem") == 2u << 20);
    CHECK(gemu_opt_given(d, "port") && gemu_opt_given(d, "stereo") && gemu_opt_given(d, "rom"));
    CHECK(gemu_opt_given(d, "label") && gemu_opt_given(d, "irq"));
    CHECK(!strcmp(d->opts, "port=0x220,irq=7,stereo,chip=OPL3,rom=roms/sb.bin,label=a,,b,mem=2M,dma=-0"));
    d = add("stereo=off,irq=0xF,port=01000");                         /* octal 512 */
    CHECK(d && !gemu_opt_bool(d, "stereo") && gemu_opt_int(d, "irq") == 15 && gemu_opt_int(d, "port") == 512);
    d = add("stereo=YES,label=,mem=1k");
    CHECK(d && gemu_opt_bool(d, "stereo") && !strcmp(gemu_opt_str(d, "label"), ""));
    CHECK(gemu_opt_size(d, "mem") == 1024);

    /* errors */
    FAILS("bogus=1", "card has no option \"bogus\" (it has: port, irq, dma, stereo, chip, rom, label, mem)");
    FAILS("irq=5,irq=6", "option irq given twice");
    FAILS("port=fish", "port=fish: not a number");
    FAILS("port=0x388h", "not a number");
    FAILS("port=", "port=: not a number");
    FAILS("port", "option port needs a value");
    FAILS("irq=16", "irq=16: out of range (2 to 15)");
    FAILS("irq=1", "out of range");
    FAILS("port=0x400", "out of range (256 to 1023)");
    FAILS("stereo=maybe", "stereo=maybe: not on/off");
    FAILS("chip=opl4", "chip=opl4: not one of opl2, opl3");
    FAILS("mem=12Q", "not a size");
    FAILS("mem=-1", "not a size");
    FAILS("mem=64KB", "not a size");
    FAILS("mem=32M", "out of range");
    FAILS("irq=99999999999999999999", "not a number");
    CHECK(!gemu_device_add(m, &plain, "p", NULL, "x=1"));
    CHECK(strstr(gemu_machine_error(m), "plain has no options (\"x\" given)"));
    CHECK(gemu_device_add(m, &plain, "p", NULL, NULL));
    CHECK(!gemu_device_find(m, "c99"));
    /* empty items are ignored */
    d = add(",irq=3,");
    CHECK(d && gemu_opt_int(d, "irq") == 3);
    /* a type's bad default is reported as such */
    CHECK(!gemu_device_add(m, &bad_def, "bd", NULL, NULL));
    CHECK(strstr(gemu_machine_error(m), "default of irq=99"));
    /* no default and 0 out of range: it must be given */
    CHECK(!gemu_device_add(m, &no_def, "nd", NULL, NULL));
    CHECK(strstr(gemu_machine_error(m), "option irq must be given"));
    d = gemu_device_add(m, &no_def, "nd", NULL, "irq=4");
    CHECK(d && gemu_opt_int(d, "irq") == 4);

    /* -device specs */
    char type[32];
    const char *rest;
    CHECK(gemu_device_spec_split("adlib,port=0x388,irq=7", type, sizeof type, &rest));
    CHECK(!strcmp(type, "adlib") && !strcmp(rest, "port=0x388,irq=7"));
    CHECK(gemu_device_spec_split("adlib", type, sizeof type, &rest) && !rest);
    CHECK(!gemu_device_spec_split(",irq=7", type, sizeof type, &rest));
    CHECK(!gemu_device_spec_split("", type, sizeof type, &rest));
    CHECK(!gemu_device_spec_split("averyveryverylongtypename,x", type, 8, &rest));

    gemu_machine_free(m);
    printf("option: %u checks passed\n", checks);
    return 0;
}
