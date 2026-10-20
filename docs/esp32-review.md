# ESP32 review, 2026-10-20

Review der ESP32-Firmware, der gemeinsam verwendeten Weboberflaeche und der
READMEs. Pfade sind relativ zum Repository `d200-unchained` (`public/` im
uebergeordneten Analyse-Repository). Die urspruenglichen C-Befunde wurden
statisch geprueft; ein Hardwaretest steht aus.

## Hohe Prioritaet

1. **Bildverlust bei gueltigen Seiten-IDs** (`esp32/main/config.c:104-117`).
   Die Bereinigung zerlegt Dateinamen am ersten `-key-`, obwohl diese Folge in
   Seiten-IDs erlaubt ist. `my-key-room-key-0-icon.rgb565` wird der Seite `my`
   zugeordnet und beim Speichern geloescht. IDs mit Praefix `key-` werden dagegen
   von der Bereinigung ausgenommen. Dateieigentuemer eindeutig bestimmen.
2. **Unzureichende Konfigurationsvalidierung** (`esp32/main/web.c:107-153`).
   Bereits `{"version":2,"pages":[]}` wird gespeichert und loest Bildbereinigung
   aus. Version, Root-Seite, Seitenbaum, IDs, Tasten und Feldtypen vor dem
   Speichern validieren. Fehler muessen die alte Konfiguration erhalten.
3. **Unbegrenztes USB-Warten** (`esp32/main/adb.c:48-53`). Der USB-Host setzt
   `timeout_ms` nicht durch; `portMAX_DELAY` blockiert bei ausbleibender Antwort
   dauerhaft. Der PING in `device.c:262-264` kann das nicht aufloesen. Eine echte
   Deadline mit sicherem Abbruch ausstehender Transfers ist erforderlich.
4. **Kein dauerhafter WLAN-Wiederanlauf** (`esp32/main/wifi.c:23-29`,
   `esp32/main/main.c:44-50`). Nach Ausschoepfen der Versuche wird nicht mehr
   verbunden. Ohne WLAN beim Boot starten Webserver, MQTT und SNTP nicht.
   Wiederanlauf mit Backoff und spaetere Dienstinitialisierung vorsehen.
5. **Host-Pruefung fehlt** (`esp32/main/web.c:49-58,254-275`). Der Zusatzheader
   allein schuetzt nicht vor DNS-Rebinding. Die gemeinsame API-Dokumentation
   verspricht bereits eine Host-Pruefung. Alle Routen entsprechend schuetzen.

## Weitere funktionale Befunde

6. **Gespeicherte Helligkeit ignoriert** (`esp32/main/device.c:23-26`,
   `esp32/main/web.c:147-154`, `esp32/main/main.c:40-49`). Startwert fest 200;
   Konfiguration wird weder beim Start noch bei Aenderung angewendet.
7. **Aktive Seite nach Import entfernt** (`esp32/main/web.c:147-154`).
   Laufzeitzustand bleibt auf einer nicht mehr vorhandenen Seite. Nach
   Konfigurationsuebernahme gegebenenfalls auf `root` wechseln.
8. **Datei bei kurzem Konfigurationswrite nicht geschlossen**
   (`esp32/main/config.c:34-37`). `&&` ueberspringt `fclose()` nach Schreibfehler.
   Immer schliessen und fehlgeschlagene temporaere Datei entfernen.
9. **Ladefehler werden als leere Konfiguration maskiert**
   (`esp32/main/config.c:16-31,40-46`). Nur eine fehlende Datei darf Defaults
   ergeben; I/O-, Parse- und Allokationsfehler muessen sichtbar bleiben.
10. **deckd-Updates nur nach Dateigroesse erkannt**
    (`esp32/main/bootstrap.c:154-169`). Ein anderer gleich grosser Build wird
    nie installiert. Hash oder Build-Identitaet pruefen.
11. **ADB-Sync ignoriert Stream-Ende und Fragmentierung**
    (`esp32/main/adb_sync.c:23-40,66-75`). `CLSE` beendet nicht alle
    Warteschleifen; der acht Byte lange Sync-Header wird nur aus einem einzelnen
    WRTE gelesen. Stream-Ende propagieren und Header zusammensetzen.
