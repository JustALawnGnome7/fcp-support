// SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <alsa/asoundlib.h>
#include <alsa/sound/tlv.h>

#include "uapi-fcp.h"
#include "fcp.h"
#include "meter.h"
#include "log.h"

/* Upper bound on a peak-index, so a typo in a hand-written map is still caught. Well above any
 * plausible slot count; the Clarett 2Pre's array is 48. */
#define METER_SLOT_LIMIT 128

/* Highest peak-index in a sources/destinations array, or -1 if it has none. */
static int max_peak_index(struct json_object *entries) {
  int max = -1;

  for (int i = 0; i < json_object_array_length(entries); i++) {
    struct json_object *entry = json_object_array_get_idx(entries, i);
    struct json_object *peak_index;

    if (!json_object_object_get_ex(entry, "peak-index", &peak_index))
      continue;

    int idx = json_object_get_int(peak_index);

    if (idx > max)
      max = idx;
  }

  return max;
}

static int add_meter_mapping_info(struct fcp_device *device, int map_size, char **labels) {
  struct fcp_meter_labels *fcp_labels;

  // Calculate the total size of the labels
  unsigned int labels_size = 0;
  for (int i = 0; i < map_size; i++)
    labels_size += strlen(labels[i]) + 1;

  fcp_labels = calloc(sizeof(*fcp_labels) + labels_size, 1);
  if (!fcp_labels) {
    log_error("Cannot allocate TLV memory");
    return -ENOMEM;
  }

  fcp_labels->labels_size = labels_size;

  // Copy the labels into fcp_labels->labels
  char *data = fcp_labels->labels;
  for (int i = 0; i < map_size; i++) {
    strcpy(data, labels[i]);
    data += strlen(labels[i]) + 1;
  }

  // Set the TLV
  int err = snd_hwdep_ioctl(device->hwdep, FCP_IOCTL_SET_METER_LABELS, fcp_labels);
  if (err < 0)
    log_error("Cannot set meter labels: %s", snd_strerror(err));

  free(fcp_labels);
  return err;
}

/* Which peak-index key applies at this speed.
 *
 * The GET_METER slot array COMPACTS as ADAT S/MUX removes destinations at double and quad speed: a
 * meter's slot is the channel's position in THAT RATE's destination table, so everything after a
 * removed entry shifts down. Measured on a Clarett 8Pre -- Mixer Input 01 reads slot 40 at 48 kHz,
 * 32 at 96 kHz and 28 at 192 kHz, while the ADAT input meters below the first removal do not move at
 * all, which is why this went unnoticed. The map supplies all three layouts.
 */
static const char *peak_key_for_band(int band) {
  return band == 2 ? "peak-index-h" :
         band == 1 ? "peak-index-m" :
                     "peak-index";
}

/* Does this map carry per-rate layouts at all?
 *
 * Only the Clarett Thunderbolt maps do. Everything else resolves to the base peak-index at every
 * rate, exactly as before, and does no rate lookup at all -- so a device whose meters do not move
 * with the sample rate pays nothing for this, not even a proc read per tick.
 */
static int array_has_rate_keys(struct json_object *entries) {
  for (int i = 0; i < json_object_array_length(entries); i++) {
    struct json_object *entry = json_object_array_get_idx(entries, i);
    struct json_object *tmp;

    if (json_object_object_get_ex(entry, "peak-index-m", &tmp) ||
        json_object_object_get_ex(entry, "peak-index-h", &tmp))
      return 1;
  }
  return 0;
}

/* Current sample rate, read from the card's own proc entry -- NO device traffic.
 *
 * The snd-clarett driver publishes "rate:" at /proc/asound/cardN/clarett: it is the side that sends
 * SET_CLOCK, so it always knows, and it seeds the value from the device once at probe. Reading it here
 * costs nothing and works while the device is idle, which matters because meters are polled
 * continuously whether or not anything is streaming.
 *
 * Deliberately NOT an FCP query. The device does answer one (opcode 0x006005 on the Clarett
 * Thunderbolt line), but that would put a command on the mailbox every tick for information the host
 * already has -- and on a USB device the rate is likewise free from its stream0 proc entry. If a USB
 * device ever needs per-rate meters, read it there rather than asking the hardware.
 */
static int read_card_rate(struct fcp_device *device) {
  char path[64], line[128];
  int rate = -1;
  FILE *f;

  snprintf(path, sizeof(path), "/proc/asound/card%d/clarett", device->card_num);
  f = fopen(path, "r");
  if (!f)
    return -1;

  while (fgets(line, sizeof(line), f))
    if (sscanf(line, "rate: %d", &rate) == 1)
      break;

  fclose(f);
  return rate;
}

