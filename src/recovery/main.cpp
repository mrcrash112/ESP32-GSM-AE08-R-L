#include <Arduino.h>
#include <Adafruit_SSD1306.h>
#include <MD5Builder.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <Update.h>
#include <Wire.h>
#include <time.h>
#include <esp_ota_ops.h>

#include "BoardPins.h"
#include "BuildInfo.h"
#include "SystemRuntime.h"

namespace {
bool sdMounted = false;
bool displayReady = false;
Adafruit_SSD1306 display(128, 64, &Wire, -1);
uint32_t lastDisplayUpdate = 0;

String currentRecoveryLogPath() {
  time_t nowEpoch = time(nullptr);
  struct tm local {};
  if (nowEpoch < 946684800) return "/logs/system-boot.csv";
  localtime_r(&nowEpoch, &local);
  if (local.tm_year + 1900 < 2020) return "/logs/system-boot.csv";
  char text[40];
  snprintf(text, sizeof(text), "/logs/system-%04u-%02u-%02u.csv",
           local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
  return String(text);
}

void appendRecoveryLog(const String &event, const String &details) {
  if (!sdMounted) return;
  SD.mkdir("/logs");
  File logFile = SD.open(currentRecoveryLogPath(), FILE_APPEND);
  if (!logFile) return;
  if (logFile.size() == 0) logFile.print("timestamp;event;details\n");
  logFile.print("recovery;");
  logFile.print(event);
  logFile.print(';');
  logFile.print(details);
  logFile.print('\n');
  logFile.close();
}

void showStatus(const String &step, const String &detail, uint8_t progress, bool force = false) {
  if (!displayReady || (!force && millis() - lastDisplayUpdate < 100)) return;
  lastDisplayUpdate = millis();
  progress = min<uint8_t>(progress, 100);
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print("Alarmmodem V2");
  display.setCursor(92, 0);
  display.printf("%3u%%", progress);
  display.drawFastHLine(0, 10, 128, SSD1306_WHITE);
  display.setCursor(0, 15);
  display.print(step.substring(0, 21));
  display.setCursor(0, 29);
  display.print(detail.substring(0, 21));
  uint8_t pulse = (millis() / 250) % 4;
  display.setCursor(0, 41);
  display.print("arbeitet");
  for (uint8_t i = 0; i < 4; ++i) {
    if (i <= pulse) display.fillCircle(53 + i * 7, 44, 2, SSD1306_WHITE);
    else display.drawCircle(53 + i * 7, 44, 2, SSD1306_WHITE);
  }
  display.drawRect(0, 52, 128, 10, SSD1306_WHITE);
  if (progress) display.fillRect(2, 54, progress * 124UL / 100, 6, SSD1306_WHITE);
  display.display();
}

bool removeTree(const String &path) {
  File node = SD.open(path);
  if (!node) return true;
  if (!node.isDirectory()) {
    node.close();
    return SD.remove(path);
  }
  for (File child = node.openNextFile(); child; child = node.openNextFile()) {
    String childPath = child.name();
    if (!childPath.startsWith("/")) childPath = path + "/" + childPath;
    child.close();
    if (!removeTree(childPath)) {
      node.close();
      return false;
    }
  }
  node.close();
  return SD.rmdir(path);
}

bool finalizeWebUpdate(bool success) {
  if (!sdMounted) return false;
  Preferences prefs;
  if (!prefs.begin("fw-update", true)) return false;
  bool pending = prefs.getBool("webPending", false);
  bool hadPrevious = prefs.getBool("webHadPrevious", true);
  prefs.end();
  if (!pending) return true;
  bool completed = false;
  if (success) {
    completed = removeTree("/www-old");
  } else {
    if (SD.exists("/www-old")) completed = removeTree("/www") && SD.rename("/www-old", "/www");
    else completed = hadPrevious || removeTree("/www");
  }
  removeTree("/www-new");
  SD.remove(BuildInfo::webPackagePath);
  if (completed && prefs.begin("fw-update", false)) {
    prefs.putBool("webPending", false);
    prefs.remove("webStage");
    prefs.remove("webHadPrevious");
    prefs.end();
  }
  return completed;
}

void bootMain(const String &result) {
  bool success = result == "Firmware erfolgreich installiert";
  appendRecoveryLog("RECOVERY_REBOOT", "result=" + result);
  showStatus("WWW abschliessen", "Dateien aufraeumen", success ? 98 : 0, true);
  finalizeWebUpdate(success);
  showStatus(result == "Firmware erfolgreich installiert" ? "Update fertig" : "UPDATE FEHLER",
             result, success ? 100 : 0, true);
  Preferences prefs;
  prefs.begin("fw-update", false);
  prefs.putString("result", result);
  prefs.putBool("ready", false);
  prefs.end();

  const esp_partition_t *mainPartition = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
  if (mainPartition && esp_ota_set_boot_partition(mainPartition) == ESP_OK) {
    Serial.println("Starte Hauptfirmware: " + result);
    delay(1500);
    ESP.restart();
  }
  Serial.println("Keine gueltige Hauptfirmware gefunden. Recovery bleibt aktiv.");
}

bool actualMd5(File &file, String &value) {
  if (!file) return false;
  MD5Builder md5;
  md5.begin();
  size_t size = file.size();
  size_t checked = 0;
  uint8_t buffer[1024];
  if (!file.seek(0)) return false;
  while (checked < size) {
    size_t wanted = min(sizeof(buffer), size - checked);
    int count = file.read(buffer, wanted);
    if (count <= 0) return false;
    md5.add(buffer, static_cast<uint16_t>(count));
    checked += static_cast<size_t>(count);
    showStatus("Firmware pruefen", String(checked / 1024) + " / " + String(size / 1024) + " KB",
               12 + min<size_t>(18, checked * 18 / size));
    SystemRuntime::kickWatchdog();
    delay(0);
  }
  md5.calculate();
  value = md5.toString();
  file.seek(0);
  return true;
}

void installUpdate(const String &expectedMd5) {
  showStatus("SD-Karte", "wird gestartet", 5, true);
  pinMode(BoardPins::sdCs, OUTPUT);
  pinMode(BoardPins::ethernetCs, OUTPUT);
  digitalWrite(BoardPins::ethernetCs, HIGH);
  SPI.begin(BoardPins::spiSck, BoardPins::spiMiso, BoardPins::spiMosi);
  if (!SD.begin(BoardPins::sdCs, SPI, 10000000)) return bootMain("SD-Karte nicht verfuegbar");
  sdMounted = true;
  showStatus("SD-Karte", "Firmware gefunden", 10, true);

  File firmware = SD.open(BuildInfo::firmwarePath, FILE_READ);
  if (!firmware || firmware.isDirectory()) return bootMain("Firmware-Datei fehlt");
  String calculated;
  if (!actualMd5(firmware, calculated) || calculated != expectedMd5) {
    firmware.close();
    return bootMain("MD5-Pruefung fehlgeschlagen");
  }

  size_t size = firmware.size();
  if (!Update.begin(size, U_FLASH)) {
    firmware.close();
    return bootMain("Update konnte nicht gestartet werden");
  }
  Update.setMD5(expectedMd5.c_str());
  size_t written = 0;
  uint8_t buffer[1024];
  showStatus("Firmware schreiben", "Flash wird vorbereitet", 32, true);
  while (firmware.available()) {
    size_t count = firmware.read(buffer, sizeof(buffer));
    if (!count || Update.write(buffer, count) != count) break;
    written += count;
    showStatus("Firmware schreiben", String(written / 1024) + " / " + String(size / 1024) + " KB",
               32 + min<size_t>(60, written * 60 / size));
    SystemRuntime::kickWatchdog();
  }
  firmware.close();
  if (written != size || !Update.end(true) || Update.hasError()) {
    return bootMain("Firmware-Installation fehlgeschlagen");
  }
  SD.remove(BuildInfo::firmwarePath);
  showStatus("WWW abschliessen", "Dateien bestaetigen", 96, true);
  Preferences completed;
  if (completed.begin("fw-update", false)) {
    completed.putString("result", "Firmware erfolgreich installiert");
    completed.end();
  }
  bootMain("Firmware erfolgreich installiert");
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(250);
  SystemRuntime::initWatchdog();
  Wire.begin(BoardPins::i2cSda, BoardPins::i2cScl);
  displayReady = display.begin(SSD1306_SWITCHCAPVCC, BoardPins::oledAddress);
  showStatus("Recovery startet", "Update wird geprueft", 1, true);
  Serial.println("Mione Recovery Updater");
  Preferences prefs;
  prefs.begin("fw-update", false);
  prefs.putString("recoveryVersion", BuildInfo::recoveryVersion);
  prefs.end();
  prefs.begin("fw-update", true);
  bool ready = prefs.getBool("ready", false);
  String expectedMd5 = prefs.getString("md5", "");
  prefs.end();
  appendRecoveryLog("RECOVERY_BOOT", String("version=") + BuildInfo::recoveryVersion +
                   ",state=" + (ready && expectedMd5.length() == 32 ? "update" : "brick"));
  expectedMd5.toLowerCase();
  if (!ready || expectedMd5.length() != 32) return bootMain("Kein Update vorgemerkt");
  installUpdate(expectedMd5);
}

void loop() {
  SystemRuntime::kickWatchdog();
  delay(1000);
}
