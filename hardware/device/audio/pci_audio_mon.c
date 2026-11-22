/* PCI audio: monitor output (see pci_audio.h).  Separate from the device so
 * that it links without the monitor. */
#include "pci_audio.h"
#include "gemu/monitor.h"
#include <inttypes.h>

void pci_audio_monitor_print(GemuMonitor *mon, PciAudio *a) {
    gemu_monitor_printf(mon, "audio: %s command=%04x BAR0=%08x BAR1=%08x irq=%u\n",
        pci_audio_name(a->model), pci_audio_cfg_read(a, 4, 2),
        pci_audio_cfg_read(a, 0x10, 4), pci_audio_cfg_read(a, 0x14, 4), a->irq_level);
    gemu_monitor_printf(mon, "  frames=%" PRIu64 " nonzero=%" PRIu64
        " dma_bytes=%" PRIu64 " interrupts=%" PRIu64 " master=%04x pcm=%04x\n",
        a->frames, a->nonzero_frames, a->dma_bytes, a->interrupts,
        a->codec[1], a->codec[0x18 / 2]);
    if (a->model == PCI_AUDIO_CS4281) {
        for (unsigned c = 0; c < 4; c++)
            gemu_monitor_printf(mon, "  dma%u mode=%08x control=%08x address=%08x count=%08x fifo=%08x\n",
                c, a->u.cs.reg[(0x150 + c * 8) / 4], a->u.cs.reg[(0x154 + c * 8) / 4],
                a->u.cs.reg[(0x110 + c * 16) / 4], a->u.cs.reg[(0x114 + c * 16) / 4],
                a->u.cs.reg[(0x180 + c * 4) / 4]);
    } else if (a->model == PCI_AUDIO_AC97) {
        for (unsigned c = 0; c < 3; c++) {
            const IchAudioDma *d = &a->u.ich.dma[c];
            gemu_monitor_printf(mon, "  dma%u bdl=%08x civ=%u lvi=%u remaining=%u status=%04x control=%02x\n",
                c, d->bdbar, d->civ, d->lvi, d->remaining, d->status, d->command);
        }
    }
}
