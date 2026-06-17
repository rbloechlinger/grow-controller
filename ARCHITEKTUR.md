# Growbox Cardputer HMI — Code-Erklärung

Dieses Dokument erklärt die Cardputer-Firmware (v5.1) ausführlicher, als es die
Kommentare im Quelltext tun: das *Warum* hinter der Aufteilung, die Datenflüsse
zwischen den Modulen und die Stolperstellen, die man kennen sollte, bevor man
etwas erweitert. Es ist als Entwickler-Begleitdokument zum Code gedacht, nicht
als Ersatz — die einzelnen `.cpp`/`.h` bleiben die maßgebliche Quelle.

Zielgruppe ist jemand, der den Code lesen, debuggen oder erweitern will und
wissen muss, wo welche Verantwortung liegt und warum.

---

## 1. Was die Firmware tut

Der Cardputer ist die **optionale Bedien- und Anzeigeschicht** (HMI) eines
zweiteiligen Systems. Die eigentliche Klimaregelung läuft autonom auf dem Stamp
S3; der Cardputer zeigt nur an, was der Stamp meldet, und schickt auf Tastendruck
Befehle zurück. Fällt der Cardputer aus, regelt der Stamp unbeeinflusst weiter.

Konkret leistet die Firmware vier Dinge:

1. **Telemetrie empfangen und anzeigen** — Temperatur, Feuchte, Lüfter-, Mode-
   und Befeuchter-Status kommen periodisch (und nach jedem Befehl) vom Stamp.
2. **Befehle senden** — Sollwert ändern, Lüfter/Befeuchter manuell schalten,
   zurück auf AUTO, Sofort-Messung anfordern.
3. **Verlauf darstellen** — ein Live-Trend der letzten ~56 Minuten plus das
   Durchstöbern alter Sessions, die auf der microSD liegen.
4. **Lückenlos protokollieren** — jede Session wird als CSV auf SD geschrieben;
   verpasste Samples werden nach einem Reconnect aus dem RAM-Puffer des Stamps
   nachgeholt (Backlog).

Die Verbindung ist **reines ESP-NOW** auf festem Kanal, ohne WiFi und ohne OTA.
Geflasht wird ausschließlich über USB bzw. über den Launcher von SD.

---

## 2. Warum die Modulaufteilung

Die Firmware war ursprünglich ein einzelner ~850-Zeilen-Sketch (`cardputer.ino`).
Bei dieser Größe wird ein Monolith unübersichtlich, und der Arduino-Mechanismus
(alle `.ino`-Dateien werden zu einer Übersetzungseinheit zusammengeklebt) bringt
einen impliziten globalen Namespace mit, den man bei wachsendem Code loswerden
will. Deshalb liegt der Code jetzt als echte `.h`/`.cpp`-Module unter
PlatformIO.

Das Leitprinzip der Aufteilung:

> **Querliegender State liegt bewusst zentral in `state.*`; modul-privater State
> bleibt `static` in der jeweiligen `.cpp`.**

Embedded-Firmware mit einem Funk-Callback (läuft im WiFi-Task) und einem einzigen
Display-Sprite hat unvermeidlich State, der mehrere Schichten berührt: die
Live-Telemetrie, die der Callback schreibt und die Oberfläche liest; der
Sollwert, den die Tastatur ändert und die Oberfläche zeichnet; der
Trend-Ringpuffer, den Loop, Backlog-Merge und Oberfläche gemeinsam anfassen.
Diesen wirklich geteilten State an *einer* Stelle zu bündeln ist sauberer, als
`extern`-Deklarationen über alle Module zu streuen. Alles andere — die SD-Datei,
die Empfangs-Queue, das Canvas-Objekt — bleibt hinter der jeweiligen
Modulschnittstelle verborgen.

---

## 3. Modulübersicht und Abhängigkeiten