static int rate_to_band(int rate) {
  if (rate > 96000)
    return 2;
  if (rate > 48000)
    return 1;
  return 0;
}

/* Look up an entry's slot for this band.
 *
 * Membership is decided by the BASE peak-index so the map keeps a constant size and the meter
 * control never has to be resized (its labels stay valid too). A destination that does not exist at
 * this speed has no band key, and maps to -1, which the driver reports as zero -- silence, which is
 * exactly what a channel S/MUX has taken away should read.
 */
static int peak_index_for_band(struct json_object *entry, int band, int *idx_out) {
  struct json_object *base, *banded;

  if (!json_object_object_get_ex(entry, "peak-index", &base))
    return 0;

  if (band == 0)
    *idx_out = json_object_get_int(base);
  else if (json_object_object_get_ex(entry, peak_key_for_band(band), &banded))
    *idx_out = json_object_get_int(banded);
  else
    *idx_out = -1;

  return 1;
}

static void build_meter_map(struct fcp_device *device, int band, int with_labels) {
  struct json_object *spec, *sources, *sinks;
  struct json_object *control_sources, *control_sinks;
  int num_meter_slots;
  char **labels = NULL;
  int meter_idx = 0;
  int err;

  fcp_meter_info(device->hwdep, &num_meter_slots);

  /* Get device specification */
  if (!json_object_object_get_ex(device->devmap, "device-specification", &spec)) {
    log_error("Cannot find device specification");
    return;
  }

  /* Get sources and sinks arrays */
  if (!json_object_object_get_ex(spec, "sources", &sources) ||
      !json_object_object_get_ex(spec, "destinations", &sinks)) {
    log_error("Cannot find sources/destinations arrays");
    return;
  }

  device->meter_per_rate = array_has_rate_keys(sources) || array_has_rate_keys(sinks);

  /* Get FCP ALSA map sources/sinks */
  if (!json_object_object_get_ex(device->fam, "sources", &control_sources) ||
      !json_object_object_get_ex(device->fam, "sinks", &control_sinks)) {
    log_error("Cannot find sources/sinks in fcp-alsa-map");
    return;
  }

  /* METER_INFO is a floor, not the array size, on devices that report no slot count: the Clarett
   * Thunderbolt line answers 00 02 0c 00 (-> 2 x 12 = 24), yet a signal routed to Mixer Input 30 on
   * a 2Pre reads back at slot 47. Where the map asks for more slots than the device admits to, and
   * every index is inside the sanity limit, believe the map -- its indices are measured. Rejecting
   * them would not just drop those entries, it would discard the whole meter map below. */
  int max_idx = max_peak_index(sources);
  int sink_max = max_peak_index(sinks);

  if (sink_max > max_idx)
    max_idx = sink_max;

  if (max_idx >= num_meter_slots && max_idx < METER_SLOT_LIMIT) {
    log_info(
      "Device reports %d meter slots; map uses up to %d, extending",
      num_meter_slots, max_idx + 1
    );
    num_meter_slots = max_idx + 1;
  }

  /* Allocate maximum possible size */
  int map_size = json_object_array_length(control_sources) +
                 json_object_array_length(control_sinks);

  int16_t *meter_map = calloc(map_size, sizeof(int16_t));

  labels = calloc(map_size, sizeof(char *));
  if (!meter_map || !labels) {
    log_error("Cannot allocate meter map/label map");
    err = -ENOMEM;
    goto done;
  }

  /* Map sources */
  for (int i = 0; i < json_object_array_length(control_sources); i++) {
    struct json_object *control_source = json_object_array_get_idx(control_sources, i);
    struct json_object *device_name, *alsa_name;

    if (!json_object_object_get_ex(control_source, "device_name", &device_name) ||
        !json_object_object_get_ex(control_source, "alsa_name", &alsa_name)) {
      log_error("Control source missing device_name/alsa_name");
      err = -1;
      goto done;
    }

    /* Find matching source in device map */
    const char *name = json_object_get_string(device_name);
    const char *alsa = json_object_get_string(alsa_name);

    for (int j = 0; j < json_object_array_length(sources); j++) {
      struct json_object *source = json_object_array_get_idx(sources, j);
      struct json_object *source_name;

      if (!json_object_object_get_ex(source, "name", &source_name))
        continue;

      if (strcmp(json_object_get_string(source_name), name))
        continue;

      {
        int idx;

        if (peak_index_for_band(source, band, &idx)) {
          if (idx >= num_meter_slots) {
            log_error("Invalid peak index %d", idx);
            err = -1;
            goto done;
          }

          meter_map[meter_idx] = idx;

          if (with_labels &&
              asprintf(&labels[meter_idx], "Source %s", alsa) < 0) {
            log_error("Cannot allocate label");
            err = -ENOMEM;
            goto done;
          }

          meter_idx++;
        }
      }
      break;
    }
  }

  /* Map sinks */
  for (int i = 0; i < json_object_array_length(control_sinks); i++) {
    struct json_object *control_sink = json_object_array_get_idx(control_sinks, i);
    struct json_object *device_name, *alsa_name;

    if (!json_object_object_get_ex(control_sink, "device_name", &device_name) ||
        !json_object_object_get_ex(control_sink, "alsa_name", &alsa_name)) {
      log_error("Control sink missing device_name/alsa_name");
      err = -1;
      goto done;
    }

    /* Find matching sink in device map */
    const char *name = json_object_get_string(device_name);
    const char *alsa = json_object_get_string(alsa_name);

    for (int j = 0; j < json_object_array_length(sinks); j++) {
      struct json_object *sink = json_object_array_get_idx(sinks, j);
      struct json_object *sink_name;

      if (!json_object_object_get_ex(sink, "name", &sink_name))
        continue;

      if (strcmp(json_object_get_string(sink_name), name))
        continue;

      {
        int idx;

        if (peak_index_for_band(sink, band, &idx)) {
          if (idx >= num_meter_slots) {
            log_error("Invalid peak index %d", idx);
            err = -1;
            goto done;
          }

          meter_map[meter_idx] = idx;

          if (with_labels &&
              asprintf(&labels[meter_idx], "Sink %s", alsa) < 0) {
            log_error("Cannot allocate label");
            err = -ENOMEM;
            goto done;
          }

          meter_idx++;
        }
      }

      break;
    }
  }

  if (meter_idx == 0) {
    log_error("No meters found");
    err = -1;
    goto done;
  }

  /* Configure meter mapping */
  struct fcp_meter_map *map = calloc(
    1, sizeof(struct fcp_meter_map) + map_size * sizeof(int16_t)
  );
  map->meter_slots = num_meter_slots;
  map->map_size = meter_idx;
  memcpy(map->map, meter_map, meter_idx * sizeof(int16_t));

  {
    char s[1024] = "Meter map:";
    char *p = s + strlen(s);
    for (int i = 0; i < meter_idx; i++)
      p += sprintf(p, " %d", meter_map[i]);
    log_debug("%s", s);
    log_debug("Meter slots: %d", map->meter_slots);
    log_debug("Map size: %d", map->map_size);
  }

  /* Send meter map to driver */
  err = snd_hwdep_ioctl(device->hwdep, FCP_IOCTL_SET_METER_MAP, map);
  if (err < 0)
    log_error("Cannot set meter map: %s", snd_strerror(err));

  /* Add mapping info control. Only on the initial build: the mapped entry set is the same at every
   * speed (absent channels map to -1 rather than dropping out), so the labels never change. */
  if (with_labels) {
    err = add_meter_mapping_info(device, meter_idx, labels);
    if (err < 0)
      goto done;
  }

done:
  if (labels) {
    for (int i = 0; i < meter_idx; i++)
      free(labels[i]);
    free(labels);
  }
  free(meter_map);
}

void add_meter_control(struct fcp_device *device) {
  device->meter_band = 0;
  device->meter_per_rate = 0;
  build_meter_map(device, 0, 1);

  /* Correct to whatever speed the device is at right now, but only for a map that has the layouts. */
  if (device->meter_per_rate)
    meter_poll_rate(device);
}

/* Re-map the meters if the device has changed speed band. Cheap: one FCP query, and a rebuild only
 * when the band actually changes. Called from the main loop's idle tick -- there is no notification
 * for a rate change, and the rate can move without any control event (a DAW opening a stream). */
void meter_poll_rate(struct fcp_device *device) {
  int rate, band;

  /* No per-rate layouts in this map: nothing to switch between, and no reason to query the rate. */
  if (!device->meter_per_rate)
    return;

  rate = read_card_rate(device);
  if (rate <= 0)
    return;

  band = rate_to_band(rate);
  if (band == device->meter_band)
    return;

  log_debug("Sample rate now %d, remapping meters (band %d -> %d)",
            rate, device->meter_band, band);
  device->meter_band = band;
  build_meter_map(device, band, 0);
}
