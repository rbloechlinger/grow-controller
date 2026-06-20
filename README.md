# Growbox Controller

Autonome Klimasteuerung für eine Growbox/Anzuchtbox auf Basis von zwei M5Stack-ESP32-S3-Knoten, gekoppelt über reines ESP-NOW (kein WiFi).

- **Controller-Node** — M5Stack Stamp S3 mit ESPHome: liest die Sensorik, regelt Lüfter und Befeuchter und puffert die Messwerte.
- **HMI-Node** — M5Stack Cardputer ADV, Firmware auf dem Arduino-Framework (PlatformIO): Display, Tastatur, Live-Trend und SD-Logging.

Der Controller läuft vollständig autonom. Das HMI ist eine optionale Schicht — fällt der Cardputer aus, regelt der Stamp unbeeinflusst weiter.

---

## Architektur

```
   DHT11 ─┐                                  ┌─ M5Cardputer Display + Keyboard
          │                                  │
   Fan ───┤   M5Stack Stamp S3   <──ESP-NOW──>   M5Cardputer ADV   ├─ microSD (CSV-Log)
          │   (ESPHome/esp-idf)   Unicast, Ch1   (Arduino-ESP32)
 Humid. ──┘                                  
```

- **Transport:** Pure ESP-NOW, fester Kanal 1, Unicast zwischen zwei bekannten MACs. Kein WiFi, kein OTA → Flashen ausschließlich über USB.
- **Wire-Format:** gemeinsame `protocol.h`, die in **beiden** Projektordnern **identisch** sein muss.
- **Globaler Modus:** Es gibt genau einen Steuermodus, `AUTO` oder `MANUAL`, für das ganze System. Die Climate-Komponente ist die einzige Quelle der Wahrheit (`COOL` = AUTO, `OFF` = MANUAL).

---

## Hardware

| Komponente | Detail |
|---|---|
| Controller | M5Stack Stamp S3 (ESP32-S3) + StampS3 Grove Breakout (SKU A144) |
| HMI | M5Stack Cardputer ADV (ESP32-S3, 240×135 Display, Tastatur, microSD) |
| Sensor | DHT11 an `GPIO1`, `update_interval: 30s` (DHT22 als Upgrade möglich) |
| Lüfter | Relais an `GPIO6` |
| Befeuchter | 5 VDC Mist-Modul via IRF520-MOSFET an `GPIO10` |
| Liveness | onboard WS2812 an `GPIO21` (Heartbeat, im YAML auskommentiert) |

### Pin- und Hardware-Fallen (ESP32-S3 / Stamp S3)

- Strapping-Pins `G0`, `G3`, `G45`, `G46` nicht für Sensoren/Aktoren verwenden.
- `GPIO19`/`GPIO20` sind die nativen USB-Leitungen → freihalten.
- ADC2 (`GPIO11–20`) ist mit aktivem Radio unbrauchbar → bei Bedarf ADC1 (`GPIO1–10`) nutzen.
- Grove-Ports liefern 5 V, aber die S3-GPIOs sind 3,3-V-only. DHT und I²C-Peripherie deshalb vom **3V3-Pin** des Stamps speisen, **nicht** vom Grove-VCC-Rail.
- `USB_SERIAL_JTAG`-Logging: zuverlässig kommt nur das Boot-Banner über `esphome logs`; Runtime-Logs nach `setup()` sind unzuverlässig — als Liveness-Anzeige eignet sich der WS2812-Heartbeat.

---

## Build & Flash

`protocol.h` muss in beiden Ordnern dieselbe Version sein. Bei jeder Protokolländerung **beide** Geräte neu flashen.

### Stamp S3 (ESPHome, esp-idf)

```bash
esphome clean growbox.yaml
esphome run   growbox.yaml     # flasht über USB
esphome logs  growbox.yaml     # nur Boot-Banner zuverlässig
```

Bei kaputten venv-Symlinks nach einem Toolchain-Upgrade:

```bash
rm -rf ~/.platformio/penv
```

### Cardputer (PlatformIO)

Die HMI-Firmware ist in Module unter `cardputer/src/` aufgeteilt (siehe *Dateien*).
Build und Flash aus dem `cardputer/`-Ordner:

```bash
cd cardputer
pio run            # build
pio run -t upload  # build + flash über USB
pio device monitor # serielle Ausgabe @115200
```

- Board: `esp32-s3-devkitc-1` (generisches ESP32-S3; es gibt keine dedizierte
  Cardputer-Board-ID). Konfiguration steht in `platformio.ini`.
- Libraries: `m5stack/M5Cardputer` aus der Registry (zieht M5Unified und M5GFX
  transitiv). Version ≥ 1.1.0.
- `protocol.h` liegt in `cardputer/include/` und muss identisch zur `protocol.h`
  beim Stamp sein.
- Flashen über USB überschreibt den bmorcelli-Launcher. Für den Launcher
  stattdessen `.pio/build/m5stack-cardputer/firmware.bin` ablegen.

Erstmalige MAC-Ermittlung:

```text
1) mac_printer/mac_printer.ino flashen, MAC über Serial (115200) ablesen
2) MAC in growbox.yaml -> substitutions.cardputer_mac eintragen
   (und in der CARD_MAC-Konstante im CMD_GET_BACKLOG-Lambda)
3) cardputer/ flashen; stampMac[] in cardputer/src/link.cpp auf die
   STA-MAC des Stamps setzen
```

---

## Bedienung (Cardputer-Tasten)

| Taste     | Funktion                                                |
|-----------|---------------------------------------------------------|
| `w` / `s` | Zieltemperatur ±0,5 °C (AUTO-Schwelle)                  |
| `SPACE`   | Lüfter manuell umschalten → wechselt in MANUAL          |
| `h`       | Befeuchter manuell umschalten → wechselt in MANUAL      |
| `a`       | zurück zu AUTO (Lüfter **und** Befeuchter)              |
| `r`       | manueller Sensor-Refresh (außerplanmäßiges DHT-Reading) |
| `v`       | Seite vor                                               |
| `,` / `/` | Wert hoch / Wert runter                                 |
| `;` / `.` | nur History-Seite: ältere / neuere Session              |

### Seiten

1. **DASH** — große Temperatur/Feuchte-Werte, Zielwert, FAN/HUM/Mode-Chips
2. **TREND** — Live-Temperaturverlauf (Ringpuffer) mit Schwellen
3. **HISTORY** — alte Sessions von der microSD durchblättern

---

## Regellogik (AUTO)

Boot-Zustand: das System startet in **MANUAL**, beide Ausgänge AUS (`restore_mode: ALWAYS_OFF`). Es regelt erst, sobald `a` (CMD_SET_AUTO) gedrückt wird.

### Lüfter — ein Besitzer für zwei Aufgaben

Der Lüfter dient sowohl der Temperatur als auch der Entfeuchtung. Damit nicht zwei Regler um dasselbe Relais kämpfen, besitzt **`resolve_fan`** als einzige Stelle das Relais: Lüfter AN, wenn *Temperatur zu hoch* **ODER** *Feuchte zu hoch*.

- **Temperatur** (bang-bang): Lüfter AN ≥ `28 °C`, AUS ≤ `24 °C` (Ziel 28 °C, Totband 4 °C).
- **Venting** (Feuchte-Hysterese): Lüfter AN ≥ `88 % RH`, AUS ≤ `83 % RH`.

### Befeuchter — gepulst

In AUTO wird der Befeuchter **gepulst**, nicht durchlaufend, weil die Box schneller befeuchtet als der DHT11 es registriert (Überschwingen). Bei `RH < 78 %` und freiem Ausgang läuft ein Burst von `30 s`, danach erzwungenes Aus und ein `60 s` Settle-Fenster, in dem die Hysterese nicht erneut auslösen darf. Ein NaN-Reading erzwingt Aus (Fail-Safe). Eine 15-Minuten-Obergrenze sichert nur ein vergessenes manuelles EIN ab.

### Anti-Oszillation

Der Schlüssel ist die Totzone zwischen „Lüften aus" (83 %) und „Befeuchten an" (78 %): 5 % Puffer, und zum erneuten Lüften muss die Feuchte erst wieder bis 88 % steigen. Damit kann das System nicht in lüften → trocken → befeuchten → feucht → lüften kippen.

> **Invariante:** `humidity_vent_stop` muss deutlich über `humidity_low` bleiben, sonst kollabiert die Totzone und der Loop kommt zurück.

> **Sensor-Hinweis:** Der DHT11 liegt um 80–90 % RH am Rand seiner Genauigkeit (±5 %). Löst das Venting zu früh/spät aus, ist das oft eher der Sensor als die Schwellen — ein DHT22 wäre hier das eigentliche Upgrade.

Alle Schwellen sind als `substitutions` oben in `growbox.yaml` einstellbar.

---

## ESP-NOW-Protokoll (`protocol.h`)

| Nachricht | Richtung | Inhalt |
|---|---|---|
| `MSG_TELEMETRY` (0x01) | Stamp → Cardputer | Temp, Feuchte, Fan, Mode, Humidifier, Uptime — 16 Byte |
| `MSG_COMMAND` (0x02) | Cardputer → Stamp | Command + float-Argument — 6 Byte |
| `MSG_BACKLOG` (0x03) | Stamp → Cardputer | History-Chunk (≤ 28 Samples), leerer Chunk = Ende |

| Command | Wirkung |
|---|---|
| `CMD_SET_TARGET` (0x01) | Lüfter-EIN-Schwelle setzen (Totband trailt) |
| `CMD_TOGGLE_VENT` (0x02) | Lüfter umschalten, wechselt in MANUAL |
| `CMD_SET_AUTO` (0x03) | beide Ausgänge zurück in AUTO |
| `CMD_GET_BACKLOG` (0x04) | „schick mir alle Samples neuer als diese Uptime" |
| `CMD_TOGGLE_HUM` (0x05) | Befeuchter umschalten, wechselt in MANUAL |
| `CMD_REFRESH` (0x06) | außerplanmäßiges DHT-Reading erzwingen |