| Modul | Verantwortung | Öffentliche Schnittstelle (Auszug) |
|-------|---------------|-------------------------------------|
| `config.h` | Compile-Zeit-Konstanten: Pins, Kanal, Farben, Geometrie, `clampf` | nur Konstanten/Makros |
| `protocol.h` | gemeinsames Wire-Format mit dem Stamp | Structs, `MSG_*`, `CMD_*` |
| `state.*` | geteilter Laufzeit-State | `g_*`, `targetTemp`, `histTemp[]`, `linkUp()` |
| `link.*` | ESP-NOW: Setup, Empfang, Senden | `setupEspNow()`, `sendCommand()` |
| `backlog.*` | RAM-History-Replay (Queue) | `backlogEnqueueFrame()`, `drainBacklogQueue()`, `backlogSyncing()` |
| `storage.*` | SD-Sessions, CSV-Log, History-Browsing | `initSd()`, `logLine()`, `loadSession()` … |
| `ui.*` | Sprite-Renderer der drei Seiten | `uiInit()`, `drawScreen()`, `switchPage()` |
| `main.cpp` | `setup()`/`loop()`, Tastatur-Orchestrierung | — |

Die Abhängigkeiten verlaufen gerichtet und ohne Zyklus. Ein Pfeil bedeutet
„nutzt / hängt ab von":

```
                 config.h   protocol.h
                    ▲            ▲
                    │            │   (fast alle Module hängen an diesen beiden)
                    └─────┬──────┘
                          │
                       state.*  ◄───────────────┐
                          ▲                      │
            ┌─────────────┼───────────┐          │
            │             │           │          │
         storage.*     backlog.* ─► storage.*    │
            ▲             ▲                       │
            │             │                       │
          link.* ────────►┘ (Callback reicht     │
            ▲                Frames an backlog)   │
            │                                     │
           ui.* ─► storage.*, backlog.*, state.* ─┘
            ▲
            │
         main.cpp  ─► link, backlog, storage, ui, state
```

Kurz gefasst: `main` orchestriert alles. `link` empfängt und gibt Backlog-Frames
an `backlog` weiter. `backlog` schreibt in den Trend (`state`) und ins Log
(`storage`). `ui` liest aus `state`, `storage` und `backlog`. Niemand hängt von
`ui` ab außer `main` — die Oberfläche ist eine reine Senke.

---

## 4. Das Wire-Format (`protocol.h`)

`protocol.h` ist die gemeinsame Sprache beider Geräte und **muss byte-identisch**
in `cardputer/include/` und neben `growbox.yaml` liegen. Mischen verschiedener
Versionen führt dazu, dass beide Seiten die Frames der anderen stillschweigend
verwerfen. Alle Wire-Structs sind `#pragma pack(1)`, damit das Layout auf beiden
Compilern exakt übereinstimmt.

Drei Nachrichtentypen, unterschieden am ersten Byte:

- **`MSG_TELEMETRY` (0x01)** — Stamp → Cardputer, 16 Byte. Felder: `temperature`
  und `humidity` (je `float`), `fan_on`, `mode`, `hum_on` (je `uint8`) und
  `uptime_s` (`uint32`). Die Uptime ist die Referenz für die Backlog-Deduplikation.
- **`MSG_COMMAND` (0x02)** — Cardputer → Stamp, 6 Byte: `command` (ein `CMD_*`)
  plus ein `float`-Argument (z. B. der neue Sollwert).
- **`MSG_BACKLOG` (0x03)** — Stamp → Cardputer, ein History-Chunk. Byte 1 ist die
  Sample-Anzahl im Chunk (0 = Ende der Übertragung), Bytes 2–3 reserviert, ab
  Byte 4 folgen `count` × `BacklogSample`.

Ein `BacklogSample` ist 8 Byte: `t_s` (Stamp-Uptime des Samples), `temp_c10`
(Temperatur × 10 als `int16`, spart gegenüber `float` die Hälfte), `hum`
(Prozent, `HUM_INVALID` = 255 falls unbekannt) und `flags` (`FLAG_FAN`,
`FLAG_AUTO`, `FLAG_HUM`).

