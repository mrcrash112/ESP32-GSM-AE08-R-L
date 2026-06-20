#include <Arduino.h>
#include <MD5Builder.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <Update.h>
#include <esp_ota_ops.h>

#include "BoardPins.h"
#include "BuildInfo.h"

namespace {
bool sdMounted = false;

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
  finalizeWebUpdate(result == "Firmware erfolgreich installiert");
  Preferences prefs;
  prefs.begin("fw-update", false);
  prefs.putString("result", result);
  prefs.putBool("ready", false);
  prefs.end();

  const esp_partition_t *mainPartition = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
  if (mainPartition && esp_ota_set_boot_partition(mainPartition) == ESP_OK) {
    Serial.println("Starte Hauptfirmware: " + result);
    delay(500);
    ESP.restart();
  }
  Serial.println("Keine gueltige Hauptfirmware gefunden. Recovery bleibt aktiv.");
}

bool actualMd5(File &file, String &value) {
  if (!file) return false;
  MD5Builder md5;
  md5.begin();
  md5.addStream(file, file.size());
  md5.calculate();
  value = md5.toString();
  file.seek(0);
  return true;
}

void installUpdate(const String &expectedMd5) {
  pinMode(BoardPins::sdCs, OUTPUT);
  pinMode(BoardPins::ethernetCs, OUTPUT);
  digitalWrite(BoardPins::ethernetCs, HIGH);
  SPI.begin(BoardPins::spiSck, BoardPins::spiMiso, BoardPins::spiMosi);
  if (!SD.begin(BoardPins::sdCs, SPI, 10000000)) return bootMain("SD-Karte nicht verfuegbar");
  sdMounted = true;

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
  size_t written = Update.writeStream(firmware);
  firmware.close();
  if (written != size || !Update.end(true) || Update.hasError()) {
    return bootMain("Firmware-Installation fehlgeschlagen");
  }
  SD.remove(BuildInfo::firmwarePath);
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
  Serial.println("Mione Recovery Updater");
  Preferences prefs;
  prefs.begin("fw-update", false);
  prefs.putString("recoveryVersion", BuildInfo::recoveryVersion);
  prefs.end();
  prefs.begin("fw-update", true);
  bool ready = prefs.getBool("ready", false);
  String expectedMd5 = prefs.getString("md5", "");
  prefs.end();
  expectedMd5.toLowerCase();
  if (!ready || expectedMd5.length() != 32) return bootMain("Kein Update vorgemerkt");
  installUpdate(expectedMd5);
}

void loop() { delay(1000); }
