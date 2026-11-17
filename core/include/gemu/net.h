#pragma once
#include "args.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * QEMU-style networking: host backends (-netdev), guest network adapters
 * (-device MODEL,netdev=ID, -nic, the legacy -net nic / -net user hub) and
 * the wiring between them.
 *
 *   -netdev user,id=n0[,hostfwd=tcp::2222-:22,...]   libslirp NAT, DHCP, DNS
 *   -netdev tap,id=n0,ifname=tap0,script=no          host TAP interface (Linux)
 *   -device i82559,netdev=n0[,mac=52:54:00:12:34:56][,id=lan]
 *   -nic user[,model=i82559][,mac=...][,hostfwd=...]  netdev + adapter in one
 *   -nic none                                        no adapters at all
 *   -net nic[,model=M][,macaddr=...][,vlan=N]        legacy hub (vlan = hub id)
 *   -net user[,vlan=N]                               ...and its backend
 *
 * With none of these a machine that has network adapters gets its default
 * one on a user-mode (slirp) backend, as QEMU does.
 *
 * Frames from a backend are queued per adapter on the backend's thread; the
 * emulated device polls them from the emulator thread with gemu_nic_recv().
 */

typedef struct GemuNic GemuNic;

/* Command-line parsing.  Called by gemu_args_parse() (-net/-nic/-netdev) and
 * by a family's setup for -device MODEL,... when MODEL is one of its network
 * adapters.  All print their own error and return false on bad input. */
bool gemu_net_parse_netdev(const char *spec);
bool gemu_net_parse_nic(const char *spec);
bool gemu_net_parse_net(const char *spec);
bool gemu_net_parse_device(const char *spec);
/* True when anything other than "none" was asked for. */
bool gemu_net_requested(void);
/* Name before the first comma of a -device spec, for the caller's model check. */
bool gemu_net_device_model(const char *spec, char *model, size_t cap);

/*
 * Resolve the command line against this machine's adapter models and start
 * the backends.  `models` lists what -device/-nic/-net nic may name (NULL /
 * 0: the machine has none, and any adapter request is an error);
 * `default_model` is what an adapter without model= becomes and what the
 * machine gets when nothing was asked for (NULL: no default adapter, e.g.
 * -nodefaults).  On success *nics points at *n adapters in command-line
 * order; the array stays valid for the process.  "model=help" lists the
 * models and exits.
 */
bool gemu_net_resolve(const GemuDevDesc *models, int n_models,
                      const char *default_model, GemuNic ***nics, int *n);
/* Stop the backend thread and free the backends. */
void gemu_net_shutdown(void);

const char *gemu_nic_model(const GemuNic *nic);
const char *gemu_nic_name(const GemuNic *nic);
void        gemu_nic_mac(const GemuNic *nic, uint8_t mac[6]);
/* One frame (no FCS) from the guest onto the wire.  Dropped while the link
 * is down or nothing is attached; always "sent" as far as the device knows. */
bool        gemu_nic_send(GemuNic *nic, const uint8_t *frame, size_t len);
/* Poll one received frame (no FCS, padded to 60 bytes) into `frame`; returns
 * its length, 0 when none is waiting. */
size_t      gemu_nic_recv(GemuNic *nic, uint8_t *frame, size_t cap);
/* The cable: false after "set_link NAME off". */
bool        gemu_nic_link_up(const GemuNic *nic);
/* Called on the emulator thread when the monitor changes the link. */
void        gemu_nic_set_link_cb(GemuNic *nic, void (*cb)(void *ud, bool up),
                                 void *ud);

/* Monitor support: "info network" and "set_link NAME on|off" (NAME is an
 * adapter or a netdev; either side takes the whole connection down, as in
 * QEMU).  set_link returns false for an unknown name. */
void gemu_net_info(void (*print)(void *ud, const char *line), void *ud);
bool gemu_net_set_link(const char *name, bool up);
