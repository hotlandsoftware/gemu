#pragma once
/*
 * An IDE (ATA) channel: the cable with up to two devices, device 0 and
 * device 1, behind one set of task-file registers.
 *
 *   reg 0       data (16- or 32-bit accesses move that many bytes)
 *   reg 1-6     error/features, sector count, LBA low/mid/high, device
 *   reg 7       status (read, acknowledges the interrupt) / command (write)
 *   reg 8       alternate status (read) / device control (write)
 *
 * As on the cable: task-file writes reach every device present (each keeps
 * its own copy), commands and data go to the selected device (the device
 * register's DEV bit), device control reaches both, and the selected device
 * drives INTRQ.  With the selected device absent the channel floats: status
 * and the other registers read 0, except the device register, which reads
 * back what was written.
 *
 * Devices implement IdeDevice as the first member of their own struct.
 */
#include <stdbool.h>
#include <stdint.h>

typedef struct IdeDevice IdeDevice;
struct IdeDevice {
    uint64_t (*data_read)(IdeDevice *d, unsigned size);
    void     (*data_write)(IdeDevice *d, uint64_t val, unsigned size);
    /* reg 1-6 and 7 (status; ack: the read acknowledges, as from the
     * command block, not alternate status) */
    uint8_t  (*reg_read)(IdeDevice *d, unsigned reg, bool ack);
    void     (*reg_write)(IdeDevice *d, unsigned reg, uint8_t val);   /* 1-6 */
    void     (*command)(IdeDevice *d, uint8_t cmd);
    void     (*control)(IdeDevice *d, uint8_t val);
    bool     (*intrq)(const IdeDevice *d);
};

typedef struct {
    IdeDevice *dev[2];      /* NULL: no device at that position */
    uint8_t    device;      /* the device register as last written (DEV: bit 4) */
    uint8_t    control;     /* device control as last written */
} IdeChannel;

void     ide_reset(IdeChannel *c);       /* device 0 selected */
uint64_t ide_read(IdeChannel *c, unsigned reg, unsigned size);
void     ide_write(IdeChannel *c, unsigned reg, uint64_t val, unsigned size);
/* The selected device: NULL if absent. */
IdeDevice *ide_selected(const IdeChannel *c);
bool     ide_intrq(const IdeChannel *c);
