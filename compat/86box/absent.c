/*
 * GEMU's 86Box layer: what GEMU doesn't have, for the drivers that refer to
 * it (one source file holds every model of a card, the Sound Blaster's from
 * the 1.0 to the AWE64).  Each of these either does what hardware with
 * nothing attached does - said so where it does - or stops GEMU with a
 * message: reached only by a model GEMU doesn't offer.
 */
#include "compat_priv.h"
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/gameport.h>
#include <86box/hdc.h>
#include <86box/hdc_ide.h>
#include <86box/isapnp.h>
#include <86box/midi.h>
#include <86box/sound.h>
#include <86box/timer.h>
#include <86box/snd_azt2316a.h>
#include <86box/snd_azt2320.h>
#include <86box/snd_emu8k.h>
#include <86box/snd_mpu401.h>

/* ── Nothing attached: as the hardware would be ───────────────────────────── */

/* Nothing on the MIDI port: what goes out is lost, nothing comes in. */
void midi_raw_out_byte(uint8_t val) { (void)val; }
void midi_clear_buffer(void) {}
void midi_in_handler(int set, void (*msg)(void *priv, uint8_t *msg, uint32_t len),
                     int (*sysex)(void *priv, uint8_t *buffer, uint32_t len, int abort),
                     void *priv) {
    (void)set; (void)msg; (void)sysex; (void)priv;
}

/* No CD audio to filter; the machine's speaker isn't mixed through a card. */
void sound_set_cd_audio_filter(void (*filter)(int channel, double *buffer, void *priv), void *priv) {
    (void)filter; (void)priv;
}
void sound_set_pc_speaker_filter(void (*filter)(int channel, double *buffer, void *priv), void *priv) {
    (void)filter; (void)priv;
}

int other_ide_present;
int isarom_type[4];          /* 86Box's settings (ISAROM_MAX): GEMU's are -device */

/* ── Models GEMU doesn't offer: these stop it ─────────────────────────────── */

#define ABSENT(what) fatal("%s: not in GEMU's 86Box layer yet\n", what)

const device_t gameport_device         = { .name = "Game port", .internal_name = "gameport" };
const device_t gameport_200_device     = { .name = "Game port (200h)", .internal_name = "gameport_200" };
const device_t gameport_pnp_device     = { .name = "Game port (PnP)", .internal_name = "gameport_pnp" };
const device_t gameport_pnp_1io_device = { .name = "Game port (PnP, 1 port)", .internal_name = "gameport_pnp_1io" };
const device_t ide_qua_pnp_device      = { .name = "IDE (PnP)", .internal_name = "ide_qua_pnp" };

void *gameport_add(const device_t *t) { ABSENT(t ? t->name : "a game port"); }
void  gameport_remap(void *priv, uint16_t address) { (void)priv; (void)address; ABSENT("a game port"); }

void *isapnp_add_card(uint8_t *rom, uint16_t rom_size,
                      void (*config_changed)(uint8_t ld, isapnp_device_config_t *config, void *priv),
                      void (*csn_changed)(uint8_t csn, void *priv),
                      uint8_t (*read_vendor_reg)(uint8_t ld, uint8_t reg, void *priv),
                      void (*write_vendor_reg)(uint8_t ld, uint8_t reg, uint8_t val, void *priv),
                      void *priv) {
    (void)rom; (void)rom_size; (void)config_changed; (void)csn_changed;
    (void)read_vendor_reg; (void)write_vendor_reg; (void)priv;
    ABSENT("ISA Plug and Play");
}

void ide_set_base_addr(int board, int base, uint16_t port) { (void)board; (void)base; (void)port; ABSENT("IDE"); }
void ide_set_irq(int board, int irq) { (void)board; (void)irq; ABSENT("IDE"); }
void ide_handlers(uint8_t board, int set) { (void)board; (void)set; ABSENT("IDE"); }
void ide_pnp_config_changed(uint8_t ld, isapnp_device_config_t *config, void *priv) {
    (void)ld; (void)config; (void)priv; ABSENT("IDE");
}
void ide_pnp_config_changed_1addr(uint8_t ld, isapnp_device_config_t *config, void *priv) {
    (void)ld; (void)config; (void)priv; ABSENT("IDE");
}

void wavetable_add_handler(void (*get_buffer)(int32_t *buffer, uint16_t len, void *priv), void *priv) {
    (void)get_buffer; (void)priv; ABSENT("wavetable sound (the EMU8000)");
}
void emu8k_init(emu8k_t *emu8k, uint16_t emu_addr, int onboard_ram) {
    (void)emu8k; (void)emu_addr; (void)onboard_ram; ABSENT("the EMU8000 (AWE32)");
}
void emu8k_change_addr(emu8k_t *emu8k, uint16_t emu_addr) { (void)emu8k; (void)emu_addr; ABSENT("the EMU8000"); }
void emu8k_close(emu8k_t *emu8k) { (void)emu8k; ABSENT("the EMU8000"); }
void emu8k_update(emu8k_t *emu8k) { (void)emu8k; ABSENT("the EMU8000"); }

void    mpu401_init(mpu_t *mpu, uint16_t addr, int irq, int mode, int receive_input) {
    (void)mpu; (void)addr; (void)irq; (void)mode; (void)receive_input; ABSENT("the MPU-401");
}
void    mpu401_setirq(mpu_t *mpu, int irq) { (void)mpu; (void)irq; ABSENT("the MPU-401"); }
void    mpu401_change_addr(mpu_t *mpu, uint16_t addr) { (void)mpu; (void)addr; ABSENT("the MPU-401"); }
void    mpu401_irq_attach(mpu_t *mpu, void (*ext_irq_update)(void *priv, int set),
                          int (*ext_irq_pending)(void *priv), void *priv) {
    (void)mpu; (void)ext_irq_update; (void)ext_irq_pending; (void)priv; ABSENT("the MPU-401");
}
uint8_t MPU401_ReadData(mpu_t *mpu) { (void)mpu; ABSENT("the MPU-401"); }
int     MPU401_InputSysex(void *priv, uint8_t *buffer, uint32_t len, int abort) {
    (void)priv; (void)buffer; (void)len; (void)abort; ABSENT("the MPU-401");
}
void    MPU401_InputMsg(void *priv, uint8_t *msg, uint32_t len) {
    (void)priv; (void)msg; (void)len; ABSENT("the MPU-401");
}

void azt2316a_enable_wss(uint8_t enable, void *priv) { (void)enable; (void)priv; ABSENT("the Aztech cards"); }
void azt2320_enable_wss(uint8_t enable, void *priv) { (void)enable; (void)priv; ABSENT("the Aztech cards"); }
void aztpr16_update_mixer(void *priv) { (void)priv; ABSENT("the Aztech cards"); }
void aztpr16_wss_mode(uint8_t mode, void *priv) { (void)mode; (void)priv; ABSENT("the Aztech cards"); }
void azt1605_update_mixer(void *priv) { (void)priv; ABSENT("the Aztech cards"); }