Wichtige Konstanten für das Verständnis des Codes:

- `MODE_AUTO = 1`, `MODE_MANUAL = 0` — der **eine** globale Regelmodus. Seit v5
  haben Lüfter und Befeuchter keine getrennten Modi mehr.
- `CMD_SET_TARGET`, `CMD_TOGGLE_VENT`, `CMD_SET_AUTO`, `CMD_GET_BACKLOG`,
  `CMD_TOGGLE_HUM`, `CMD_REFRESH` — die Befehls-IDs.
- `BACKLOG_CAPACITY = 5760` — Obergrenze des RAM-Puffers auf dem Stamp
  (~48 h bei 30 s/Sample). Der Cardputer dimensioniert seine Empfangs-Queue
  danach.
- `HUM_INVALID = 255` — der `uint8`-Platzhalter für „keine gültige Feuchte",
  das Pendant zu `NAN` bei der Temperatur.

---

## 5. Die drei zentralen Datenflüsse

Wer den Code verstehen will, sollte diese drei Pfade im Kopf haben. Sie laufen
durch mehrere Module, und genau dafür existiert das `state.*`-Modul.

### 5.1 Live-Telemetrie

```
Stamp ──ESP-NOW──► onEspNowRecv()        [link.cpp, läuft im WiFi-Task]
                       │ schreibt g_temp, g_hum, g_fanOn, g_humOn,
                       │ g_mode, g_stampUptime, g_lastRxMs
                       ▼
                    state.*  (volatile Globals)
                       │ liest
                       ▼
                    drawDash() / drawTrend()  [ui.cpp, läuft im Loop]
```

Der Empfang schreibt nur ein paar Werte und kehrt sofort zurück. Die Oberfläche
liest dieselben Werte beim nächsten Redraw. `g_lastRxMs` (Zeitstempel des letzten
Frames) speist `linkUp()`, das überall entscheidet, ob die Anzeige „frisch" oder
„veraltet" dargestellt wird.

### 5.2 Befehle (Tastatur → Stamp)

```
Taste  ──► loop() wertet keysState() aus   [main.cpp]
              │
              ├─► sendCommand(CMD_*, value) [link.cpp] ──ESP-NOW──► Stamp
              │
              └─► optimistisches lokales Update von g_mode / g_fanOn / g_humOn
```

Die Tastenlogik sitzt komplett in `main.cpp::loop()`. Sie schickt den Befehl und
aktualisiert bei Schalt-Aktionen den lokalen State *optimistisch*, damit die
Anzeige sofort reagiert — die nächste echte Telemetrie korrigiert das dann
autoritativ (siehe Abschnitt 7.4).

### 5.3 Backlog-Replay

Der aufwändigste Pfad, und der einzige, der bewusst zweistufig ist:

```
Stamp ──MSG_BACKLOG──► onEspNowRecv()              [WiFi-Task]
                          │ backlogEnqueueFrame()
                          ▼
                    rxQueue (Ringpuffer)            [backlog.cpp, PRODUCER]
                          │
                          ▼
                    drainBacklogQueue()             [Loop, CONSUMER]
                          │ dedup via lastKnownUptime
                          ├─► histTemp/histHum (Trend)   [state.*]
                          └─► logLine(...)               [storage.*]
```

Der Funk-Callback **darf die SD-Karte nicht anfassen** (siehe 7.1). Deshalb legt
er die Rohsamples nur in eine Queue, und die eigentliche Arbeit — Einsortieren in
den Trend und Schreiben ins CSV — passiert im Loop, gedrosselt auf maximal 64
Samples pro Durchlauf.

---

## 6. Modul für Modul

### 6.1 `config.h`

