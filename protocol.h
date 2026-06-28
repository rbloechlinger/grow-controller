/*
 * protocol.h -- shared ESP-NOW wire format for the growbox link.
 *
 * This file MUST be identical in both project folders:
 *   - the ESPHome folder next to growbox.yaml (Stamp S3)
 *   - cardputer/include/ in the Cardputer PlatformIO project
 *
 * v6.1 changes (OTA trigger): new command CMD_OTA opens an on-demand WiFi
 *   update window on the Stamp. Wire structs are UNCHANGED, so this is backward
 *   compatible and does NOT bump PROTOCOL_VERSION (same rule as v5.1/v5.3/v5.4);
 *   both devices need this build for the feature to do anything.
 *
 * v6 changes (setpoint echo + startup handshake):
 *   - TelemetryMsg now carries the live setpoints (target_temp, hum_low,
 *     vent_high, vent_delay_s). The Stamp is the single source of truth for
 *     these, so echoing them in every frame keeps the Cardputer's display and
 *     SETTINGS page correct -- it self-heals after a Cardputer reboot instead
 *     of drifting until the user nudges a value. The packet grew 16 -> 30
 *     bytes: this is an INCOMPATIBLE wire change, so PROTOCOL_VERSION bumps to
 *     6 and BOTH devices must be reflashed (mixed versions silently drop each
 *     other's frames).
 *   - new command CMD_HELLO: the Cardputer announces itself on link-up so the
 *     Stamp forces an out-of-cycle DHT read and replies with a fresh frame
 *     (incl. the setpoints) at once, instead of waiting up to 30 s for the next
 *     poll. Functionally it is CMD_REFRESH with startup semantics; the Stamp
 *     handles both in one case.
 *
 * v5.5 changes (vent on-delay): set vent on-delay to 300
 *
 * v5.4 changes (vent on-delay): new command CMD_SET_VENT_DELAY sets how long RH
 *   must stay above the venting threshold before the fan starts venting -- rides
 *   out post-burst overshoot and DHT11 noise. Wire structs UNCHANGED (backward
 *   compatible); both devices need this build for the feature to work.
 *
 * v5.3 changes (RH setpoints): two new commands let the Cardputer set the
 *   humidity control thresholds at runtime -- CMD_SET_TARGET_RH (humidify-below)
 *   and CMD_SET_VENT_RH (venting-above). Wire structs are UNCHANGED, so this is
 *   backward compatible; an old Stamp just logs them as unknown. Both devices
 *   need this build for the feature to do anything.
 *
 * v5.1 changes (manual refresh):
 *   - new command CMD_REFRESH: the Cardputer asks the Stamp for an out-of-cycle
 *     DHT read so live values update on demand instead of waiting for the 30 s
 *     poll. Wire structs are UNCHANGED, so this is backward compatible: an old
 *     Stamp just logs an unknown command and ignores it. No reflash is forced,
 *     but both devices need this build for the feature to do anything.
 *
 * v5 changes (unified control mode):
 *   - fan and humidifier no longer have separate auto/manual modes -- there is
 *     now ONE global control mode. TelemetryMsg.hum_mode was removed, so the
 *     packet shrank from 17 to 16 bytes. BOTH devices must be reflashed with
 *     this protocol.h; mixed versions silently drop each other's frames.
 *   - any manual fan OR humidifier action puts the whole system into MANUAL;
 *     CMD_SET_AUTO hands BOTH outputs back to automatic control.
 *
 * v4 (humidifier): TelemetryMsg gained hum_on; FLAG_HUM added to the backlog.
 * v3 (backlog): TelemetryMsg carries the Stamp uptime so the Cardputer can
 *   deduplicate; the Stamp keeps a HARD-CAPPED RAM ring buffer and replays it
 *   via CMD_GET_BACKLOG / MSG_BACKLOG chunks, oldest first, terminated by an
 *   empty chunk.
 */
#pragma once
#include <stdint.h>

// ---- Wire-compatibility version --------------------------------------------
// Bump ONLY when the on-wire structs change incompatibly (as v5 did: 17 -> 16
// bytes; as v6 does: 16 -> 30 bytes). A pure feature add that leaves the structs
// untouched (new command IDs like v5.1/v5.3/v5.4) does NOT bump this -- those
// ride along on unchanged frames and are tracked by the per-device firmware
// versions instead. Identical PROTOCOL_VERSION on both ends therefore means
// "the frames line up"; a mismatch means reflash both.
#define PROTOCOL_VERSION 6

// ---- Message types (first byte on the wire) --------------------------------
static const uint8_t MSG_TELEMETRY = 0x01;  // Stamp -> Cardputer (live)
static const uint8_t MSG_COMMAND   = 0x02;  // Cardputer -> Stamp
static const uint8_t MSG_BACKLOG   = 0x03;  // Stamp -> Cardputer (history chunk)