12. **Langsame SSE-Clients blockieren Aktionen und Timer**
    (`esp32/main/events.c:33-54`, `esp32/main/device.c:181-189`). Synchroner
    Netzwerkversand unter globalem Lock, auch aus dem Timer-Task. Versand
    entkoppeln und langsame Empfaenger abweisen.
13. **Iconaenderung zeichnet dynamische Taste nicht neu**
    (`esp32/main/keyfiles.c:191-205`). Nach Icon-/Alticon-Mutation fehlt Redraw.
14. **Autosave ueberschreibt Import**
    (`host/src/d200_deck/web/static/io.js:22-29`, `app.js:287-293`). Import
    speichert neu, anschliessendes Reload speichert noch offene alte Aenderungen.
    Import mit Autosave serialisieren und obsolete Saves ausschliessen.
15. **Backlight-Aktionen nicht atomar** (`esp32/main/actions.c:65-74`).
    Gleichzeitige Web-/Hardwareaktionen verlieren Schritte oder heben
    `disp_off` mit einem veralteten Snapshot auf.
16. **HTTP-Bodies nur einmal gelesen** (`esp32/main/web.c:167-171,219-223`).
    Seitenwechsel und MQTT-Einstellungen muessen den vollstaendigen Body lesen,
    nicht einen einzelnen TCP-/HTTP-Empfang als vollstaendig behandeln.
17. **Abgeschnittenes JSON in /api/info** (`esp32/main/web.c:83-89`).
    Fester 6-KB-Puffer, Rueckgabegroesse ignoriert. Viele Variablen koennen eine
    erfolgreiche Antwort mit ungueltigem JSON erzeugen.
18. **Neues Icon-Dirty-Flag durch alten Upload geloescht**
    (`host/src/d200_deck/web/static/autosave.js:89-97`). Flags nur loeschen, wenn
    die hochgeladene Revision noch aktuell ist.
19. **Version 1 akzeptiert, aber nicht migriert**
    (`esp32/main/web.c:113-114`, `esp32/main/config.c:57-64`). Flache Tastenliste
    vor dem Speichern in eine Version-2-Konfiguration mit Root-Seite umwandeln.

## Zusaetzliche Protokoll- und API-Befunde

- Selbsttest hinterlaesst einen offenen Ereignisstream (`usb_adb.c:67-72`,
  `deckd.c:153-190`); anschliessende Sitzung verwendet dieselbe lokale ADB-ID.
  Stream schliessen und eingehende Nachrichten der Sitzung zuordnen.
- Der 1024-Byte-Empfangspuffer des deckd-Clients kann waehrend einer
  Bilduebertragung durch gueltige Tastenevents ueberlaufen (`deckd.c:30-73`).
  Events fortlaufend parsen, auch beim Warten auf ADB-Bestaetigungen.
- Abgelehnte Helligkeitsaenderungen werden als erfolgreich behandelt
  (`device.c:248-254`). `DECKD_OK` pruefen und Fehler propagieren.
- Bild-GET ignoriert den konfigurierten Bilddateinamen
  (`keyfiles.c:84-88,185`), obwohl der Renderer diesen verwendet.
- TEST-Geraeteaktionen melden offline Erfolg statt des dokumentierten 503
  (`actions.c:115-118`, `keyfiles.c:173-180`).

## Dokumentation

### esp32/README.md

- Zeilen 7-10: `/api/info` gleichzeitig als Stub und implementiert beschrieben.
- Zeilen 4-5: API-Gleichheit zu pauschal angesichts der obigen Unterschiede.
- Zeilen 13-17: MQTT-Limits fehlen: 16 Variablen, feste Feldlaengen und
  Verwerfen fragmentierter Nachrichten bei Empfangspuffer 2048 Byte.
- Zeile 28: Partitionstabelle stimmt: 3 MiB App, 1 MiB WWW, 11,9375 MiB Storage.
- Zeilen 32-38: PSRAM, Zeitzone und WLAN-Einbettung stimmen grundsaetzlich.
  N8R8 erfuellt die aktuelle 16-MB-Partitionstabelle jedoch nicht.
