# Mione Alarmmelder fuer NORVI GSM-AE08-R-L

PlatformIO-Grundprojekt fuer den NORVI GSM-AE08-R-L mit ESP32-WROOM32 und 4 MB
Flash. Die Pinbelegung folgt dem
[NORVI-Datenblatt](https://norvi.io/docs/norvi-gsm-ae08-r-l-datasheet/).

## Stand

Implementiert:

- WLAN und W5500-Ethernet, jeweils DHCP oder statische IPv4-Konfiguration
- Ersteinrichtung ueber offenen Hotspot `Mione-Setup-XXXXXX`
- Captive Portal mit automatischer Weiterleitung auf die Config-Seite
- persistente, validierte JSON-Konfiguration in NVS
- SD-Karte, DS3231 und SSD1306; alle Schnittstellen einzeln abschaltbar
- klare OLED-Statusanzeige mit Verbindungsart sowie WLAN-/LTE-Signalsymbolen
- OLED-Liveanzeige von Alarmierungsart, Rufnummer und Versandresultat
- Alarmfortschritt pro Rufnummer ueber MQTT und lokalen TCP-Socket
- gleitender ADC-Mittelwert aus zehn Messungen fuer die Fronttasten
- Datum und Uhrzeit mit Sekunden aus dem DS3231, alle 6 Stunden per NTP oder Mobilfunknetz synchronisiert
- Aktivierung des EC25-/SIM7500-Datenkontexts als Fallback ohne WLAN und Ethernet
- unveraenderliche Seriennummer aus der ESP32-eFuse-MAC und Modem-IMEI per `AT+GSN`
- automatische, nicht ueberschreibbare Modellerkennung per `ATI` fuer EC25 oder SIM7500
- Web-API sowie statische Webseiten von `/www` auf der SD-Karte
- Upload, Download und Loeschen unter `/www`, `/firmware` und `/logs`
- gepuffertes SD-Systemprotokoll mit konfigurierbarem Intervall ab 10 Sekunden
- MQTT-Alarmbefehle fuer SMS und Anruf mit gemeinsamem Befehls-Secret
- MQTT-Konfigurationsaenderungen mit anschliessendem Neustart
- Firmware-Upload oder GitHub-Download auf SD und MD5-Pruefung
- semantischer Versionsvergleich und Updatefreigabe per Taste, Web oder MQTT
- OLED-Fortschrittsanzeige fuer Download, MD5, WWW-Staging, Recovery und Flashvorgang
- automatische OTA-Bereinigung und bis zu drei Neuversuche bei MD5-, Download- oder WWW-Entpackfehlern
- optionales Recovery-Update vor der Installation der Hauptfirmware
- separate Factory-Recovery, die nur eine bereits gepruefte SD-Datei installiert

Noch hardwareabhaengig zu vervollstaendigen:

- direkter Mobilfunk-TCP-Kanal. EC25 und SIM7500 verwenden unterschiedliche
  Socket-AT-Kommandos. Host, Port und Aktivierung sind bereits Teil der Config;
  der Transport muss nach Feststellung der verbauten Variante implementiert und
  mit dem Mione-Protokoll getestet werden.
- TLS-Zertifikats-Pinning fuer GitHub und MQTT/TLS. Der aktuelle GitHub-Download
  wird per HTTPS transportiert und anhand des vorgegebenen MD5 geprueft. Fuer
  Produktion sollte zusaetzlich eine signierte SHA-256-Manifestdatei verwendet
  werden; MD5 allein ist kein kryptografischer Herkunftsnachweis.

Die Alarm-Grundfunktion `SMS`/`call` benutzt nur Modem-UART und NVS. Ein SD-Fehler
deaktiviert deshalb lediglich Webseiten von SD, Dateien, Updates und
Protokollierung.

Das Systemprotokoll liegt unter `/logs/system.csv`. Ereignisse werden im RAM
gepuffert und gesammelt im konfigurierten Intervall geschrieben. Das
Standardintervall betraegt 10 Sekunden und kann zwischen 10 und 3600 Sekunden
eingestellt werden. Protokolliert werden unter anderem Bootzaehler und
Resetgrund, Alarmempfaenger und Versandresultat, MQTT-Zustand, Netzwerkweg, IP,
WLAN-RSSI, Mobilfunk-CSQ/dBm, Netzbetreiber, Registrierung, Datenkontext,
Tastenmittelwert, RTC, SD und freier Heap. Heartbeats werden zum Schutz der
SD-Karte nicht protokolliert.
`MOBILE_CONFIG` wird nur bei einer tatsaechlichen Aenderung erzeugt und nennt
je Slot die alten und neuen Werte fuer Rufnummer, Aktivstatus und Alarmierungsart
sowie Aenderungen des Technical-Zeitfensters. Unveraenderte MQTT-Wiederholungen
werden weder erneut in NVS geschrieben noch protokolliert.
Der Button `Letzte Logeintraege anzeigen` oeffnet `/logs.html` in einem neuen
Fenster. Der Viewer zeigt standardmaessig die neuesten 250 Eintraege; die
Anzahl kann dort zwischen 1 und 2000 geaendert werden.

## Flash-Aufteilung

| Partition | Offset | Groesse | Zweck |
|---|---:|---:|---|
| `factory` | `0x10000` | 512 KB | Recovery-Updater |
| `ota_0` | `0x90000` | 3.375 MB | Hauptfirmware |
| `nvs` / `otadata` / `coredump` | variabel | Rest | Config, Bootstatus, Fehler |

Es gibt absichtlich keinen zweiten grossen OTA-Slot. Die Hauptfirmware legt das
Update und das zur Version gehoerende WWW-TAR auf SD ab. Die Webdateien werden
zuerst nach `/www-new` entpackt und mit `/www-old` als Rueckfall aktiviert.
Danach wird das Update in NVS markiert und `factory` gestartet. Die Recovery
prueft MD5 erneut, schreibt `ota_0` und bestaetigt anschliessend das neue
Webverzeichnis. Bei einem Fehler wird sowohl die vorhandene Hauptfirmware als
auch `/www-old` wiederhergestellt.

## Bauen und erstmalig flashen

```sh
~/.platformio/penv/bin/pio run -e main -e recovery
~/.platformio/penv/bin/pio run -e recovery -t upload
~/.platformio/penv/bin/pio run -e main -t upload
```

Die Umgebungen schreiben durch `board_upload.offset_address` an `0x10000` bzw.
`0x90000`. Beim ersten Recovery-Start ohne Update wird die vorhandene `ota_0`
ausgewaehlt. Danach mit dem Setup-Hotspot verbinden und `http://192.168.4.1`
aufrufen. Das initiale Web-Passwort und Befehls-Secret entsprechen der
12-stelligen Chip-ID und sollten bei der Ersteinrichtung geaendert werden.
Nur der noch nicht eingerichtete Setup-Hotspot erlaubt den Zugriff ohne
Anmeldung. Zugriffe ueber das normale WLAN oder Netzwerk sowie alle API-,
Datei- und Firmwarefunktionen sind durch HTTP Basic Authentication mit dem in
der Config-Seite festgelegten Web-Benutzer und Web-Passwort geschuetzt.
Werden die Tasten `UP` und `DOWN` gleichzeitig mindestens 350 ms gedrueckt,
zeigt das OLED die 12-stellige Chip-ID fuer 60 Sekunden mit Countdown an. Diese
Tastenkombination bestaetigt kein Firmwareupdate.

## MQTT

Basis-Topic ist standardmaessig `mione`:

Die aktuelle MiOne-Anwendung sendet alle fuenf Empfaenger gemeinsam als
retained Nachricht. Alarme werden separat und nicht retained gesendet:

```text
<mqtt_benutzername>/Alarmfunktionen/Config/Mobile
<mqtt_benutzername>/Alarmfunktionen/Config/Mobile/modemImei
<mqtt_benutzername>/Alarmfunktionen/Heartbeat
<mqtt_benutzername>/Alarmfunktionen/Alarm
```

Wenn `Alarmfortschritt` in der Web-Konfiguration aktiviert ist, veroeffentlicht
das Geraet fuer jede SMS und jeden Anruf `starting`, `succeeded` oder `failed`
unter `<mqtt_benutzername>/Alarmfunktionen/AlarmStatus`. Jede Meldung enthaelt IMEI,
Alarmcode, Alarmtext, Rufnummer und Alarmierungsart. MiOne zeigt nur Meldungen
mit der dort konfigurierten Modem-IMEI an.

Alle fuenf Sekunden sendet das Geraet ausserdem einen retained Modemstatus unter
`<mqtt_benutzername>/Alarmfunktionen/ModemStatus`. Er enthaelt Mobilfunkregistrierung,
Datenkontext, Signal, Netzbetreiber und den vollstaendigen OTA-Status fuer
Hauptfirmware, Recovery und WWW. MiOne wertet 60 Sekunden ohne neue Meldung als
Verbindungsverlust.

## TCP-Socket Direktverbindung

Bei aktivierter TCP-Socket Direktverbindung lauscht das Geraet im WLAN und Ethernet auf dem
konfigurierten TCP-Port. In MiOne werden die IP-Adresse des Modems und derselbe
Port eingetragen. MiOne sendet ein JSON-Objekt mit Zeilenumbruch und haelt die
Verbindung fuer die Alarmbearbeitung offen. Das Modem antwortet mit denselben
`alarmProgress`-JSON-Zeilen wie ueber MQTT und abschliessend mit `alarmResult`.
Der Schalter `Alarmfortschritt` steuert MQTT- und TCP-Rueckmeldungen gemeinsam.
Eine TCP-Verbindung erhaelt alle fuenf Sekunden zusaetzlich eine
`modemStatus`-Zeile. Mit `{"type":"statusRequest"}` kann MiOne diesen Status
sofort abfragen.

`Mobile/modemImei` enthaelt die in MiOne konfigurierte Modem-IMEI als retained
Textwert. Der ESP vergleicht sie mit der direkt aus dem Mobilfunkmodem gelesenen
IMEI. Der Heartbeat wird alle 5 Sekunden erwartet und gilt nach 60 Sekunden
ohne passendes Signal als gestoert. Beide Zustaende werden in der MQTT-Sektion
der Config-Seite angezeigt.

Beispiel fuer die Mobile-Konfiguration:

```json
{
  "modemImei": "867530912345678",
  "mobile": [
    {
      "slot": 1,
      "nummer": "+491701234567",
      "aktiv": true,
      "alarmsTo": 2,
      "alarmierung": "Anruf/Nachricht",
      "technicalAlarmMessagingFrom": 28800,
      "technicalAlarmMessagingUntil": 64800
    }
  ]
}
```

`alarmsTo` bedeutet `0 = Anruf`, `1 = SMS`, `2 = beides`, `3 = keine
Alarmierung`. Die frueheren Einzel-Topics unter `Alarmfunktionen/config/Mobile Slot 1-5`
werden aus Kompatibilitaetsgruenden weiterhin angenommen.

Die aktuelle MiOne-Anwendung sendet einen Alarm in dieser Form:

```json
{
  "modemImei": "867530912345678",
  "datum": "19.06.26",
  "uhrzeit": "12:30:00",
  "alarmCode": "4711",
  "prioritaet": "urgent",
  "alarmText": "Alarm Zone 2"
}
```

Das fruehere Topic `<mqtt_benutzername>/Alarmfunktionen/Alarme` wird weiterhin empfangen.
Vor der Verarbeitung muss `modemImei` exakt der direkt aus dem Modem gelesenen
IMEI entsprechen; andernfalls werden weder SMS noch Anrufe ausgeloest.

`urgent` wird unabhaengig von der Uhrzeit verarbeitet. `technical` wird nur im
Fenster `[From, Until)` verarbeitet; ein Fenster ueber Mitternacht wird
unterstuetzt. `From == Until` bedeutet ganztags. Zeiten sind Sekunden seit
00:00 Uhr. Jeder Alarm geht an alle aktiven Slots, jeweils als SMS,
mindestens 30 Sekunden langer Anruf oder beides.

Die IMEI wird vor jeder Uebernahme geprueft. Bei den kompatiblen Einzel-Topics
wird zusaetzlich das Befehls-Secret verlangt. Alarm-ID und Revision
werden vor der Ausfuehrung in NVS gespeichert, damit retained Nachrichten nach
einer Wiederverbindung nicht erneut ausgeloest werden.

Eine App-Konfiguration besteht aus
`{"imei":"...","secret":"...","revision":"...","config":{...}}`. Eine neue
Revision wird sofort gespeichert und startet das Geraet neu; dieselbe retained
Revision wird nach der Wiederverbindung nur bestaetigt und nicht erneut
angewendet.

## Web-API

| Methode | Pfad | Funktion |
|---|---|---|
| `GET` | `/api/status` | Laufzeitstatus |
| `GET`, `PUT` | `/api/config` | Config lesen oder speichern |
| `GET` | `/api/files?path=/www` | Dateien auflisten |
| `GET` | `/api/logs?limit=100` | letzte Systemlog-Eintraege lesen |
| `GET`, `DELETE` | `/api/file?path=/www/datei` | Download oder Loeschen |
| `POST` | `/api/file?path=/www/datei` | Multipart-Upload mit Feld `file` |
| `POST` | `/api/firmware/fetch?url=...&md5=...` | GitHub-Datei auf SD laden |
| `POST` | `/api/firmware/check` | konfiguriertes GitHub-Manifest pruefen |
| `POST` | `/api/firmware/approve` | angezeigtes neueres Release bestaetigen |

Nach der Ersteinrichtung ist HTTP Basic Auth aktiv. Dateipfade sind auf `/www`,
`/firmware` und `/logs` begrenzt. Die Firmwaredatei muss
`/firmware/update.bin` heissen.

Ein Release-Manifest fuer GitHub kann erzeugt werden mit:

```sh
python3 scripts/firmware_manifest.py .pio/build/main/firmware.bin 0.1.3 \
  https://github.com/mrcrash112/ESP32-GSM-AE08-R-L/releases/download/v0.1.3 \
  --recovery .pio/build/recovery/firmware.bin --recovery-version 0.1.1
```

Der Generator legt neben dem Manifest drei eindeutig benannte Release-Artefakte
`mione-main-<Version>.bin`, `mione-www-<Version>.tar` und
`mione-recovery-<Recovery-Version>.bin` an. Das WWW-Paket wird standardmaessig
aus `data/www` erzeugt; ein anderer Ordner kann mit `--www-dir` angegeben
werden. Alle Dateien muessen in dasselbe GitHub Release hochgeladen werden.

Als Standard ist
`https://github.com/mrcrash112/ESP32-GSM-AE08-R-L/releases/latest/download/firmware.json`
als `update.manifestUrl` hinterlegt. Das Geraet
prueft `product` und vergleicht `version` numerisch mit `FIRMWARE_VERSION`.
Die zum Build gehoerenden Versionen werden vorher im Abschnitt `[versions]` der
`platformio.ini` gesetzt; Manifest- und Build-Version muessen uebereinstimmen.
Nur eine hoehere Version wird angezeigt. Die Freigabe erfolgt durch 1,5 Sekunden
Tastendruck, `POST /api/firmware/approve` oder eine authentisierte Nachricht an
`update/approve`. Ist die Manifest-Recovery neuer als die zuletzt gestartete
Recovery, wird zuerst deren Factory-Partition geschrieben und per MD5
rueckgelesen. Anschliessend werden Hauptfirmware und WWW-TAR geladen und per MD5
geprueft. Das Manifest wird nur akzeptiert, wenn `web.version` exakt der neuen
Firmwareversion entspricht. Nach erfolgreichem WWW-Staging installiert die
Recovery die Hauptfirmware in `ota_0` und schliesst den Verzeichnistausch ab.

## Verzeichnisstruktur

```text
src/main/                 Hauptfirmware
src/recovery/             kleiner Factory-Updater
lib/AlarmCore/            Config, Befehle und Modem-Grundfunktionen
data/www/                 Vorlage fuer die SD-Webseite
data/www/config.css       responsive Bedienoberflaeche
data/www/config.js        Formular-, Status- und Dateiverwaltung
data/www/logo.png         Firmenlogo auf der Config-Seite
data/www/logs.html        geschuetzter Systemlog-Viewer
data/www/logs.css         responsive Logtabelle
data/www/logs.js          Laden und Begrenzen der letzten Logeintraege
include/BoardPins.h       dokumentierte NORVI-Pins
partitions.csv            gemeinsame 4-MB-Partitionstabelle
```