Telemetrie wird nach jedem DHT-Reading **und** nach jedem ausgeführten Command gesendet (Instant-Feedback). `CMD_REFRESH` ist abwärtskompatibel: ein alter Stamp loggt es nur als „unknown command".

### Backlog

Der Stamp hält einen statisch dimensionierten RAM-Ringpuffer mit harter Obergrenze (`BACKLOG_CAPACITY = 5760` Samples × 8 Byte = 45 KB, wächst nie). Bei 30 s/Sample sind das ~48 h. Reconnectet der Cardputer, fordert er alle neueren Samples an; der Stamp streamt sie ältest-zuerst in Chunks. Der Puffer überlebt keinen Stamp-Reboot.

---

## SD-Logging (Cardputer)

Pro Boot wird eine neue Session-Datei `/growbox/log_NNNN.csv` angelegt:

```text
t_s,temp_c,hum_pct,fan,mode,humid
```

`t_s` kann **negativ** sein — das sind aus dem Backlog nachgefüllte Samples, die vor dem aktuellen Cardputer-Start aufgenommen wurden. Alte 5-Spalten-Dateien laden weiterhin (der History-Parser liest nur zwei Spalten).

---

## Dateien

```text
growbox.yaml                  ESPHome-Konfiguration des Stamp S3 (Sensorik, Regelung, ESP-NOW)
protocol.h                    gemeinsames Wire-Format (MUSS mit cardputer/include/protocol.h identisch sein)
cardputer/                    PlatformIO-Projekt der HMI-Firmware
  platformio.ini              Board, Flags, Library-Abhängigkeiten
  include/protocol.h          identische Kopie von protocol.h
  src/config.h                Konstanten: Pins, ESP-NOW-Kanal, Farben, Geometrie
  src/state.{h,cpp}           geteilter Laufzeit-State (Telemetrie, Sollwert, Trend-Ringpuffer)
  src/link.{h,cpp}            ESP-NOW: Setup, RX-Callback, sendCommand (stampMac hier)
  src/backlog.{h,cpp}         RAM-History-Replay (Enqueue im Callback, Drain im Loop)
  src/storage.{h,cpp}         SD-Sessions, CSV-Logging, History-Browsing
  src/ui.{h,cpp}              Sprite-Renderer: DASH / TREND / HISTORY
  src/main.cpp                setup()/loop(), Tastatur-Orchestrierung
mac_printer/mac_printer.ino   Wegwerf-Sketch: druckt die Cardputer-STA-MAC
README.md                     diese Datei
```

Die Firmware war zuvor ein einzelner Arduino-Sketch (`cardputer.ino`) und wurde
in die obigen Module aufgeteilt. Querliegender State (Telemetrie, Sollwert,
Trend-Ringpuffer) liegt bewusst zentral in `state.*`; modul-privater State
bleibt `static` in der jeweiligen `.cpp`. Abhängigkeiten verlaufen gerichtet:
`link → backlog → storage → state`, `ui → {storage, backlog, state}`,
`main` orchestriert alle.

---

## Bekannte Einschränkungen & TODO

- **Backlog-Flood beim Boot:** `lastKnownUptime` startet bei jedem Cardputer-Boot bei 0, der Cardputer fordert also den **kompletten** Stamp-Backlog an. Aktuell unkritisch, aber bei langer Stamp-Uptime ein wachsender Daten-Burst direkt nach dem Start. Möglicher Fix: nur ein begrenztes Zeitfenster anfordern.
- **Stromversorgung:** Ein schwacher Cardputer-Akku kann beim Radio-Init einen Brownout auslösen und in eine sich beschleunigende Reset-Schleife führen. Diagnose immer zuerst per Serial: `rst:0x... (BROWNOUT_RST)` = Strom, `Guru Meditation` = Software.
- **DHT11 am RH-Limit** (siehe Sensor-Hinweis oben).
- **Keine Persistenz der Setpoints** über einen Stamp-Reboot hinweg (Climate fällt auf die `default_target_*` zurück).
- **Geplant:** Bewässerungssteuerung (Pumpe + Drip), Kamera-Integration (OV5647 am Pi / optional ESP32-CAM).

---

## Diagnose-Quickref

| Symptom | Erster Schritt |
|---|---|
| Cardputer crasht / Reset-Schleife | Serial @115200, Reset-Ursache beim Boot lesen; testweise an USB statt Akku |
| Keine Telemetrie (`RX --`) | MACs in `growbox.yaml` und `cardputer/src/link.cpp` prüfen, Kanal 1 auf beiden Seiten |
| Stamp-Logs nach `setup()` fehlen | erwartetes Verhalten bei `USB_SERIAL_JTAG`; WS2812-Heartbeat als Liveness nutzen |
| Geräte „sehen" sich nicht nach Protokolländerung | beide neu flashen — `protocol.h` muss identisch sein |