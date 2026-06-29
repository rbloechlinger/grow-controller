# grow-controller (Stamp S3 / ESPHome)

Autonomous climate controller for a propagation / cuttings box, built on an
**M5Stack Stamp S3** running **ESPHome**. It reads temperature and humidity,
drives a heater, an exhaust fan and an ultrasonic humidifier, buffers history in
RAM, and talks to an optional **Cardputer** HMI over pure ESP-NOW.

This repository is the **controller node only**. The companion HMI firmware
lives in a separate repository:
[**grow-cardputer**](https://github.com/rbloechlinger/grow-cardputer). The two
share a wire-format header, `protocol.h`, which **must be byte-identical** in
both repos.

The controller runs fully on its own. The Cardputer is a convenience layer — if
it is switched off or out of range, the Stamp keeps regulating unaffected.

```
   SHT30 ─┐                                   ┌─ Cardputer Display + Keyboard
          │                                   │
  Heater ─┤    M5Stack Stamp S3   <─ESP-NOW─>    M5Cardputer ADV   ├─ microSD (CSV)
   Fan ───┤    (ESPHome / esp-idf)  Unicast,Ch1   (Arduino-ESP32)
  Humid. ─┤                                   
   OLED ──┘
```

---

## Highlights

- **Heating-focused control.** The ESPHome `climate` component regulates a
  heater relay with hysteresis. Its mode doubles as the single global mode:
  `HEAT` = AUTO, `OFF` = MANUAL.
- **Exhaust fan as a shared resolver.** One PWM fan handles both humidity
  venting and an over-temperature safety vent, with an 80 % purge burst that
  settles to a 20 % trickle.
- **Pulsed humidifier** with a forced rest window, so the box cannot overshoot
  the slow sensor into a humidify/vent oscillation.
- **Two isolated I²C buses** so a stuck OLED can never drag the sensor down.
- **On-demand OTA over WiFi**, triggered from the Cardputer, with automatic
  return to ESP-NOW and AUTO afterwards.
- **CI/CD**: a git tag builds the firmware and publishes a versioned manifest to
  GitHub Pages that the Stamp can pull over the air.

---

## Hardware

| Component | Detail |
|---|---|
| Controller | M5Stack Stamp S3 (ESP32-S3) + Stamp S3 Grove Breakout (SKU A144) |
| Sensor | SHT30 (ENV III), I²C `sht3xd` @ `0x44`, `update_interval: 30s` |
| Display | SH1107 128×128 OLED, I²C `ssd1306_i2c` @ `0x3C` |
| Heater | relay on `GPIO6` (Grove3), `restore_mode: ALWAYS_OFF` |
| Fan | 5 V PWM exhaust fan, LEDC on `GPIO4` (Grove2 IO1), 25 kHz |
| Humidifier | 5 V ultrasonic mist module via IRF520 MOSFET, gate on `GPIO10` |
| Buttons | M5 Dual Button Unit (Grove5): blue `GPIO9`, red `GPIO7` (inverted) |
| Liveness | onboard WS2812 on `GPIO21` (heartbeat, optional/commented) |

### Pin map and I²C buses

| Pin | Function | Notes |
|---|---|---|
| `GPIO13 / GPIO15` | `bus_sensor` SDA / SCL | SHT30 alone, Grove6, **100 kHz** |
| `GPIO1 / GPIO2` | `bus_display` SDA / SCL | OLED alone, Grove1, **400 kHz** |
| `GPIO4` | Fan PWM | Grove2 IO1; **`GPIO3` left unconnected (strapping pin)** |
| `GPIO6` | Heater relay | Grove3 |
| `GPIO10` | Humidifier (IRF520 gate) | Grove G10/G11 SIG (yellow) |
| `GPIO9 / GPIO7` | Blue / red button | Grove5, inverted |

### Hardware notes & gotchas (ESP32-S3 / Stamp S3)

- **Strapping pins** `GPIO0`, `GPIO3`, `GPIO45`, `GPIO46` must stay free. This is
  why the fan PWM is on `GPIO4` only and there is no tachometer input.
- **Two isolated buses on purpose.** The SHT30 and OLED used to share one Grove6
  bus; a hung OLED could stall the sensor. They now live on separate I²C
  controllers, which fixes the class of failure structurally.
- **Bus speed:** the OLED bus runs at 400 kHz now that it is isolated; the
  sensor bus runs at 100 kHz.
- **Power & seating:** Grove ports carry 5 V but the S3 GPIOs are 3.3 V only.
  Feed I²C peripherals from the Stamp's **3V3** pin, not the Grove 5 V rail. A
  loose Stamp in the A144 breakout shows up as phantom 3.3 V on VCC and silent
  I²C dropouts — seat it firmly before debugging in software.
- **SDA/SCL silk:** the project has a history of swapped silkscreen on Grove
  ports; when assigning a new port, try swapping SDA/SCL first if the bus scan
  finds nothing.

---

## Control logic

The system boots into **MANUAL** with both actuators off (the climate is forced
to `OFF` in `on_boot`, and every switch is `restore_mode: ALWAYS_OFF`). It only
starts regulating once it receives `CMD_SET_AUTO` (the Cardputer `a` key, or
holding both local buttons). The one exception is an OTA reboot — see
[OTA](#ota-over-the-air-updates).

### Heater (AUTO only)

The `climate` component runs bang-bang hysteresis around the target. Defaults:
heat **ON below 24 °C**, **OFF at/above 28 °C** (28 °C target / cuttings ceiling,
4 °C dead band). The climate does not drive the relay directly — it sets a
`temp_heat_demand` flag and `resolve_heater` applies it, gated so MANUAL keeps
ownership of the relay. In MANUAL the heater stays off.

### Fan — one resolver, two jobs

A single function, `resolve_fan_pwm`, owns the PWM fan so two control paths can
never fight over it. In AUTO it runs the fan when **humidity venting** OR an
**over-temperature** condition is active:

- **Humidity venting** (hysteresis): vent at/above `g_vent_high` (default
  **95 % RH**), stop at `g_vent_high − vent_hyst` (default **90 % RH**). RH must
  stay above the threshold for `g_vent_delay_s` (default **300 s**) before
  venting starts — this rides out post-burst overshoot and sensor noise.
- **Over-temperature safety:** exhaust whenever the box exceeds
  `temp_vent_high` (default **32 °C**), independent of mode.

Every vent cycle (AUTO or manual) runs the same profile: an **80 % purge for
5 s**, then a **20 % trickle** for the rest of the cycle. A 1 s interval drives
the timed transition.

### Humidifier — pulsed

In AUTO the humidifier is pulsed, not run continuously, because the box
humidifies faster than the sensor registers (overshoot). Below `g_hum_low`
(default **80 % RH**) it runs a burst of `hum_burst_s` (**30 s**), then forces
off for a `hum_rest_s` (**60 s**) settle window before re-evaluating. A NaN
reading forces it off (dead-sensor fail-safe). A 15-minute hard cap only guards
against a forgotten manual ON.

### Anti-oscillation invariant

The wide gap between "stop venting" (90 %) and "start humidifying" (80 %) is the
dead zone that keeps the box from cycling vent → dry → humidify → wet → vent.

> **Invariant:** `g_vent_high − vent_hyst` must stay clearly above `g_hum_low`,
> otherwise the dead zone collapses and the loop returns.

> **Sensor note:** around 80–90 % RH the sensor is near the edge of its accuracy.
> If venting triggers early or late, suspect the reading before the thresholds.

All thresholds are exposed as `substitutions` at the top of `growbox.yaml` and
most are runtime-adjustable globals (`g_hum_low`, `g_vent_high`,
`g_vent_delay_s`, climate target) that the Cardputer can set live.

### Local buttons (Dual Button Unit)

The blue/red buttons give manual control without the Cardputer: they cycle the
manual actuator state and trigger venting, and **holding both returns to AUTO**.
Manual venting from the buttons uses the same 80 % → 20 % purge profile as AUTO.

---

## ESP-NOW protocol (`protocol.h`, v7)

Transport is **pure ESP-NOW**, fixed **channel 1**, unicast between the Stamp and
the Cardputer's MAC. `protocol.h` here must be **byte-identical** to
`grow-cardputer/include/protocol.h`. `PROTOCOL_VERSION` is bumped only on an
incompatible wire change; mismatched versions silently drop each other's frames.

| Message | Direction | Payload |
|---|---|---|
| `MSG_TELEMETRY` (0x01) | Stamp → Cardputer | temp, hum, fan_on, mode, hum_on, **heat_on**, uptime_s, live setpoints — **31 bytes** |
| `MSG_COMMAND` (0x02) | Cardputer → Stamp | command id + float argument — 6 bytes |
| `MSG_BACKLOG` (0x03) | Stamp → Cardputer | history chunk (≤ 28 samples); an empty chunk marks end-of-transfer |

| Command | Effect |
|---|---|
| `CMD_SET_TARGET` (0x01) | set the climate target (°C) |
| `CMD_TOGGLE_VENT` (0x02) | toggle the fan → enters MANUAL |
| `CMD_SET_AUTO` (0x03) | hand both outputs back to AUTO |
| `CMD_GET_BACKLOG` (0x04) | "send all samples newer than this Stamp uptime" |
| `CMD_TOGGLE_HUM` (0x05) | toggle the humidifier → enters MANUAL |
| `CMD_REFRESH` (0x06) | force an out-of-cycle sensor read |
| `CMD_SET_TARGET_RH` (0x07) | set the humidify-below threshold (% RH) |
| `CMD_SET_VENT_RH` (0x08) | set the venting-above threshold (% RH) |
| `CMD_SET_VENT_DELAY` (0x09) | set the vent on-delay (seconds) |
| `CMD_HELLO` (0x0A) | Cardputer announces itself on link-up; forces a fresh read so the reply carries live values + setpoints at once |
| `CMD_OTA` (0x0B) | open an on-demand WiFi OTA window on the Stamp |

Telemetry is sent after every sensor read **and** after every executed command,
so the Cardputer gets instant feedback. The frame carries the live setpoints
(`target_temp`, `hum_low`, `vent_high`, `vent_delay_s`), so the Cardputer
self-heals its SETTINGS display after a reboot instead of drifting.

**v7** added `heat_on` to the telemetry frame (30 → 31 bytes) so the Cardputer
can show and log whether the heater is running right now. `FLAG_HEAT` already
carried the heater state in the backlog before v7.

### Backlog

The Stamp keeps a statically sized RAM ring buffer with a hard cap
(`BACKLOG_CAPACITY = 5760` samples × 8 bytes = 45 KB, never grows). At 30 s per
sample that is ~48 h. On reconnect the Cardputer requests everything newer than
the last uptime it knows; the Stamp streams it oldest-first in chunks. The
buffer does **not** survive a Stamp reboot.

---

## OTA (over-the-air updates)

Pure ESP-NOW has no always-on WiFi, so OTA is **on demand**. When the Stamp
receives `CMD_OTA` (from the Cardputer's FW-UPDATE row), `run_ota_window`:

1. shuts the actuators off (brownout protection during flash),
2. brings WiFi up and checks the GitHub Pages manifest
   (`firmware/manifest.json`),
3. flashes if a newer build is available, then
4. **reboots back into ESP-NOW** — the native `espnow:` component binds the radio
   only at setup, so a reboot is the clean way back.

A persisted one-shot global, `g_restore_auto_after_ota`, is set only when an OTA
window opens while in AUTO. After the deliberate OTA reboot the Stamp restores
AUTO; an ordinary brownout reboot (flag clear) still lands safely in MANUAL.

> **WiFi credentials** come from `secrets.yaml` (`wifi_ssid`, `wifi_password`)
> and are compiled **into** the firmware. Because that firmware is published to a
> public branch, the password is extractable from the `.bin` — use a dedicated
> guest/IoT SSID you don't mind being effectively public. Copy
> `secrets.yaml.example` to `secrets.yaml` for local builds; CI injects the
> values from repository secrets.

---

## CI/CD (GitHub Actions → Pages)

`.github/workflows/main.yml` builds and publishes firmware:

- **Triggers:** a `v*` tag push (release), or a manual `workflow_dispatch` on any
  branch (test build).
- **Versioning:** a tag `vX.Y.Z` builds version `X.Y.Z`, publishes to
  `firmware/X.Y.Z/`, and also refreshes the stable `firmware/` (the
  `manifest.json` the Stamp polls). A dispatch build goes to
  `firmware/branches/<branch>/` and leaves the release manifest untouched.
- **Version stamping:** the workflow `sed`s the tag version straight into
  `growbox.yaml`'s `fw_version`, making the git tag the single source of truth
  (the YAML keeps a literal default for local USB builds).
- **Secrets:** `secrets.yaml` is generated at build time from the `WIFI_SSID` /
  `WIFI_PASSWORD` repository secrets (Settings → Secrets and variables →
  Actions).
- **Publish:** artifacts are committed to `gh-pages` (accumulating; old versions
  are kept), served verbatim via `.nojekyll`.

Manifest URL polled by the Stamp:
`https://rbloechlinger.github.io/grow-controller/firmware/manifest.json`

---

## Build & flash

`protocol.h` must match the Cardputer's copy. On any wire-format change
(`PROTOCOL_VERSION` bump), **reflash both devices** — mixed versions show a blank
Cardputer until both are updated.

### Local (USB)

```bash
esphome clean growbox.yaml
esphome run   growbox.yaml     # build + flash over USB
esphome logs  growbox.yaml     # serial logs
```

If the venv symlinks break after a toolchain upgrade:

```bash
rm -rf ~/.platformio/penv
```

### Release (OTA via CI)

```bash
git add growbox.yaml protocol.h
git commit -m "…"
git tag v6.5.0
git push origin main --tags     # CI builds + publishes the manifest
```

Then trigger the OTA from the Cardputer (SETTINGS → FW UPDATE → confirm), and the
Stamp pulls the new build from Pages.

### First-time setup

```text
1) Flash the Cardputer's mac_printer once, read its STA MAC over serial (115200).
2) Put that MAC in growbox.yaml -> substitutions.cardputer_mac
   (and in the CARD_MAC constant inside the CMD_GET_BACKLOG lambda).
3) Set the Stamp's STA MAC in grow-cardputer/src/link.cpp (stampMac[]).
```

---

## Files

```text
growbox.yaml                 ESPHome config: sensor, OLED, actuators, control logic, ESP-NOW, OTA
protocol.h                   shared wire format (MUST match grow-cardputer/include/protocol.h)
secrets.yaml.example         template for the OTA WiFi credentials (copy to secrets.yaml)
.github/workflows/main.yml   CI: build on tag, publish manifest to GitHub Pages
ARCHITEKTUR.md               design notes
README.md                    this file
```

---

## Key defaults

| Setting | Substitution | Default |
|---|---|---|
| Climate target / ceiling | `default_target_temperature_high` | 28 °C |
| Heat-on edge | (target − dead band) | 24 °C |
| Humidify below | `humidity_low` | 80 % RH |
| Vent above | `humidity_vent_high` | 95 % RH |
| Vent hysteresis | `vent_hyst` | 5 % RH (stop at 90 %) |
| Vent on-delay | `vent_delay_s` | 300 s |
| Fan purge | `fan_purge_pct` / `fan_purge_s` | 80 % for 5 s |
| Fan trickle | `fan_trickle_pct` | 20 % |
| Over-temp vent | `temp_vent_high` | 32 °C |
| Humidifier burst / rest | `hum_burst_s` / `hum_rest_s` | 30 s / 60 s |
| ESP-NOW channel | `espnow_channel` | 1 |