- Zeilen 42-44: `scripts/deckd.py` existiert nicht; richtig `scripts/embed.py`.
- Zeilen 52-53: Getrenntes Flashen von Firmware und WWW stimmt.
- Testskript benoetigt gcc und einen vorherigen Firmwarebuild fuer Fonts/cJSON.

### README.md

- Zeilen 8-9,24-25: "changes nothing outside /userdata/d200" ist woertlich
  falsch: temporaeres Log, CPU-Sysfs und Framebuffer werden ebenfalls
  beschrieben. Keine Herstellerfirmware wird ueberschrieben.
- Neustart stellt die Stock-UI nur dauerhaft her, wenn der steuernde Host
  vorher gestoppt bzw. der ESP32 getrennt wird.
- Zeilen 46-67: Architekturvergleich stimmt, JSON-Austausch und MQTT brauchen
  die tatsaechlichen Einschraenkungen.
- Zeilen 102-133: Sicherheitsabschnitt bezieht sich ueberwiegend auf Linux;
  Geltungsbereich und ESP32-Schutz muessen deutlich werden.
- Die aeussere Analyse-README erwaehnt die ESP32-Variante bisher nicht.

## Verifikation des Reviews

- 19 vorhandene JavaScript-Tests bestanden.
- Import-/Autosave-Ueberschreiben und Icon-Dirty-Rennen mit den tatsaechlichen
  UI-Modulen und In-Memory-Mocks reproduziert.
- C-Befunde statisch durch Kontrollfluss-, Aufrufer- und Protokollpruefung;
  kein neuer Firmwarebuild oder Hardwaretest waehrend des Reviews.

## Umsetzung

Die Befunde oben dokumentieren den Ausgangszustand; Zeilennummern beziehen sich
auf diesen Stand. Fortschritt und Verifikation werden hier nachgetragen.

### Implementiert am 2026-10-20

| Befunde | Korrektur | Verifikation |
|---|---|---|
| 1 | Bildbereinigung verwendet den letzten Dateinamen-Trenner und behaelt noch referenzierte Bilder | Nativer Dateisystemtest mit `my-key-room`, `my-key`, `key-room` und gemeinsam genutztem Bild |
| 2, 19 | Eigene portable Schema-/Baumvalidierung, Defaults und v1-Migration vor Persistierung | Native Tests fuer ungueltige Seiten, Zyklen, doppelte IDs/Indizes, Typen, Aktionen, Regeln, Migration und unbekannte Aktionstypen |
| 3 | 3-s-Deadline, Endpoint halt/flush und Warten auf Abschluss vor Wiederverwendung; Neustart bei fehlgeschlagenem Abbruch | Mock-USB-Test fuer normale Uebertragung und Timeout mit Abschluss-Callback; Firmwarebuild |
| 4 | WLAN-Verbindung asynchron, unbegrenzter Retry mit Backoff bis 60 s; Dienste starten unabhaengig vom ersten DHCP-Erfolg | Firmwarebuild; Routerausfall/Boot ohne WLAN noch auf Hardware pruefen |
| 5, 16 | Host-Pruefung vor allen registrierten Routen und vollstaendiges Lesen kleiner HTTP-Bodies | Native Tests fuer erlaubten/abgelehnten Host, fragmentierten Body, Abbruch und Groessenlimit |
| 6, 7, 15 | Starthelligkeit laden, geaenderte Konfigurationshelligkeit uebernehmen, geloeschte aktive Seite auf Root setzen; atomare Backlight-Aktionen | Nativer Zustands-/Threadtest fuer Starthelligkeit, parallele Schritte und Display-off; Firmwarebuild |
| 8, 9 | Immer schliessen, fehlgeschlagene Temp-Datei entfernen, nur bei ENOENT Defaults laden; expliziter API-Import kann defekte Konfiguration ersetzen | Native Tests mit kurzem Write, Dateifehler und ungueltigem JSON |
| 10 | SHA-256 des eingebetteten, installierten und laufenden Daemons vergleichen; Push verifizieren | Firmwarebuild; Austausch gleich grosser Builds auf Hardware noch pruefen |
| 11 | Sync-Header ueber WRTEs sammeln; CLSE beendet alle relevanten Wartezustaende | Native Tests fuer alle sieben Headeraufteilungen, OPEN-Abweisung und vorzeitig geschlossenen Stream |
| 12 | Begrenzte Eventqueue, separater Sender, nichtblockierender SSE-Versand; volle Queue erzwingt Resync | Firmwarebuild; langsame WLAN-Clients noch auf Hardware pruefen |
| 13 | Redraw nach Icon-/Alticon-Upload und Loeschen | Aufruferpruefung und Firmwarebuild |
| 14, 18 | Vor Import offene Saves abschliessen und Bearbeitung sperren; Icon-Flags nur fuer die gespeicherte Revision loeschen | Vier neue JavaScript-Regressionstests |
| Weitere Protokoll/API-Befunde | Selbsttest-Stream schliessen, Stream-IDs pruefen, Events waehrend Bildtransfer parsen, Fehlerantworten beachten, Bild-GET aus Konfiguration aufloesen, offline TEST mit 503 beantworten | Native Protokolltests mit 1024 Events und Stream-Close; Zustandspruefung und Firmwarebuild |

