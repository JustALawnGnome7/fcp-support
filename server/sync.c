// SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "device-ops.h"
#include "log.h"

static const char *sync_enum_names[] = { "Unlocked", "Locked" };
static const int sync_enum_count = 2;

static int read_sync_control(
  struct fcp_device    *device,
  struct control_props *props,
  int                  *value
) {
  int new_value = fcp_sync_read(device->hwdep);

  if (new_value < 0) {
    log_error("Failed to read sync status: %s", strerror(-new_value));
    return new_value;
  }

  /* The Clarett Thunderbolt and Red word is a bitfield: bit 0 = locked, bit 1 = the sync state
   * changed since the last read (latched by a clock event, cleared by reading it). Measured on a
   * Red 8Line: idle on Internal reads 1, an external source with no signal reads 0, and the first
   * read after any stream start reads 3 (or 2 when unlocked) and the next 1 (or 0). Collapsing the
   * word to "non-zero" therefore showed an unlocked device as Locked whenever that latch was set.
   * These cards have no USB product ID; USB devices keep the original non-zero test. */
  *value = device->usb_pid ? !!new_value : new_value & 1;
  return 0;
}

void add_sync_control(struct fcp_device *device) {
  struct control_props props = {
    .name          = "Sync Status",
    .interface     = SND_CTL_ELEM_IFACE_MIXER,
    .type          = SND_CTL_ELEM_TYPE_ENUMERATED,
    .category      = CATEGORY_SYNC,
    .enum_names    = (char **)sync_enum_names,
    .enum_count    = sync_enum_count,
    .read_only     = 1,
    .notify_client = 8,
    .notify_device = 0,
    .offset        = 0,
    .value         = 0,
    .read_func     = read_sync_control,
    .write_func    = NULL
  };

  add_control(device, &props);
}