Reiner Header mit Compile-Zeit-Konstanten: ESP-NOW-Kanal, Regel-Totband
(`DEAD_BAND`), Plot-Bereich (`TEMP_MIN`/`TEMP_MAX`), `LINK_TIMEOUT_MS`, die
SD-Pins, die Display-Geometrie, die Ringpuffer-Größe (`HIST_N = 112`,
`HIST_PERIOD_MS = 30000`) und die komplette RGB565-Farbpalette. Dazu die kleine
Inline-Hilfe `clampf()`, die sowohl die Oberfläche als auch der Loop braucht.

Alles hat *internal linkage* (eine Kopie pro Übersetzungseinheit), was für diese
kleinen Konstanten unproblematisch ist und eine `config.cpp` spart. `LOG_DIR` ist
als `#define` umgesetzt statt als `static const char*` — ein Makro hat keinen
Speicher und löst in Modulen, die es nicht nutzen, keine „unused"-Warnung aus.
Die Stamp-MAC steht bewusst **nicht** hier, sondern in `link.cpp`, weil nur der
Link-Layer sie braucht.

### 6.2 `state.*`

Das zentrale State-Modul. Die `extern`-Deklarationen stehen in `state.h`, die
einzige Definition jeweils in `state.cpp`. Inhalt:

- **Live-Telemetrie** als `volatile`: `g_temp`, `g_hum`, `g_fanOn`, `g_humOn`,
  `g_mode`, `g_lastRxMs`, `g_stampUptime`. `volatile`, weil der WiFi-Task sie
  schreibt und der Loop sie liest (siehe 7.1).
- **Sollwert** `targetTemp` (Startwert 32 °C, passend zum Stamp-Default).
- **Seiten-Enum** `Page { PAGE_DASH, PAGE_TREND, PAGE_HIST, PAGE_COUNT }` und
  `currentPage`.
- **Trend-Ringpuffer** `histTemp[]`, `histHum[]`, `histHead` (siehe 7.2).
- `lastKnownUptime` — die neueste bereits bekannte Stamp-Uptime, Dreh- und
  Angelpunkt der Backlog-Deduplikation.
- `linkUp()` — die zentrale Frische-Prüfung: `true`, wenn überhaupt schon ein
  Frame kam und der letzte jünger als `LINK_TIMEOUT_MS` ist.

### 6.3 `link.*`

Der ESP-NOW-Transport. `setupEspNow()` enthält das gehärtete Funk-Setup, das in
diesem Projekt hart erarbeitet wurde: `WiFi.persistent(false)`,
`esp_wifi_set_ps(WIFI_PS_NONE)` und das explizite Setzen des Kanals — ohne diese
bleibt das Radio nach einem Reboot nicht zuverlässig auf Kanal 1. Schlägt
`esp_now_init()` fehl, gibt die Funktion `false` zurück; die Fehleranzeige
übernimmt `main.cpp` (Trennung von Logik und Darstellung).

`onEspNowRecv()` ist `static` (nur intern via `esp_now_register_recv_cb`
sichtbar). Sie unterscheidet am ersten Byte: Telemetrie wird direkt in den
State geschrieben, ein `MSG_BACKLOG`-Frame wird ohne weitere Verarbeitung an
`backlogEnqueueFrame()` durchgereicht. Der unbenutzte `info`-Parameter der
vorgeschriebenen Callback-Signatur ist absichtlich unbenannt.

`sendCommand()` verpackt Befehl und Argument in eine `CommandMsg` und sendet sie
an die Stamp-MAC. Die MAC `stampMac[6]` steht hier als `static` und ist die
**einzige Stelle**, die man beim Gerätewechsel anpassen muss.

### 6.4 `backlog.*`

Kapselt die Empfangs-Queue vollständig hinter drei Funktionen — die Queue selbst
(`rxQueue`, `rxqHead`, `rxqTail`) ist `static` und von außen unsichtbar.

- `backlogEnqueueFrame()` ist die **Producer**-Seite (läuft im Callback): Sie
  prüft die Chunk-Länge, kopiert die Samples in den Ringpuffer und verwirft den
  Rest, falls die Queue voll ist. Bewusst minimal, weil sie im WiFi-Task läuft.