// ---- Command IDs (CommandMsg.command) ---------------------------------------
static const uint8_t CMD_SET_TARGET     = 0x01; // value = fan ON threshold (deg C)
static const uint8_t CMD_TOGGLE_VENT    = 0x02; // toggle fan, ENTERS global manual
static const uint8_t CMD_TOGGLE_HUM     = 0x05; // toggle humidifier, ENTERS global manual
static const uint8_t CMD_SET_AUTO       = 0x03; // hand BOTH outputs back to auto
static const uint8_t CMD_GET_BACKLOG    = 0x04; // value = "send samples newer than this Stamp uptime (s)"
static const uint8_t CMD_REFRESH        = 0x06; // force an out-of-cycle DHT read (value unused)
static const uint8_t CMD_SET_TARGET_RH  = 0x07; // value = humidify-below thr (%RH)
static const uint8_t CMD_SET_VENT_RH    = 0x08; // value = venting-above thr (%RH)
static const uint8_t CMD_SET_VENT_DELAY = 0x09; // value = vent on-delay (seconds)
static const uint8_t CMD_HELLO          = 0x0A; // Cardputer announces itself on link-up:
                                                // forces a fresh DHT read so the reply carries live values + setpoints
                                                // at once (value unused)
static const uint8_t CMD_OTA            = 0x0B; // open an on-demand WiFi OTA window on the Stamp (value unused):
                                                // the Stamp shuts its actuators, brings WiFi up, checks the GitHub
                                                // Pages manifest, flashes if newer, then reboots back to ESP-NOW.
                                                // Pure command add -> CommandMsg unchanged -> NO PROTOCOL_VERSION bump
                                                // (an old Stamp would just log it as unknown and ignore it).

// ---- Control mode (TelemetryMsg.mode) -- now the SINGLE global mode ----------
static const uint8_t MODE_MANUAL = 0;
static const uint8_t MODE_AUTO   = 1;

// ---- Backlog sample flags ----------------------------------------------------
static const uint8_t FLAG_FAN    = 0x01;
static const uint8_t FLAG_AUTO   = 0x02;
static const uint8_t FLAG_HUM    = 0x04;
static const uint8_t FLAG_HEAT   = 0x08;  // backlog only: relay/heater on (pure add, no PROTOCOL_VERSION bump)
static const uint8_t HUM_INVALID = 255;

// ---- Backlog sizing -----------------------------------------------------------
// HARD memory cap: the store is a statically sized ring buffer. At one sample
// per DHT reading (30 s) this holds ~48 hours; older samples are overwritten.
// 5760 samples x 8 bytes = 45 KB of RAM on the Stamp -- fixed, never grows.
static const uint16_t BACKLOG_CAPACITY = 5760;

// Max samples per MSG_BACKLOG chunk: 4-byte header + 28 x 8 = 228 <= 250 (ESP-NOW limit)
static const uint8_t  BACKLOG_MAX_PER_PKT = 28;

#pragma pack(push, 1)

// Stamp -> Cardputer, sent after every DHT reading and after every command.
typedef struct {
  uint8_t  type;         // MSG_TELEMETRY
  float    temperature;  // deg C
  float    humidity;     // % RH
  uint8_t  fan_on;       // 0/1, current fan relay state
  uint8_t  mode;         // GLOBAL control mode: MODE_MANUAL / MODE_AUTO
  uint8_t  hum_on;       // 0/1, current humidifier state
  uint32_t uptime_s;     // Stamp uptime in seconds (backlog dedup reference)
  float    target_temp;  // climate target_high = fan ON threshold (deg C)
  float    hum_low;      // g_hum_low: humidify-below threshold (% RH)
  float    vent_high;    // g_vent_high: venting-above threshold (% RH)
  uint16_t vent_delay_s; // g_vent_delay_s: vent on-delay (seconds)
} TelemetryMsg;          // 30 bytes on the wire

// Cardputer -> Stamp, keyboard-triggered or automatic (backlog request / hello).
typedef struct {
  uint8_t type;     // MSG_COMMAND
  uint8_t command;  // CMD_*
  float   value;    // command argument
} CommandMsg;       // 6 bytes on the wire

// One buffered sample (also the wire format inside MSG_BACKLOG chunks).
typedef struct {
  uint32_t t_s;       // Stamp uptime when sampled (seconds)
  int16_t  temp_c10;  // temperature * 10 (e.g. 24.6 C -> 246)
  uint8_t  hum;       // humidity %, HUM_INVALID if unknown
  uint8_t  flags;     // FLAG_FAN | FLAG_AUTO | FLAG_HUM
} BacklogSample;      // 8 bytes

// MSG_BACKLOG chunk layout on the wire:
//   [0] = MSG_BACKLOG
//   [1] = sample count in this chunk (0 = end-of-transfer marker)
//   [2] = reserved
//   [3] = reserved
//   [4...] = count x BacklogSample

#pragma pack(pop)

// RAM store on the Stamp (NOT a wire struct). Statically sized -> hard cap.
typedef struct {
  BacklogSample buf[BACKLOG_CAPACITY];
  uint16_t head  = 0;   // next write position
  uint16_t count = 0;   // number of valid samples (saturates at capacity)
} BacklogStore;

// Accessor for the single store instance. Defined here (instead of an
// ESPHome `globals:` entry) because ESPHome emits its globals declarations
// BEFORE the user includes in main.cpp -- the type would not be visible yet.
// The function-local static is only emitted in translation units that call
// it, so the Cardputer build pays no RAM for this.
inline BacklogStore &backlogStore() {
  static BacklogStore store;
  return store;
}