Beim Fixen zusaetzlich gefunden und korrigiert:

- MQTT-Lock-Inversion: esp-mqtt ruft Callbacks unter seinem API-Lock auf.
  Subscription-Aenderungen werden deshalb nicht mehr unter dem Variablenlock
  ausgefuehrt. Ein separater Client-Lock schuetzt Publish gegen paralleles
  Zerstoeren/Ersetzen des Clients. Durch Quellpruefung des verwendeten ESP-IDF
  bestaetigt; Firmwarebuild bestanden, Broker-Reconnect-Stresstest noch offen.
- JSON-Pfadsegmente passen nun bis zur dokumentierten Pfadlaenge in den Parser.
  Grosse JSON-Zahlen werden vor der Ganzzahl-Konvertierung auf ihren Bereich
  geprueft. Beide Faelle sind im nativen MQTT-Test abgedeckt.
- API-Info wird auf die benoetigte Groesse erweitert; ein nativer Test mit 200
  Variablen prueft die Laengenberechnung und gueltiges JSON jenseits von 6 KiB.

Die ESP32- und Haupt-README sowie die gemeinsame MQTT-/API-Dokumentation wurden
auf den implementierten Stand gebracht. Auch die aeussere Analyse-README
verweist jetzt auf die ESP32-Variante und beschreibt die Wiederherstellung der
Stock-Oberflaeche korrekt.

### Checks

- `node --test tests/web/*.test.mjs` in `host/`: **23 bestanden**.
- `scripts/test.sh` in `esp32/`: **10 native Testprogramme bestanden**
  (config, protocol, adb_transport, web_guard, device_json, device_state,
  placeholders, rle, mqtt, render).
- `cargo build --release --target armv7-unknown-linux-musleabihf --bin deckd`
  in `device/`: **bestanden**.
- ESP32-PlatformIO-Build mit ESP-IDF 5.5.3: **bestanden**.
- `git diff --check`: **bestanden**.

Fuer diese Umgebung wurden PlatformIO-Core, Python-Umgebung und Buildausgabe
unter dem ignorierten `esp32/.pio/` angelegt. Die Buildbefehle verwenden deshalb
`PLATFORMIO_CORE_DIR="$PWD/.pio/core"` und
`PLATFORMIO_BUILD_DIR="$PWD/.pio/review-build"`; die nativen Tests nutzen denselben
Core-Pfad fuer cJSON. Die normalen README-Befehle bleiben unveraendert nutzbar.

**Noch nicht verifiziert:** Flashen und Betrieb auf einem echten ESP32/D200,
insbesondere WLAN-Ausfall, physisches USB-Abziehen, Abbruch eines echten
USB-Transfers, langsame SSE-Clients, MQTT-Reconnect unter Last und Austausch des
laufenden Daemons. Firmware und Webpartition muessen fuer den Hardwaretest beide
aktualisiert werden. Es wurde nicht geflasht; der Commitstand steht in der
Git-Historie.