- `drainBacklogQueue()` ist die **Consumer**-Seite (läuft im Loop): Sie holt bis
  zu 64 Samples pro Durchlauf, verwirft Duplikate anhand `lastKnownUptime`,
  mischt jedes Sample in den Trend-Ringpuffer und schreibt es ins CSV. Erst wenn
  die Queue leergelaufen ist, wird die Logdatei einmal geflusht.
- `backlogSyncing()` meldet der Oberfläche, ob gerade eine Übertragung läuft
  (für die „SYNC"-Anzeige in der Statusleiste).

Die Zeitabbildung beim Einsortieren erzeugt absichtlich negative Timestamps für
Samples, die vor dem aktuellen Boot aufgenommen wurden (siehe 7.3).

### 6.5 `storage.*`

Das umfangreichste Modul: SD-Sessions, CSV-Logging und das Aufbereiten alter
Sessions für die History-Seite. Modul-privat bleiben die offene Logdatei
(`logFile`), der Flush-Zähler und die Liste der gefundenen Sessions; nach außen
sichtbar sind nur Status-/Navigationsfelder (`sdOk`, `sessionCount`, `histSel`,
`histLoaded`) und die aufbereiteten View-Puffer (`viewBuf`, `viewHum`, …).

Wichtige Funktionen:

- `initSd()` — initialisiert zuerst die View-Puffer (damit die History-Seite auch
  vor dem ersten Laden gefahrlos gezeichnet werden kann), startet SPI und SD,
  scannt vorhandene Sessions, legt die neue Session-Datei `log_NNNN.csv` an und
  füllt aus der vorherigen Session das Ende des Trend-Puffers vor.
- `refillTrendFrom()` — liest nur die letzten ~8 KB der vorigen Datei (mehr passt
  ohnehin nicht in den Trend), dezimiert sie und legt die Werte ans Pufferende,
  mit einer `NAN`-Lücke als sichtbarer Trenner „alt | neu".
- `logLine()` — schreibt eine CSV-Zeile (`t_s,temp_c,hum_pct,fan,mode`) und
  flusht nur alle vier Zeilen, um die SD-Karte zu schonen. `t_s` kann negativ
  sein (nachgefüllte Samples).
- `loadSession()` — lädt eine alte Session in zwei Durchläufen: erst Zeilen zählen
  und Dauer bestimmen, dann die Datei in `HIST_N` Buckets dezimieren
  (Mittelwert je Bucket) und dabei Min/Max festhalten. Alte 5-Spalten-Dateien
  laden weiterhin, weil der Parser nur die ersten Spalten liest.

### 6.6 `ui.*`

Der gesamte Renderer. Das einzige `M5Canvas` (Voll-Bild-Sprite) lebt hier als
`static`; `uiInit()` legt sein Backing-Buffer an. Sämtliche Zeichenhelfer
(`chip`, `degMark`, `dashedHLine`, `tempToY`, `humToY`) und die drei
Seitenzeichner (`drawDash`, `drawTrend`, `drawHist`) sind `static` — nach außen
gibt das Modul nur vier Funktionen frei:

- `uiInit()` — Sprite anlegen (nach `M5Cardputer.begin()`).
- `drawScreen()` — komponiert das Bild: Sprite löschen, Statusleiste, je nach
  `currentPage` eine der drei Seiten, dann in einem Rutsch auf das Display
  schieben (`pushSprite`).
- `switchPage()` / `enterHistPage()` — Navigation; letztere lädt beim ersten
  Betreten der History-Seite faul die jüngste Session.

`drawTrend` und `drawHist` zeichnen die Feuchte (cyan, Sekundärachse 0–100 %)
zuerst und die Temperatur (weiß) darüber, damit die Temperaturkurve oben liegt.
`humToY` und `tempToY` bilden auf dasselbe Pixelband ab, aber mit
unterschiedlichen Wertebereichen.

### 6.7 `main.cpp`

Die schlanke Orchestrierungsschicht. Hier liegen nur die loop-eigenen
Timing-Variablen (`lastSampleMs`, `lastDrawMs`, `prevLinkUp`, `backlogAsked`) als
`static`.

`setup()` bringt M5 hoch, legt das Sprite an, initialisiert die Trend-Arrays mit
`NAN`/`HUM_INVALID`, ruft `initSd()` (das den Trend aus der letzten Session
nachfüllt), startet ESP-NOW (mit Fehlerbildschirm bei Fehlschlag) und zeichnet
das erste Bild.

`loop()` macht in dieser Reihenfolge:

1. **Tastatur** auswerten — die komplette Tastenlogik (siehe Tabelle unten).
2. **Backlog anfordern** — einmal pro Verbindungsepisode, sobald die
   Stamp-Uptime bekannt ist, mit `CMD_GET_BACKLOG` und `lastKnownUptime` als
   „schick mir alles Neuere".
3. **Backlog verarbeiten** — `drainBacklogQueue()`.
4. **Live-Sampling** — alle `HIST_PERIOD_MS` (30 s) den aktuellen Wert in den
   Trend und ins CSV schreiben, sofern die Verbindung steht.
5. **Periodischer Redraw** — alle `REDRAW_PERIOD_MS` (150 ms) neu zeichnen.

| Taste | Befehl | Nebeneffekt |
|-------|--------|-------------|
| `w` / `s` | `CMD_SET_TARGET` (±0,5 °C) | — |
| `Leertaste` | `CMD_TOGGLE_VENT` | geht in MANUAL, schaltet `g_fanOn` optimistisch |
| `h` | `CMD_TOGGLE_HUM` | geht in MANUAL, schaltet `g_humOn` optimistisch |
| `a` | `CMD_SET_AUTO` | zurück auf AUTO (beide Ausgänge) |
| `r` | `CMD_REFRESH` | Sofort-Messung anfordern |
| `,` / `/` | — | Seite zurück / vor |
| `;` / `.` | — | nur History: ältere / neuere Session |

---

## 7. Querschnittsthemen

### 7.1 Nebenläufigkeit: WiFi-Task vs. Loop

Der ESP-NOW-Empfangs-Callback läuft **nicht** im Loop, sondern im WiFi-Task des
ESP32. Daraus folgen zwei Regeln, die das Design prägen:

- **Kein SD-Zugriff im Callback.** SD läuft über SPI und blockiert; im WiFi-Task
  würde das den Funk-Stack stören und kann zu Abstürzen führen. Deshalb staged
  der Callback nur Daten in die `rxQueue`, und der Loop erledigt die SD-Arbeit.
  Das ist der ganze Grund für die zweistufige Backlog-Architektur.
- **`volatile` für geteilte Werte.** Die Telemetrie-Globals sind `volatile`,
  damit der Compiler Lese-/Schreibzugriffe nicht wegoptimiert. Die Queue trennt
  Producer (Callback, schreibt `rxqHead`) und Consumer (Loop, schreibt `rxqTail`)
  sauber, sodass kein Lock nötig ist.

### 7.2 Der Trend-Ringpuffer

`histTemp[]` und `histHum[]` teilen sich `histHead` und werden im Gleichschritt
beschrieben. `histHead` zeigt auf die **nächste Schreibposition**, also zugleich
auf den ältesten Eintrag. Gelesen wird beim Zeichnen mit
`histTemp[(histHead + i) % HIST_N]` für `i = 0…HIST_N-1`, also vom ältesten zum
neuesten. Die Feuchte liegt als `uint8` vor (DHT11 liefert ohnehin nur ganze
Prozent); `HUM_INVALID` markiert Lücken, so wie `NAN` bei der Temperatur. An
Lücken (`NAN`/`HUM_INVALID`) bricht der Zeichner die Linie ab und beginnt eine
neue — so entstehen keine falschen Verbindungslinien über Datenlöcher hinweg.

### 7.3 Zeitabbildung und negative Timestamps

Backlog-Samples tragen die Stamp-Uptime ihres Entstehungszeitpunkts (`t_s`). Beim
Einsortieren rechnet `drainBacklogQueue()` diese auf die Uhr der aktuellen
Cardputer-Session um:

```
mapped = millis()/1000 − (g_stampUptime − s.t_s)
```

Samples, die **vor** dem aktuellen Cardputer-Boot entstanden, bekommen dadurch
absichtlich negative `t_s`-Werte im CSV. Das ist kein Fehler, sondern die
Markierung „das war vor meinem Start". Die History-Anzeige und der Parser kommen
damit zurecht.

### 7.4 Optimistische Oberfläche vs. autoritative Telemetrie

Drückt man Leertaste oder `h`, schaltet der Code `g_fanOn`/`g_humOn` und `g_mode`
sofort lokal um, damit die Anzeige nicht erst auf die nächste Telemetrie warten
muss. Diese lokalen Werte sind aber nur eine *optimistische Vermutung*. Die nächste
echte Telemetrie vom Stamp überschreibt sie und ist die **autoritative** Wahrheit
— wenn der Stamp den Befehl anders interpretiert hat (oder er verloren ging),
korrigiert sich die Anzeige automatisch innerhalb eines Telemetrie-Zyklus.

### 7.5 Flimmerfreies Rendering

Es wird nie direkt auf das Display gezeichnet. Stattdessen baut `drawScreen()` das
komplette Bild im `M5Canvas`-Sprite (im RAM) auf und schiebt es mit einem einzigen
`pushSprite(0,0)` auf den Schirm. Dadurch sieht man nie ein halb gezeichnetes
Bild — kein Flimmern, kein Tearing.

---

## 8. Erweitern: typische Aufgaben

### 8.1 Einen neuen Telemetriewert anzeigen

1. Feld in `TelemetryMsg` (in **beiden** `protocol.h`) ergänzen — beide Geräte neu
   flashen.
2. Im `MSG_TELEMETRY`-Zweig von `onEspNowRecv()` (`link.cpp`) das Feld in einen
   neuen `g_*`-State schreiben.
3. State in `state.h`/`state.cpp` deklarieren und definieren (`volatile`).
4. Im passenden Seitenzeichner in `ui.cpp` darstellen.

### 8.2 Eine neue Taste / einen neuen Befehl

1. `CMD_*`-ID in beiden `protocol.h` vergeben; den Handler im `growbox.yaml` des
   Stamps ergänzen.
2. In der Tastatur-Schleife in `main.cpp::loop()` einen `else if (c == …)`-Zweig
   hinzufügen, der `sendCommand(CMD_…, value)` aufruft.
3. Bei Schalt-Aktionen ggf. den lokalen State optimistisch aktualisieren.
4. Den Hinweistext (`drawHint(...)`) in `ui.cpp` anpassen — auf die Breite achten
   (240 px), sonst wird er abgeschnitten.

### 8.3 Eine neue Seite

1. Wert in das `Page`-Enum (`state.h`) vor `PAGE_COUNT` einfügen.
2. Einen `static void drawNeu()` in `ui.cpp` schreiben und im `switch` von
   `drawScreen()` einhängen.
3. Die Seitennavigation (`switchPage`) läuft automatisch über `PAGE_COUNT`, muss
   also nicht angefasst werden — außer die neue Seite braucht (wie die History)
   eine Lazy-Load-Logik beim Betreten.

---

## 9. Build und Flash (Kurzverweis)

Gebaut wird mit PlatformIO aus dem `cardputer/`-Ordner (`pio run` bzw. der
Build-Button in VS Code). Das App-Binary liegt danach unter
`.pio/build/m5stack-cardputer/firmware.bin` und lässt sich über USB flashen oder
per SD-Karte im Launcher installieren. Vor dem ersten Flash die `stampMac[]` in
`link.cpp` auf die STA-MAC des eigenen Stamps setzen. Details und die
MAC-Ermittlung stehen in der `README.md`.
