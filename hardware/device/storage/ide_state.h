#pragma once
/* Save state (gemu/dev/state.h) for the IDE models: the ATA disk, the ATAPI
 * CD-ROM, a channel and the CMD649.  prefix goes before every item name ("" for
 * none); false if any registration failed.  Only device state is
 * registered: the wiring (the IdeDevice ops, the configs, the channel's and
 * controller's pointers) is the machine's, set before a load and kept. */
#include "gemu/dev/device.h"
#include "storage/ata_disk.h"
#include "storage/atapi_cdrom.h"
#include "storage/cmd649.h"
#include "storage/ide.h"
#include <stdbool.h>

bool ata_disk_state_add(GemuDevice *d, const char *prefix, AtaDisk *x);
bool atapi_cdrom_state_add(GemuDevice *d, const char *prefix, AtapiCdrom *x);
bool ide_channel_state_add(GemuDevice *d, const char *prefix, IdeChannel *x);
bool cmd649_state_add(GemuDevice *d, const char *prefix, Cmd649 *x);

/* The CD-ROM's response buffer (data, data_len) as one custom item called
 * `name`: the length and the bytes.  A load replaces the buffer (freeing the
 * old one); a length over ATAPI_CDROM_STATE_MAX_DATA is refused.
 * atapi_cdrom_state_add() registers it as prefix + "data". */
#define ATAPI_CDROM_STATE_MAX_DATA ((uint64_t)1 << 30)
bool atapi_cdrom_state_add_data(GemuDevice *d, const char *name, AtapiCdrom *c);
