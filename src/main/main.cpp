#include <Arduino.h>
#include <ArduinoJson.h>
#include <Ethernet.h>
#include <EthernetUdp.h>
#include <DNSServer.h>
#include <PubSubClient.h>
#include <RTClib.h>
#include <SD.h>
#include <SPI.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <MD5Builder.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <time.h>

#include "BoardPins.h"
#include "BuildInfo.h"
#include "AlarmRouter.h"
#include "ConfigStore.h"
#include "DeviceConfig.h"
#include "ModemService.h"

namespace {
class Esp32EthernetServer : public EthernetServer {
 public:
  explicit Esp32EthernetServer(uint16_t port) : EthernetServer(port) {}
  void begin(uint16_t port = 0) override {
    (void)port;
    EthernetServer::begin();
  }
};

DeviceConfig config;
ConfigStore configStore;
ModemService modem;
AlarmRouter alarmRouter(modem);
WebServer web(80);
DNSServer captiveDns;
WiFiClient wifiClient;
EthernetClient ethernetClient;
WiFiServer *wifiAlarmServer = nullptr;
Esp32EthernetServer *ethernetAlarmServer = nullptr;
WiFiClient wifiAlarmClient;
EthernetClient ethernetAlarmClient;
Client *activeAlarmSocket = nullptr;
String alarmSocketInput;
WiFiUDP wifiUdp;
EthernetUDP ethernetUdp;
PubSubClient mqtt(wifiClient);
RTC_DS3231 rtc;
Adafruit_SSD1306 display(128, 64, &Wire, -1);

bool sdReady = false;
bool rtcReady = false;
bool displayReady = false;
bool ethernetReady = false;
bool accessPoint = false;
File uploadFile;
String uploadPath;
uint32_t lastMqttAttempt = 0;
int mqttConnectionCode = -1;
String mqttConnectionMessage = "Noch nicht gestartet";
String mqttConnectionTransport;
bool mqttMobileSubscriptionReady = false;
uint32_t mqttMobileReceivedAt = 0;
String mqttMobileSyncMessage = "Noch keine Mobile-Konfiguration empfangen";
uint32_t mioneHeartbeatReceivedAt = 0;
bool mioneHeartbeatValue = false;
String mioneHeartbeatImei;
String mioneHeartbeatTimestamp;
String mioneHeartbeatMessage = "Noch kein Heartbeat empfangen";
uint32_t mioneImeiReceivedAt = 0;
String mioneConfiguredImei;
uint32_t lastDisplay = 0;
uint32_t restartAt = 0;
uint32_t lastTimeAttempt = 0;
uint32_t lastTimeSync = 0;
uint32_t lastUpdateCheck = 0;
uint32_t updateButtonSince = 0;
int updateButtonIdle = 4095;
int currentButtonAdc = 4095;
uint16_t buttonSamples[10] = {};
uint32_t buttonSampleSum = 0;
uint8_t buttonSampleIndex = 0;
uint32_t lastButtonSample = 0;
uint32_t chipIdVisibleUntil = 0;
uint32_t chipIdButtonSince = 0;
bool chipIdButtonLatched = false;
bool updateCheckRequested = false;
String modemActivity;
String modemActivityNumber;
uint32_t modemActivityUntil = 0;
String pendingSystemLog;
uint32_t lastSystemLogWrite = 0;
uint32_t lastUpdateDisplayAt = 0;

void queueSystemLog(const String &event, const String &details);

struct UpdateState {
  bool available = false;
  bool firmwareAvailable = false;
  bool recoveryAvailable = false;
  bool checking = false;
  bool approved = false;
  bool installing = false;
  bool failed = false;
  String version;
  String firmwareUrl;
  String firmwareMd5;
  String recoveryVersion;
  String recoveryUrl;
  String recoveryMd5;
  String webVersion;
  String webUrl;
  String webMd5;
  String message;
  String detail;
  uint8_t progress = 0;
} updateState;

void showUpdateStatus(const String &step, const String &detail, uint8_t progress);

String chipId() {
  uint64_t mac = ESP.getEfuseMac();
  char value[13];
  snprintf(value, sizeof(value), "%04X%08X", static_cast<uint16_t>(mac >> 32), static_cast<uint32_t>(mac));
  return value;
}

String serialNumber() { return "MIONE-" + chipId(); }

bool parseIp(const String &value, IPAddress &address) { return address.fromString(value); }

bool newerVersion(String candidate, String current) {
  candidate.trim();
  current.trim();
  if (candidate.startsWith("v")) candidate.remove(0, 1);
  if (current.startsWith("v")) current.remove(0, 1);
  String candidateCore = candidate.substring(0, candidate.indexOf('-') < 0 ? candidate.length() : candidate.indexOf('-'));
  String currentCore = current.substring(0, current.indexOf('-') < 0 ? current.length() : current.indexOf('-'));
  for (uint8_t part = 0; part < 3; ++part) {
    int candidateDot = candidateCore.indexOf('.');
    int currentDot = currentCore.indexOf('.');
    int candidateValue = (candidateDot < 0 ? candidateCore : candidateCore.substring(0, candidateDot)).toInt();
    int currentValue = (currentDot < 0 ? currentCore : currentCore.substring(0, currentDot)).toInt();
    if (candidateValue != currentValue) return candidateValue > currentValue;
    candidateCore = candidateDot < 0 ? "0" : candidateCore.substring(candidateDot + 1);
    currentCore = currentDot < 0 ? "0" : currentCore.substring(currentDot + 1);
  }
  return current.indexOf('-') >= 0 && candidate.indexOf('-') < 0;
}

bool githubUrl(const String &url) {
  return url.startsWith("https://github.com/") || url.startsWith("https://raw.githubusercontent.com/") ||
         url.startsWith("https://objects.githubusercontent.com/");
}

bool validMd5(const String &value) {
  if (value.length() != 32) return false;
  for (size_t i = 0; i < value.length(); ++i) if (!isxdigit(static_cast<unsigned char>(value[i]))) return false;
  return true;
}

String installedRecoveryVersion() {
  Preferences prefs;
  prefs.begin("fw-update", true);
  String version = prefs.getString("recoveryVersion", "0.0.0");
  prefs.end();
  return version;
}

void setInstalledRecoveryVersion(const String &version) {
  if (version.isEmpty()) return;
  Preferences prefs;
  if (prefs.begin("fw-update", false)) {
    prefs.putString("recoveryVersion", version);
    prefs.end();
  }
}

bool checkUpdateManifest(String &error) {
  if (!sdReady) { error = "SD-Karte nicht verfuegbar"; return false; }
  if (WiFi.status() != WL_CONNECTED) { error = "Update-Pruefung benoetigt WLAN"; return false; }
  if (!githubUrl(config.updateManifestUrl)) { error = "GitHub-Manifest-URL fehlt"; return false; }
  updateState.checking = true;
  updateState.message = "Pruefe GitHub";
  showUpdateStatus("Manifest pruefen", "GitHub wird kontaktiert", 2);
  WiFiClientSecure tls;
  tls.setInsecure();
  HTTPClient request;
  request.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!request.begin(tls, config.updateManifestUrl)) { updateState.checking = false; error = "Manifest konnte nicht geoeffnet werden"; return false; }
  int status = request.GET();
  if (status != HTTP_CODE_OK) {
    request.end();
    updateState.checking = false;
    error = "Manifest HTTP " + String(status);
    return false;
  }
  DynamicJsonDocument manifest(3072);
  DeserializationError parsed = deserializeJson(manifest, request.getStream());
  request.end();
  updateState.checking = false;
  if (parsed) { error = String("Manifest ungueltig: ") + parsed.c_str(); return false; }
  if (manifest["product"].as<String>() != BuildInfo::product) { error = "Manifest gehoert zu einem anderen Produkt"; return false; }
  String version = manifest["version"] | "";
  JsonObjectConst firmware = manifest["firmware"];
  String firmwareUrl = firmware["url"] | manifest["url"] | "";
  String firmwareMd5 = firmware["md5"] | manifest["md5"] | "";
  firmwareMd5.toLowerCase();
  if (version.isEmpty() || !githubUrl(firmwareUrl) || !validMd5(firmwareMd5)) { error = "Firmware-Angaben im Manifest fehlen"; return false; }
  bool firmwareAvailable = newerVersion(version, BuildInfo::version);
  JsonObjectConst recovery = manifest["recovery"];
  String recoveryUrl = recovery["url"] | "";
  String recoveryMd5 = recovery["md5"] | "";
  recoveryMd5.toLowerCase();
  String recoveryVersion = recovery["version"] | "";
  if (!recoveryVersion.isEmpty() && (!githubUrl(recoveryUrl) || !validMd5(recoveryMd5))) { error = "Recovery-Angaben im Manifest sind ungueltig"; return false; }
  bool recoveryAvailable = !recoveryVersion.isEmpty() && newerVersion(recoveryVersion, installedRecoveryVersion());
  JsonObjectConst webPackage = manifest["web"];
  String webVersion = webPackage["version"] | "";
  String webUrl = webPackage["url"] | "";
  String webMd5 = webPackage["md5"] | "";
  String webFormat = webPackage["format"] | "";
  webMd5.toLowerCase();
  if (firmwareAvailable && (webVersion != version || webFormat != "tar" || !githubUrl(webUrl) || !validMd5(webMd5))) {
    error = "Passendes WWW-Paket fehlt oder ist ungueltig";
    return false;
  }
  updateState.firmwareAvailable = firmwareAvailable;
  updateState.recoveryAvailable = recoveryAvailable;
  updateState.available = firmwareAvailable || recoveryAvailable;
  updateState.version = version;
  updateState.firmwareUrl = firmwareUrl;
  updateState.firmwareMd5 = firmwareMd5;
  updateState.recoveryVersion = recoveryVersion;
  updateState.recoveryUrl = recoveryUrl;
  updateState.recoveryMd5 = recoveryMd5;
  updateState.webVersion = webVersion;
  updateState.webUrl = webUrl;
  updateState.webMd5 = webMd5;
  if (firmwareAvailable && recoveryAvailable) updateState.message = "Firmware " + version + " und Recovery " + recoveryVersion + " verfuegbar";
  else if (firmwareAvailable) updateState.message = "Firmware " + version + " verfuegbar";
  else if (recoveryAvailable) updateState.message = "Recovery " + recoveryVersion + " verfuegbar";
  else updateState.message = "Firmware und Recovery sind aktuell";
  return true;
}

bool downloadToSd(const String &url, const char *path, const String &expectedMd5,
                  const String &label, uint8_t progressFrom, uint8_t progressTo,
                  String &error) {
  if (!githubUrl(url) || !validMd5(expectedMd5)) { error = "Download-Angaben ungueltig"; return false; }
  WiFiClientSecure tls;
  tls.setInsecure();
  HTTPClient request;
  request.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!request.begin(tls, url)) { error = "Download konnte nicht gestartet werden"; return false; }
  int status = request.GET();
  if (status != HTTP_CODE_OK) { request.end(); error = "Download HTTP " + String(status); return false; }
  SD.remove(path);
  File target = SD.open(path, FILE_WRITE);
  if (!target) { request.end(); error = "Download-Datei konnte nicht erstellt werden"; return false; }
  WiFiClient *stream = request.getStreamPtr();
  int total = request.getSize();
  size_t written = 0;
  uint32_t lastData = millis();
  uint8_t buffer[1024];
  showUpdateStatus(label, total > 0 ? "0 / " + String(total / 1024) + " KB" : "Verbindung steht", progressFrom);
  while ((request.connected() || stream->available()) &&
         (total < 0 || written < static_cast<size_t>(total))) {
    size_t available = stream->available();
    if (available) {
      size_t count = min(available, sizeof(buffer));
      int received = stream->readBytes(buffer, count);
      if (received <= 0 || target.write(buffer, received) != static_cast<size_t>(received)) {
        error = "Download konnte nicht gespeichert werden";
        break;
      }
      written += received;
      lastData = millis();
    } else if (millis() - lastData > 15000) {
      error = "Download-Zeitueberschreitung";
      break;
    } else {
      delay(10);
    }
    uint8_t percent = total > 0 ? min<uint32_t>(100, written * 100UL / total) : 0;
    uint8_t overall = total > 0 ? progressFrom + (progressTo - progressFrom) * percent / 100 : progressFrom;
    String amount = String(written / 1024) + " KB";
    if (total > 0) amount += " / " + String(total / 1024) + " KB";
    showUpdateStatus(label, amount, overall);
  }
  target.close();
  request.end();
  if (!error.isEmpty() || written == 0 || (total > 0 && written != static_cast<size_t>(total))) {
    SD.remove(path);
    if (error.isEmpty()) error = "Download ist unvollstaendig";
    return false;
  }
  showUpdateStatus(label, "MD5 wird geprueft", progressTo);
  File check = SD.open(path, FILE_READ);
  MD5Builder md5;
  md5.begin();
  md5.addStream(check, check.size());
  md5.calculate();
  check.close();
  if (md5.toString() != expectedMd5) { SD.remove(path); error = "MD5-Pruefung fehlgeschlagen"; return false; }
  return true;
}

bool removeSdTree(const String &path) {
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
    if (!removeSdTree(childPath)) {
      node.close();
      return false;
    }
  }
  node.close();
  return SD.rmdir(path);
}

bool safeTarPath(const String &path) {
  if (path.isEmpty() || path.startsWith("/") || path.indexOf("..") >= 0 ||
      path.indexOf('\\') >= 0 || path.length() > 160) return false;
  uint8_t depth = 0;
  for (size_t i = 0; i < path.length(); ++i) if (path[i] == '/' && ++depth > 8) return false;
  return true;
}

uint32_t tarOctal(const uint8_t *value, size_t length) {
  uint32_t result = 0;
  for (size_t i = 0; i < length; ++i) {
    if (value[i] == 0 || value[i] == ' ') continue;
    if (value[i] < '0' || value[i] > '7') break;
    result = (result << 3) + value[i] - '0';
  }
  return result;
}

String tarText(const uint8_t *value, size_t length) {
  String result;
  for (size_t i = 0; i < length && value[i]; ++i) result += static_cast<char>(value[i]);
  return result;
}

bool ensureSdParents(const String &path) {
  int slash = path.indexOf('/', 1);
  while (slash > 0) {
    String directory = path.substring(0, slash);
    if (!SD.exists(directory) && !SD.mkdir(directory)) return false;
    slash = path.indexOf('/', slash + 1);
  }
  return true;
}

bool stageWebPackage(String &error) {
  File archive = SD.open(BuildInfo::webPackagePath, FILE_READ);
  if (!archive) { error = "WWW-Paket fehlt"; return false; }
  if (!removeSdTree("/www-new") || !SD.mkdir("/www-new")) {
    archive.close();
    error = "WWW-Staging-Verzeichnis konnte nicht erstellt werden";
    return false;
  }
  uint8_t header[512];
  uint8_t data[1024];
  uint16_t files = 0;
  size_t archiveSize = archive.size();
  showUpdateStatus("Webdateien", "Paket wird entpackt", 72);
  while (archive.available()) {
    if (archive.read(header, sizeof(header)) != sizeof(header)) { error = "WWW-TAR-Header ist unvollstaendig"; break; }
    bool empty = true;
    for (uint8_t value : header) if (value) { empty = false; break; }
    if (empty) break;
    String name = tarText(header, 100);
    String prefix = tarText(header + 345, 155);
    if (!prefix.isEmpty()) name = prefix + "/" + name;
    if (!safeTarPath(name)) { error = "Ungueltiger Pfad im WWW-Paket"; break; }
    uint32_t size = tarOctal(header + 124, 12);
    char type = static_cast<char>(header[156]);
    String targetPath = "/www-new/" + name;
    if (type == '5') {
      if (!SD.exists(targetPath) && !SD.mkdir(targetPath)) { error = "WWW-Verzeichnis konnte nicht erstellt werden"; break; }
    } else if (type == 0 || type == '0') {
      if (!ensureSdParents(targetPath)) { error = "WWW-Unterverzeichnis konnte nicht erstellt werden"; break; }
      SD.remove(targetPath);
      File target = SD.open(targetPath, FILE_WRITE);
      if (!target) { error = "WWW-Datei konnte nicht erstellt werden"; break; }
      uint32_t remaining = size;
      while (remaining) {
        size_t count = archive.read(data, min<size_t>(sizeof(data), remaining));
        if (!count || target.write(data, count) != count) { error = "WWW-Datei konnte nicht entpackt werden"; break; }
        remaining -= count;
      }
      target.close();
      if (!error.isEmpty()) break;
      ++files;
    } else {
      archive.seek(archive.position() + size);
    }
    uint16_t padding = (512 - (size % 512)) % 512;
    if (padding && !archive.seek(archive.position() + padding)) { error = "WWW-TAR-Padding ist ungueltig"; break; }
    uint8_t progress = archiveSize ? 72 + min<size_t>(10, archive.position() * 10 / archiveSize) : 72;
    showUpdateStatus("Webdateien", String(files) + " Dateien entpackt", progress);
  }
  archive.close();
  if (!error.isEmpty() || files == 0 || !SD.exists("/www-new/index.html") ||
      !SD.exists("/www-new/config.css") || !SD.exists("/www-new/config.js")) {
    removeSdTree("/www-new");
    if (error.isEmpty()) error = "WWW-Paket enthaelt nicht alle Pflichtdateien";
    return false;
  }
  return true;
}

bool activateStagedWeb(String &error) {
  bool hadWeb = SD.exists("/www");
  Preferences prefs;
  if (!prefs.begin("fw-update", false)) { error = "WWW-Transaktion konnte nicht markiert werden"; return false; }
  prefs.putBool("webPending", true);
  prefs.putBool("webHadPrevious", hadWeb);
  prefs.putString("webStage", "marked");
  prefs.putString("webVersion", updateState.webVersion);
  prefs.putString("result", "Firmwareupdate noch nicht ausgefuehrt");
  prefs.end();
  if (!removeSdTree("/www-old")) {
    if (prefs.begin("fw-update", false)) { prefs.putBool("webPending", false); prefs.end(); }
    error = "Altes WWW-Backup konnte nicht entfernt werden";
    return false;
  }
  if (prefs.begin("fw-update", false)) { prefs.putString("webStage", "backup"); prefs.end(); }
  if (hadWeb && !SD.rename("/www", "/www-old")) {
    if (prefs.begin("fw-update", false)) { prefs.putBool("webPending", false); prefs.end(); }
    error = "WWW-Backup konnte nicht erstellt werden";
    return false;
  }
  if (prefs.begin("fw-update", false)) { prefs.putString("webStage", "active"); prefs.end(); }
  if (!SD.rename("/www-new", "/www")) {
    bool restored = !hadWeb || SD.rename("/www-old", "/www");
    if (restored && prefs.begin("fw-update", false)) { prefs.putBool("webPending", false); prefs.end(); }
    error = "Neues WWW-Verzeichnis konnte nicht aktiviert werden";
    return false;
  }
  return true;
}

bool rollbackStagedWeb() {
  Preferences prefs;
  bool hadPrevious = true;
  if (prefs.begin("fw-update", true)) {
    hadPrevious = prefs.getBool("webHadPrevious", true);
    prefs.end();
  }
  bool restored = true;
  if (SD.exists("/www-old")) {
    restored = removeSdTree("/www") && SD.rename("/www-old", "/www");
  } else if (!hadPrevious) {
    restored = removeSdTree("/www");
  }
  removeSdTree("/www-new");
  SD.remove(BuildInfo::webPackagePath);
  if (restored && prefs.begin("fw-update", false)) {
    prefs.putBool("webPending", false);
    prefs.remove("webStage");
    prefs.remove("webHadPrevious");
    prefs.end();
  }
  return restored;
}

void recoverInterruptedWebUpdate() {
  if (!sdReady) return;
  Preferences prefs;
  if (!prefs.begin("fw-update", true)) return;
  bool pending = prefs.getBool("webPending", false);
  String result = prefs.getString("result", "");
  prefs.end();
  if (!pending) return;
  if (result == "Firmware erfolgreich installiert") {
    removeSdTree("/www-old");
    removeSdTree("/www-new");
    SD.remove(BuildInfo::webPackagePath);
    if (prefs.begin("fw-update", false)) {
      prefs.putBool("webPending", false);
      prefs.remove("webStage");
      prefs.remove("webHadPrevious");
      prefs.end();
    }
    queueSystemLog("UPDATE_COMMIT", "WWW-Paket nach erfolgreichem Firmwareupdate bestaetigt");
  } else {
    bool restored = rollbackStagedWeb();
    SD.remove(BuildInfo::firmwarePath);
    if (prefs.begin("fw-update", false)) {
      prefs.putBool("ready", false);
      prefs.putString("result", restored ? "Unterbrochenes Update zurueckgerollt" :
                                           "WWW-Rollback muss erneut versucht werden");
      prefs.end();
    }
    queueSystemLog(restored ? "UPDATE_ROLLBACK" : "UPDATE_ROLLBACK_ERROR",
                   restored ? "Unterbrochenes WWW/Firmware-Update zurueckgerollt" :
                              "WWW-Backup konnte nicht wiederhergestellt werden");
  }
}

bool installRecovery(const String &expectedMd5, String &error) {
  File image = SD.open(BuildInfo::recoveryPath, FILE_READ);
  const esp_partition_t *factory = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
  if (!image || !factory || image.size() > factory->size || image.read() != 0xE9) { image.close(); error = "Recovery-Image ist ungueltig"; return false; }
  image.seek(0);
  size_t imageSize = image.size();
  size_t eraseSize = (imageSize + SPI_FLASH_SEC_SIZE - 1) & ~(SPI_FLASH_SEC_SIZE - 1);
  showUpdateStatus("Recovery", "Flash wird geloescht", 19);
  if (esp_partition_erase_range(factory, 0, eraseSize) != ESP_OK) { image.close(); error = "Recovery-Partition konnte nicht geloescht werden"; return false; }
  uint8_t buffer[1024];
  size_t offset = 0;
  while (offset < imageSize) {
    size_t count = image.read(buffer, min(sizeof(buffer), imageSize - offset));
    if (!count || esp_partition_write(factory, offset, buffer, count) != ESP_OK) { image.close(); error = "Recovery konnte nicht geschrieben werden"; return false; }
    offset += count;
    showUpdateStatus("Recovery schreiben", String(offset / 1024) + " / " + String(imageSize / 1024) + " KB",
                     20 + min<size_t>(5, offset * 5 / imageSize));
  }
  image.close();
  MD5Builder verify;
  verify.begin();
  for (offset = 0; offset < imageSize; offset += sizeof(buffer)) {
    size_t count = min(sizeof(buffer), imageSize - offset);
    if (esp_partition_read(factory, offset, buffer, count) != ESP_OK) { error = "Recovery konnte nicht rueckgelesen werden"; return false; }
    verify.add(buffer, count);
    showUpdateStatus("Recovery pruefen", String((offset + count) / 1024) + " / " + String(imageSize / 1024) + " KB",
                     25 + min<size_t>(4, (offset + count) * 4 / imageSize));
  }
  verify.calculate();
  if (verify.toString() != expectedMd5) { error = "Recovery-Rueckpruefung fehlgeschlagen"; return false; }
  SD.remove(BuildInfo::recoveryPath);
  setInstalledRecoveryVersion(updateState.recoveryVersion);
  return true;
}

bool prepareApprovedUpdate(String &error) {
  if (!updateState.available || !sdReady || WiFi.status() != WL_CONNECTED) { error = "Update ist nicht bereit"; return false; }
  updateState.installing = true;
  updateState.message = "Update wird geladen";
  showUpdateStatus("Update vorbereiten", "Version " + updateState.version, 4);
  if (updateState.recoveryAvailable) {
    updateState.message = "Recovery wird aktualisiert";
    if (!downloadToSd(updateState.recoveryUrl, BuildInfo::recoveryPath, updateState.recoveryMd5,
                      "Recovery laden", 5, 18, error) ||
        !installRecovery(updateState.recoveryMd5, error)) { updateState.installing = false; updateState.message = error; return false; }
  }
  if (!updateState.firmwareAvailable) {
    updateState.installing = false;
    updateState.recoveryAvailable = false;
    updateState.available = false;
    updateState.message = "Recovery " + updateState.recoveryVersion + " installiert";
    showUpdateStatus("Recovery fertig", "Version " + updateState.recoveryVersion, 100);
    queueSystemLog("UPDATE_RECOVERY", "version=" + updateState.recoveryVersion + ",bereit=0");
    return true;
  }
  updateState.message = "Hauptfirmware wird geladen";
  if (!downloadToSd(updateState.firmwareUrl, BuildInfo::firmwarePath, updateState.firmwareMd5,
                    "Firmware laden", 30, 55, error)) { updateState.installing = false; updateState.message = error; return false; }
  updateState.message = "Weboberflaeche wird geladen";
  if (!downloadToSd(updateState.webUrl, BuildInfo::webPackagePath, updateState.webMd5,
                    "Webseite laden", 56, 70, error)) {
    SD.remove(BuildInfo::firmwarePath);
    updateState.installing = false;
    updateState.message = error;
    return false;
  }
  updateState.message = "Weboberflaeche wird vorbereitet";
  showUpdateStatus("Webdateien", "Staging wird vorbereitet", 71);
  if (!stageWebPackage(error)) {
    SD.remove(BuildInfo::firmwarePath);
    SD.remove(BuildInfo::webPackagePath);
    updateState.installing = false;
    updateState.message = error;
    return false;
  }
  const esp_partition_t *factory = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
  if (!factory) {
    removeSdTree("/www-new");
    SD.remove(BuildInfo::firmwarePath);
    SD.remove(BuildInfo::webPackagePath);
    updateState.installing = false;
    error = "Recovery-Partition wurde nicht gefunden";
    updateState.message = error;
    return false;
  }
  updateState.message = "Weboberflaeche wird aktiviert";
  showUpdateStatus("Webdateien", "Neue Version aktivieren", 84);
  if (!activateStagedWeb(error)) {
    SD.remove(BuildInfo::firmwarePath);
    SD.remove(BuildInfo::webPackagePath);
    updateState.installing = false;
    updateState.message = error;
    return false;
  }
  Preferences prefs;
  prefs.begin("fw-update", false);
  prefs.putString("md5", updateState.firmwareMd5);
  prefs.putBool("ready", true);
  prefs.end();
  if (esp_ota_set_boot_partition(factory) != ESP_OK) {
    rollbackStagedWeb();
    SD.remove(BuildInfo::firmwarePath);
    Preferences resetPrefs;
    if (resetPrefs.begin("fw-update", false)) {
      resetPrefs.putBool("ready", false);
      resetPrefs.end();
    }
    updateState.installing = false;
    error = "Recovery kann nicht gestartet werden";
    updateState.message = error;
    return false;
  }
  queueSystemLog("UPDATE", "version=" + updateState.version + ",webVersion=" + updateState.webVersion + ",bereit=1");
  updateState.message = "Neustart in Recovery";
  showUpdateStatus("Vorbereitung fertig", "Neustart in Recovery", 95);
  restartAt = millis() + 1000;
  return true;
}

bool authorized() {
  // Only the captive portal used for first-time setup may be opened without credentials.
  if (accessPoint && !config.provisioned) return true;
  if (web.authenticate(config.webUser.c_str(), config.webPassword.c_str())) return true;
  web.requestAuthentication();
  return false;
}

void jsonResponse(int code, JsonDocument &doc) {
  String body;
  serializeJson(doc, body);
  web.send(code, "application/json", body);
}

void errorResponse(int code, const String &message) {
  StaticJsonDocument<256> doc;
  doc["ok"] = false;
  doc["error"] = message;
  jsonResponse(code, doc);
}

String safePath(String path) {
  path.replace("\\", "/");
  if (!path.startsWith("/")) path = "/" + path;
  if (path.indexOf("..") >= 0 || path.length() > 96) return "";
  if (!path.startsWith("/www/") && !path.startsWith("/firmware/") && !path.startsWith("/logs/") &&
      path != "/www" && path != "/firmware" && path != "/logs") return "";
  return path;
}

void beginWifi() {
  if (!config.provisioned) {
    accessPoint = true;
    String ssid = "Mione-Setup-" + chipId().substring(6);
    WiFi.mode(WIFI_AP);
    WiFi.softAPsetHostname("mione-setup");
    WiFi.softAP(ssid.c_str());
    captiveDns.start(53, "*", WiFi.softAPIP());
    Serial.printf("Setup-Hotspot: %s, http://%s\n", ssid.c_str(), WiFi.softAPIP().toString().c_str());
    return;
  }
  if (!config.wifiEnabled || config.wifiSsid.isEmpty()) return;
  WiFi.mode(WIFI_STA);
  if (!config.wifiIp.dhcp) {
    IPAddress ip, gateway, subnet, dns;
    if (parseIp(config.wifiIp.address, ip) && parseIp(config.wifiIp.gateway, gateway) &&
        parseIp(config.wifiIp.subnet, subnet) && parseIp(config.wifiIp.dns, dns)) {
      WiFi.config(ip, gateway, subnet, dns);
    }
  }
  WiFi.begin(config.wifiSsid.c_str(), config.wifiPassword.c_str());
}

template <typename UdpClient>
bool requestNtp(UdpClient &udp, uint32_t &epoch) {
  uint8_t packet[48] = {};
  packet[0] = 0b11100011;
  packet[1] = 0;
  packet[2] = 6;
  packet[3] = 0xEC;
  if (!udp.begin(2390)) return false;
  if (!udp.beginPacket("pool.ntp.org", 123)) { udp.stop(); return false; }
  udp.write(packet, sizeof(packet));
  udp.endPacket();
  uint32_t started = millis();
  while (millis() - started < 1800) {
    int size = udp.parsePacket();
    if (size >= 48) {
      udp.read(packet, sizeof(packet));
      udp.stop();
      uint32_t seconds1900 = (static_cast<uint32_t>(packet[40]) << 24) |
                             (static_cast<uint32_t>(packet[41]) << 16) |
                             (static_cast<uint32_t>(packet[42]) << 8) | packet[43];
      if (seconds1900 <= 2208988800UL) return false;
      epoch = seconds1900 - 2208988800UL;
      return true;
    }
    delay(10);
  }
  udp.stop();
  return false;
}

void setRtcFromEpoch(uint32_t epoch) {
  time_t timestamp = epoch;
  struct tm local {};
  localtime_r(&timestamp, &local);
  rtc.adjust(DateTime(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                      local.tm_hour, local.tm_min, local.tm_sec));
}

bool setRtcFromModem(const String &response) {
  int quote = response.indexOf('"');
  if (quote < 0) return false;
  String value = response.substring(quote + 1, response.indexOf('"', quote + 1));
  int year = value.substring(0, value.indexOf('/')).toInt();
  if (year < 100) year += 2000;
  int firstSlash = value.indexOf('/');
  int secondSlash = value.indexOf('/', firstSlash + 1);
  int comma = value.indexOf(',');
  int firstColon = value.indexOf(':', comma + 1);
  int secondColon = value.indexOf(':', firstColon + 1);
  if (firstSlash < 0 || secondSlash < 0 || comma < 0 || firstColon < 0 || secondColon < 0) return false;
  int month = value.substring(firstSlash + 1, secondSlash).toInt();
  int day = value.substring(secondSlash + 1, comma).toInt();
  int hour = value.substring(comma + 1, firstColon).toInt();
  int minute = value.substring(firstColon + 1, secondColon).toInt();
  int second = value.substring(secondColon + 1, secondColon + 3).toInt();
  if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 59) return false;
  rtc.adjust(DateTime(year, month, day, hour, minute, second));
  return true;
}

void maintainClock() {
  if (!rtcReady || (lastTimeAttempt && millis() - lastTimeAttempt < 60000) ||
      (lastTimeSync && millis() - lastTimeSync < 21600000UL)) return;
  lastTimeAttempt = millis();
  uint32_t epoch = 0;
  bool synced = false;
  if (WiFi.status() == WL_CONNECTED) synced = requestNtp(wifiUdp, epoch);
  if (!synced && ethernetReady && Ethernet.linkStatus() == LinkON) synced = requestNtp(ethernetUdp, epoch);
  if (synced) setRtcFromEpoch(epoch);
  else {
    String modemTime;
    synced = modem.getNetworkTime(modemTime) && setRtcFromModem(modemTime);
  }
  if (synced) lastTimeSync = millis();
}

String clockText() {
  if (!rtcReady) return "RTC nicht verfuegbar";
  DateTime now = rtc.now();
  char text[32];
  snprintf(text, sizeof(text), "%02u.%02u.%04u %02u:%02u:%02u",
           now.day(), now.month(), now.year(), now.hour(), now.minute(), now.second());
  return text;
}

String logTimestamp() {
  if (!rtcReady) return "uptime-" + String(millis() / 1000);
  DateTime now = rtc.now();
  char text[32];
  snprintf(text, sizeof(text), "%04u-%02u-%02u %02u:%02u:%02u",
           now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second());
  return text;
}

String cleanLogValue(String value) {
  value.replace(';', ',');
  value.replace('\r', ' ');
  value.replace('\n', ' ');
  return value;
}

void queueSystemLog(const String &event, const String &details) {
  String line = logTimestamp() + ";" + cleanLogValue(event) + ";" + cleanLogValue(details) + "\n";
  while (pendingSystemLog.length() + line.length() > 8192) {
    int newline = pendingSystemLog.indexOf('\n');
    if (newline < 0) {
      pendingSystemLog = "";
      break;
    }
    pendingSystemLog.remove(0, newline + 1);
  }
  pendingSystemLog += line;
}

String activeNetworkName() {
  if (accessPoint) return "hotspot";
  if (ethernetReady && Ethernet.linkStatus() == LinkON) return "ethernet";
  if (WiFi.status() == WL_CONNECTED) return "wifi";
  if (modem.packetDataConnected()) return "cellular";
  return "offline";
}

void maintainSystemLog(bool force = false) {
  if (!sdReady) return;
  uint32_t interval = static_cast<uint32_t>(config.logIntervalSeconds) * 1000UL;
  if (!force && millis() - lastSystemLogWrite < interval) return;
  lastSystemLogWrite = millis();
  String ip = accessPoint ? WiFi.softAPIP().toString() :
              (ethernetReady && Ethernet.linkStatus() == LinkON ? Ethernet.localIP().toString() :
               (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "0.0.0.0"));
  String status = "network=" + activeNetworkName() + ",ip=" + ip +
                  ",wifiRssi=" + String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0) +
                  ",cellCsq=" + String(modem.signalQuality()) +
                  ",cellDbm=" + String(modem.signalQuality() >= 0 && modem.signalQuality() <= 31
                                            ? -113 + 2 * modem.signalQuality() : 0) +
                  ",operator=" + modem.networkOperator() +
                  ",registered=" + String(modem.connected() ? 1 : 0) +
                  ",packetData=" + String(modem.packetDataConnected() ? 1 : 0) +
                  ",mqtt=" + String(mqtt.connected() ? 1 : 0) +
                  ",sd=1,rtc=" + String(rtcReady ? 1 : 0) +
                  ",buttonAvg=" + String(currentButtonAdc) +
                  ",heartbeatAge=" + String(mioneHeartbeatReceivedAt
                                                 ? (millis() - mioneHeartbeatReceivedAt) / 1000 : UINT32_MAX) +
                  ",heap=" + String(ESP.getFreeHeap());
  queueSystemLog("STATUS", status);

  File logFile = SD.open("/logs/system.csv", FILE_APPEND);
  if (!logFile) return;
  if (logFile.size() == 0) logFile.print("timestamp;event;details\n");
  String batch = pendingSystemLog;
  size_t written = logFile.print(batch);
  logFile.flush();
  logFile.close();
  if (written == batch.length()) pendingSystemLog = "";
}

const char *resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt-watchdog";
    case ESP_RST_TASK_WDT: return "task-watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep-sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    default: return "unknown";
  }
}

void beginSharedSpi() {
  pinMode(BoardPins::sdCs, OUTPUT);
  pinMode(BoardPins::ethernetCs, OUTPUT);
  digitalWrite(BoardPins::sdCs, HIGH);
  digitalWrite(BoardPins::ethernetCs, HIGH);
  SPI.begin(BoardPins::spiSck, BoardPins::spiMiso, BoardPins::spiMosi);
}

void beginSd() {
  if (!config.sdEnabled) return;
  sdReady = SD.begin(BoardPins::sdCs, SPI, 10000000);
  if (sdReady) {
    SD.mkdir("/www");
    SD.mkdir("/firmware");
    SD.mkdir("/logs");
  }
}

void beginEthernet() {
  if (!config.ethernetEnabled) return;
  byte mac[] = {0x02, 0x4D, 0x49, 0x4F, static_cast<byte>(ESP.getEfuseMac() >> 8), static_cast<byte>(ESP.getEfuseMac())};
  Ethernet.init(BoardPins::ethernetCs);
  if (config.ethernetIp.dhcp) {
    ethernetReady = Ethernet.begin(mac, 5000, 1500) != 0;
  } else {
    IPAddress ip, gateway, subnet, dns;
    if (parseIp(config.ethernetIp.address, ip) && parseIp(config.ethernetIp.gateway, gateway) &&
        parseIp(config.ethernetIp.subnet, subnet) && parseIp(config.ethernetIp.dns, dns)) {
      Ethernet.begin(mac, ip, dns, gateway, subnet);
      ethernetReady = Ethernet.hardwareStatus() != EthernetNoHardware;
    }
  }
}

void servePortal() {
  if (sdReady && SD.exists("/www/index.html")) {
    File file = SD.open("/www/index.html", FILE_READ);
    web.streamFile(file, "text/html");
    file.close();
    return;
  }
  static const char page[] PROGMEM = R"HTML(
<!doctype html><html lang="de"><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>Mione Notfall-Setup</title><style>
*{box-sizing:border-box}body{margin:0;background:#f2f6f8;color:#142536;font:15px system-ui}header{color:#fff;background:#071b2c;border-bottom:3px solid #ffba08;padding:24px}main{max-width:680px;margin:24px auto;padding:0 14px}.card{background:#fff;border:1px solid #dfe7ed;border-radius:14px;padding:22px;margin-bottom:16px}h1,h2{margin:0 0 8px}p{color:#647485}label{display:block;font-weight:700;margin:14px 0 5px}input{width:100%;padding:11px;border:1px solid #cad6df;border-radius:8px;font:inherit}.id{display:grid;grid-template-columns:1fr 1fr;gap:10px}.id div{background:#edf5f8;border-radius:8px;padding:10px}.id small{display:block;color:#647485}button{border:0;border-radius:8px;padding:12px 16px;color:#fff;background:#0787d1;font-weight:800}#msg{font-weight:700}@media(max-width:520px){.id{grid-template-columns:1fr}}
</style><header><b>ELEKTROTECHNIK JOZEFOWICZ</b><h1>Mione Alarmmelder</h1><span>Notfall-Konfiguration ohne SD-Karte</span></header><main>
<div class="card"><h2>Geraeteidentitaet</h2><div class="id"><div><small>Seriennummer</small><b id="serial">-</b></div><div><small>Modem-IMEI</small><b id="imei">-</b></div></div></div>
<form class="card" id="form"><h2>Ersteinrichtung</h2><p>Die SD-Karte ist nicht verfuegbar. Hier koennen die wichtigsten Zugangsdaten gesetzt werden.</p><label for="device">Geraete-ID</label><input id="device" required maxlength="48"><label><input id="wifiOn" type="checkbox"> WLAN aktivieren</label><label for="ssid">WLAN-Name (SSID)</label><input id="ssid" maxlength="32"><label for="wifiPass">Neues WLAN-Passwort</label><input id="wifiPass" type="password" maxlength="64" placeholder="Leer lassen zum Beibehalten"><label for="webUser">Web-Benutzer</label><input id="webUser" required><label for="webPass">Neues Web-Passwort</label><input id="webPass" type="password" minlength="8" placeholder="Leer lassen zum Beibehalten"><p><button>Speichern und neu starten</button></p><div id="msg"></div></form></main><script>
let cfg;const el=id=>document.getElementById(id);Promise.all([fetch('/api/config').then(r=>r.json()),fetch('/api/status').then(r=>r.json())]).then(([c,s])=>{cfg=c;el('device').value=c.deviceId;el('wifiOn').checked=c.wifi.enabled;el('ssid').value=c.wifi.ssid;el('webUser').value=c.web.user;el('serial').textContent=s.serialNumber;el('imei').textContent=s.modemImei||'nicht verfuegbar'});el('form').onsubmit=async e=>{e.preventDefault();cfg.deviceId=el('device').value.trim();cfg.provisioned=true;cfg.wifi.enabled=el('wifiOn').checked;cfg.wifi.ssid=el('ssid').value.trim();cfg.wifi.password=el('wifiPass').value||'***';cfg.web.user=el('webUser').value.trim();cfg.web.password=el('webPass').value||'***';let r=await fetch('/api/config',{method:'PUT',headers:{'Content-Type':'application/json'},body:JSON.stringify(cfg)});el('msg').textContent=await r.text()};
</script></html>)HTML";
  web.send_P(200, "text/html", page);
}

void setupWeb() {
  web.on("/", HTTP_GET, [] { if (authorized()) servePortal(); });
  web.on("/api/status", HTTP_GET, [] {
    if (!authorized()) return;
    DynamicJsonDocument doc(1792);
    doc["version"] = BuildInfo::version;
    doc["deviceId"] = config.deviceId;
    doc["serialNumber"] = serialNumber();
    doc["modemImei"] = modem.imei();
    doc["modemModel"] = modem.model();
    doc["provisioning"] = accessPoint;
    doc["wifiIp"] = accessPoint ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
    doc["ethernetIp"] = Ethernet.localIP().toString();
    doc["sd"] = sdReady;
    doc["rtc"] = rtcReady;
    doc["cellular"] = modem.connected();
    doc["signal"] = modem.signalQuality();
    doc["mqtt"] = mqtt.connected();
    JsonObject mqttConnection = doc.createNestedObject("mqttConnection");
    mqttConnection["connected"] = mqtt.connected();
    mqttConnection["code"] = mqttConnectionCode;
    mqttConnection["message"] = mqttConnectionMessage;
    mqttConnection["transport"] = mqttConnectionTransport;
    mqttConnection["lastAttemptMs"] = lastMqttAttempt;
    JsonObject logging = doc.createNestedObject("logging");
    logging["intervalSeconds"] = config.logIntervalSeconds;
    logging["pendingBytes"] = pendingSystemLog.length();
    logging["path"] = "/logs/system.csv";
    doc["dateTime"] = clockText();
    doc["internet"] = WiFi.status() == WL_CONNECTED ? "wifi" :
                      (ethernetReady && Ethernet.linkStatus() == LinkON) ? "ethernet" :
                      modem.packetDataConnected() ? "cellular" : "offline";
    JsonObject update = doc.createNestedObject("update");
    update["currentVersion"] = BuildInfo::version;
    update["recoveryVersion"] = installedRecoveryVersion();
    update["available"] = updateState.available;
    update["firmwareAvailable"] = updateState.firmwareAvailable;
    update["recoveryAvailable"] = updateState.recoveryAvailable;
    update["version"] = updateState.version;
    update["recoveryTargetVersion"] = updateState.recoveryVersion;
    update["checking"] = updateState.checking;
    update["installing"] = updateState.installing;
    update["message"] = updateState.message;
    jsonResponse(200, doc);
  });
  web.on("/api/alarm-routing", HTTP_GET, [] {
    if (!authorized()) return;
    DynamicJsonDocument doc(2560);
    alarmRouter.toJson(doc.to<JsonObject>());
    doc["subscriptionReady"] = mqttMobileSubscriptionReady;
    doc["lastReceivedMs"] = mqttMobileReceivedAt;
    doc["syncMessage"] = mqttMobileSyncMessage;
    doc["sourceTopic"] = config.mqttUser + "/MiOne/Config/Mobile";
    JsonObject heartbeat = doc.createNestedObject("heartbeat");
    uint32_t heartbeatAge = mioneHeartbeatReceivedAt ? millis() - mioneHeartbeatReceivedAt : UINT32_MAX;
    bool heartbeatImeiMatches = !modem.imei().isEmpty() && mioneHeartbeatImei == modem.imei();
    heartbeat["received"] = mioneHeartbeatReceivedAt != 0;
    heartbeat["online"] = mioneHeartbeatReceivedAt != 0 && heartbeatAge <= 15000 && heartbeatImeiMatches;
    heartbeat["ageSeconds"] = mioneHeartbeatReceivedAt ? heartbeatAge / 1000 : -1;
    heartbeat["value"] = mioneHeartbeatValue;
    heartbeat["timestampUtc"] = mioneHeartbeatTimestamp;
    heartbeat["imei"] = mioneHeartbeatImei;
    heartbeat["imeiMatches"] = heartbeatImeiMatches;
    heartbeat["message"] = mioneHeartbeatMessage;
    heartbeat["topic"] = config.mqttUser + "/MiOne/Heartbeat";
    JsonObject imeiCheck = doc.createNestedObject("imeiCheck");
    imeiCheck["received"] = mioneImeiReceivedAt != 0;
    imeiCheck["local"] = modem.imei();
    imeiCheck["configured"] = mioneConfiguredImei;
    imeiCheck["matches"] = !modem.imei().isEmpty() && mioneConfiguredImei == modem.imei();
    imeiCheck["topic"] = config.mqttUser + "/MiOne/Config/Mobile/modemImei";
    jsonResponse(200, doc);
  });
  web.on("/api/config", HTTP_GET, [] {
    if (!authorized()) return;
    DynamicJsonDocument doc(4096);
    config.toJson(doc.to<JsonObject>(), false);
    jsonResponse(200, doc);
  });
  web.on("/api/config", HTTP_PUT, [] {
    if (!authorized()) return;
    DynamicJsonDocument doc(4096);
    DeserializationError parsed = deserializeJson(doc, web.arg("plain"));
    if (parsed) return errorResponse(400, parsed.c_str());
    DeviceConfig candidate = config;
    String error;
    if (!candidate.fromJson(doc.as<JsonObjectConst>(), error)) return errorResponse(422, error);
    candidate.provisioned = true;
    if (!configStore.save(candidate, error)) return errorResponse(500, error);
    web.send(200, "application/json", "{\"ok\":true,\"restart\":true}");
    queueSystemLog("CONFIG", "Konfiguration gespeichert, Neustart geplant");
    restartAt = millis() + 250;
  });
  web.on("/api/files", HTTP_GET, [] {
    if (!authorized()) return;
    if (!sdReady) return errorResponse(503, "SD-Karte nicht verfuegbar");
    String rootPath = safePath(web.arg("path"));
    if (rootPath.isEmpty()) rootPath = "/www";
    File root = SD.open(rootPath);
    if (!root || !root.isDirectory()) return errorResponse(404, "Verzeichnis nicht gefunden");
    web.setContentLength(CONTENT_LENGTH_UNKNOWN);
    web.send(200, "application/json", "");
    web.sendContent("{\"files\":[");
    bool first = true;
    for (File item = root.openNextFile(); item; item = root.openNextFile()) {
      String itemPath = item.name();
      if (!itemPath.startsWith("/")) itemPath = rootPath + "/" + itemPath;
      String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);
      StaticJsonDocument<320> entry;
      JsonObject out = entry.to<JsonObject>();
      out["name"] = itemName;
      out["path"] = itemPath;
      out["size"] = item.size();
      out["directory"] = item.isDirectory();
      String json;
      serializeJson(entry, json);
      if (!first) web.sendContent(",");
      web.sendContent(json);
      first = false;
      item.close();
    }
    root.close();
    web.sendContent("]}");
    web.sendContent("");
  });
  web.on("/api/logs", HTTP_GET, [] {
    if (!authorized()) return;
    if (!sdReady || !SD.exists("/logs/system.csv")) return errorResponse(404, "Noch kein Systemprotokoll vorhanden");
    int limit = web.arg("limit").toInt();
    if (limit < 1) limit = 250;
    if (limit > 2000) limit = 2000;
    File logFile = SD.open("/logs/system.csv", FILE_READ);
    if (!logFile) return errorResponse(500, "Systemprotokoll konnte nicht geoeffnet werden");

    size_t size = logFile.size();
    size_t position = size;
    size_t startOffset = 0;
    uint16_t lineBreaks = 0;
    bool trailingBreakSkipped = false;
    uint8_t buffer[256];
    bool found = false;
    while (position > 0 && !found) {
      size_t blockStart = position > sizeof(buffer) ? position - sizeof(buffer) : 0;
      size_t blockSize = position - blockStart;
      logFile.seek(blockStart);
      size_t read = logFile.read(buffer, blockSize);
      for (size_t i = read; i > 0; --i) {
        if (buffer[i - 1] != '\n') continue;
        size_t absolute = blockStart + i - 1;
        if (!trailingBreakSkipped && absolute == size - 1) {
          trailingBreakSkipped = true;
          continue;
        }
        ++lineBreaks;
        if (lineBreaks == limit) {
          startOffset = absolute + 1;
          found = true;
          break;
        }
      }
      position = blockStart;
    }
    logFile.seek(startOffset);
    web.setContentLength(size - startOffset);
    web.send(200, "text/plain; charset=utf-8", "");
    String chunk;
    chunk.reserve(512);
    while (logFile.available()) {
      chunk = "";
      while (logFile.available() && chunk.length() < 512) chunk += static_cast<char>(logFile.read());
      web.sendContent(chunk);
    }
    logFile.close();
    web.sendContent("");
  });
  web.on("/api/file", HTTP_GET, [] {
    if (!authorized()) return;
    String path = safePath(web.arg("path"));
    if (!sdReady || path.isEmpty() || !SD.exists(path)) return errorResponse(404, "Datei nicht gefunden");
    File file = SD.open(path, FILE_READ);
    web.sendHeader("Content-Disposition", "attachment; filename=\"" + String(file.name()) + "\"");
    web.streamFile(file, "application/octet-stream");
    file.close();
  });
  web.on("/api/file", HTTP_DELETE, [] {
    if (!authorized()) return;
    String path = safePath(web.arg("path"));
    if (!sdReady || path.isEmpty()) return errorResponse(400, "Ungueltiger Pfad");
    bool ok = SD.remove(path);
    web.send(ok ? 200 : 404, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
  });
  web.on("/api/file", HTTP_POST,
    [] {
      if (!authorized()) return;
      if (uploadFile) uploadFile.close();
      web.send(uploadPath.isEmpty() ? 400 : 200, "application/json", uploadPath.isEmpty() ? "{\"ok\":false}" : "{\"ok\":true}");
      uploadPath = "";
    },
    [] {
      if (!authorized() || !sdReady) return;
      HTTPUpload &upload = web.upload();
      if (upload.status == UPLOAD_FILE_START) {
        uploadPath = safePath(web.arg("path"));
        if (!uploadPath.isEmpty()) {
          SD.remove(uploadPath);
          uploadFile = SD.open(uploadPath, FILE_WRITE);
        }
      } else if (upload.status == UPLOAD_FILE_WRITE && uploadFile) {
        uploadFile.write(upload.buf, upload.currentSize);
      } else if (upload.status == UPLOAD_FILE_END && uploadFile) {
        uploadFile.close();
      }
    });
  web.on("/api/firmware/check", HTTP_POST, [] {
    if (!authorized()) return;
    updateCheckRequested = true;
    web.send(202, "application/json", "{\"ok\":true,\"checking\":true}");
  });
  web.on("/api/firmware/approve", HTTP_POST, [] {
    if (!authorized()) return;
    if (!updateState.available || updateState.installing) return errorResponse(409, "Kein freigegebenes Update verfuegbar");
    updateState.approved = true;
    web.send(202, "application/json", "{\"ok\":true,\"approved\":true}");
  });
  web.on("/api/firmware/fetch", HTTP_POST, [] {
    if (!authorized()) return;
    if (!sdReady) return errorResponse(503, "SD-Karte nicht verfuegbar");
    if (WiFi.status() != WL_CONNECTED) return errorResponse(503, "Firmware-Download benoetigt WLAN");
    String url = web.arg("url");
    String expected = web.arg("md5");
    expected.toLowerCase();
    if (!url.startsWith("https://github.com/") && !url.startsWith("https://objects.githubusercontent.com/")) {
      return errorResponse(400, "Nur GitHub-HTTPS-URLs sind erlaubt");
    }
    if (expected.length() != 32) return errorResponse(400, "MD5 muss 32 Hex-Zeichen enthalten");
    WiFiClientSecure tls;
    // The pinned MD5 is the mandatory payload integrity check; GitHub still uses HTTPS in transit.
    tls.setInsecure();
    HTTPClient request;
    request.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (!request.begin(tls, url)) return errorResponse(502, "Download konnte nicht gestartet werden");
    int status = request.GET();
    if (status != HTTP_CODE_OK) {
      request.end();
      return errorResponse(502, "GitHub antwortet mit HTTP " + String(status));
    }
    SD.remove(BuildInfo::firmwarePath);
    File target = SD.open(BuildInfo::firmwarePath, FILE_WRITE);
    int written = target ? request.writeToStream(&target) : -1;
    target.close();
    request.end();
    if (written <= 0) return errorResponse(502, "Firmware konnte nicht gespeichert werden");
    File firmware = SD.open(BuildInfo::firmwarePath, FILE_READ);
    MD5Builder md5;
    md5.begin();
    md5.addStream(firmware, firmware.size());
    md5.calculate();
    firmware.close();
    if (md5.toString() != expected) {
      SD.remove(BuildInfo::firmwarePath);
      return errorResponse(422, "MD5-Pruefung fehlgeschlagen");
    }
    web.send(200, "application/json", "{\"ok\":true,\"staged\":true}");
  });
  web.onNotFound([] {
    if (!authorized()) return;
    String path = safePath("/www" + web.uri());
    if (sdReady && !path.isEmpty() && SD.exists(path)) {
      File file = SD.open(path, FILE_READ);
      String type = path.endsWith(".css") ? "text/css" :
                    path.endsWith(".js") ? "application/javascript" :
                    path.endsWith(".png") ? "image/png" :
                    path.endsWith(".svg") ? "image/svg+xml" :
                    path.endsWith(".json") ? "application/json" :
                    "text/html";
      web.streamFile(file, type);
      file.close();
    } else if (accessPoint) {
      web.sendHeader("Location", "http://192.168.4.1/", true);
      web.send(302, "text/plain", "Mione Setup");
    } else errorResponse(404, "Nicht gefunden");
  });
  web.begin();
}

String mqttDeviceRoot() {
  if (modem.imei().isEmpty()) return "";
  return config.mqttBaseTopic + "/modems/" + modem.imei();
}

bool mqttIdentityValid(JsonObjectConst input) {
  String imei = input["imei"] | "";
  String secret = input["secret"] | "";
  return !modem.imei().isEmpty() && imei == modem.imei() && secret == config.commandSecret;
}

bool configRevisionIsNew(const String &revision) {
  if (revision.isEmpty() || revision.length() > 64) return false;
  Preferences prefs;
  prefs.begin("mqtt-sync", true);
  bool isNew = prefs.getString("configRev", "") != revision;
  prefs.end();
  return isNew;
}

void storeConfigRevision(const String &revision) {
  Preferences prefs;
  if (prefs.begin("mqtt-sync", false)) {
    prefs.putString("configRev", revision);
    prefs.end();
  }
}

bool recordMioneHeartbeat(const String &imei, bool value, const String &timestamp, const String &source, String &result) {
  mioneHeartbeatImei = imei;
  mioneHeartbeatValue = value;
  mioneHeartbeatTimestamp = timestamp;
  mioneHeartbeatReceivedAt = millis();
  bool ok = !modem.imei().isEmpty() && imei == modem.imei();
  result = ok ? "Heartbeat empfangen" : "Heartbeat-IMEI stimmt nicht mit dem Modem ueberein";
  mioneHeartbeatMessage = result + " (" + source + ")";
  return ok;
}

void onMqtt(char *topic, byte *payload, unsigned int length) {
  String incoming(topic);
  String root = mqttDeviceRoot();
  String mioneRoot = config.mqttUser + "/MiOne/config";
  String mioneHeartbeatTopic = config.mqttUser + "/MiOne/Heartbeat";
  String mioneImeiTopic = config.mqttUser + "/MiOne/Config/Mobile/modemImei";
  if (!config.mqttUser.isEmpty() && incoming == mioneImeiTopic) {
    String received;
    received.reserve(length);
    for (unsigned int i = 0; i < length; ++i) received += static_cast<char>(payload[i]);
    received.trim();
    if (received.startsWith("\"") && received.endsWith("\"") && received.length() >= 2) {
      received = received.substring(1, received.length() - 1);
    }
    mioneConfiguredImei = received;
    mioneImeiReceivedAt = millis();
    return;
  }
  if (!config.mqttUser.isEmpty() && incoming == mioneHeartbeatTopic) {
    DynamicJsonDocument input(512);
    DeserializationError parsed = deserializeJson(input, payload, length);
    if (parsed) {
      mioneHeartbeatMessage = "Ungueltiger Heartbeat: " + String(parsed.c_str());
      return;
    }
    String result;
    String heartbeatImei = input["modemImei"] | input["imei"] | "";
    recordMioneHeartbeat(heartbeatImei, input["value"] | true, input["timestampUtc"] | "", "MQTT", result);
    return;
  }
  String currentMobileTopic = config.mqttUser + "/MiOne/Config/Mobile";
  if (!config.mqttUser.isEmpty() && incoming == currentMobileTopic) {
    DynamicJsonDocument input(3072);
    DeserializationError parsed = deserializeJson(input, payload, length);
    String result = parsed ? String(parsed.c_str()) : "";
    bool changed = false;
    bool ok = !parsed && alarmRouter.updateMobileSlots(input.as<JsonObjectConst>(), modem.imei(), result, &changed);
    mqttMobileReceivedAt = millis();
    mqttMobileSyncMessage = ok ? result : "Abgelehnt: " + result;
    if (!ok || changed) queueSystemLog(ok ? "MOBILE_CONFIG" : "MOBILE_CONFIG_ERROR", result);
    if (!root.isEmpty()) {
      DynamicJsonDocument response(1536);
      response["ok"] = ok;
      response["imei"] = modem.imei();
      response["message"] = result;
      String body;
      serializeJson(response, body);
      mqtt.publish((root + "/mobile/result").c_str(), body.c_str(), false);
    }
    return;
  }
  if (!config.mqttUser.isEmpty() && incoming.startsWith(mioneRoot + "/Mobile Slot ")) {
    int slot = incoming.substring((mioneRoot + "/Mobile Slot ").length()).toInt();
    DynamicJsonDocument input(1536);
    DeserializationError parsed = deserializeJson(input, payload, length);
    String result = parsed ? String(parsed.c_str()) : "";
    bool ok = !parsed && alarmRouter.updateMobileSlot(slot, input.as<JsonObjectConst>(),
                                                       modem.imei(), config.commandSecret, result);
    mqttMobileReceivedAt = millis();
    mqttMobileSyncMessage = ok ? result : "Abgelehnt: " + result;
    if (!root.isEmpty()) {
      DynamicJsonDocument response(384);
      response["ok"] = ok;
      response["imei"] = modem.imei();
      response["slot"] = slot;
      response["message"] = result;
      String body;
      serializeJson(response, body);
      mqtt.publish((root + "/mobile/result").c_str(), body.c_str(), false);
    }
    return;
  }
  String currentAlarmTopic = config.mqttUser + "/MiOne/Alarm";
  String compatibleAlarmTopic = config.mqttUser + "/MiOne/Alarme";
  if (!config.mqttUser.isEmpty() &&
      (incoming == currentAlarmTopic || incoming == compatibleAlarmTopic ||
       incoming == mioneRoot + "/Alarme")) {
    DynamicJsonDocument input(6144);
    DeserializationError parsed = deserializeJson(input, payload, length);
    String result = parsed ? String(parsed.c_str()) : "";
    DateTime now = rtcReady ? rtc.now() : DateTime(2000, 1, 1, 0, 0, 0);
    uint32_t secondsOfDay = rtcReady ? now.hour() * 3600UL + now.minute() * 60UL + now.second() : UINT32_MAX;
    bool ok = !parsed && alarmRouter.processAlarmPayload(input.as<JsonObjectConst>(),
                                                          modem.imei(), config.commandSecret,
                                                          secondsOfDay, result);
    String alarmCode = input["alarmCode"] | "";
    String priority = input["prioritaet"] | "";
    queueSystemLog(ok ? "ALARM" : "ALARM_REJECTED",
                   "code=" + alarmCode + ",priority=" + priority + ",result=" + result);
    if (!root.isEmpty()) {
      DynamicJsonDocument response(512);
      response["ok"] = ok;
      response["imei"] = modem.imei();
      response["message"] = result;
      String body;
      serializeJson(response, body);
      mqtt.publish((root + "/alarms/result").c_str(), body.c_str(), false);
    }
    return;
  }
  if (root.isEmpty() || !incoming.startsWith(root + "/")) return;
  String configTopic = root + "/app/config";
  String updateCheckTopic = root + "/update/check";
  String updateApproveTopic = root + "/update/approve";
  if (incoming == updateCheckTopic || incoming == updateApproveTopic) {
    DynamicJsonDocument input(384);
    DeserializationError parsed = deserializeJson(input, payload, length);
    bool ok = !parsed && mqttIdentityValid(input.as<JsonObjectConst>());
    if (ok && incoming == updateCheckTopic) updateCheckRequested = true;
    if (ok && incoming == updateApproveTopic) {
      ok = updateState.available && !updateState.installing;
      if (ok) updateState.approved = true;
    }
    String resultTopic = root + "/update/result";
    mqtt.publish(resultTopic.c_str(), ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"nicht autorisiert oder kein Update\"}", false);
    return;
  }
  if (incoming == configTopic) {
    DynamicJsonDocument input(4608);
    DynamicJsonDocument response(384);
    DeserializationError parsed = deserializeJson(input, payload, length);
    String revision = input["revision"] | "";
    String error;
    bool identityOk = !parsed && mqttIdentityValid(input.as<JsonObjectConst>());
    bool revisionValid = !revision.isEmpty() && revision.length() <= 64;
    bool changed = identityOk && revisionValid && configRevisionIsNew(revision);
    bool ok = identityOk && revisionValid && input["config"].is<JsonObject>();
    bool applied = false;
    if (!parsed && !identityOk) error = "IMEI oder Secret stimmt nicht";
    else if (!parsed && !input["config"].is<JsonObject>()) error = "Config-Objekt fehlt";
    else if (!revisionValid) error = "Config-Revision fehlt";
    else if (!changed) error = "Konfiguration ist bereits aktuell";
    DeviceConfig candidate = config;
    if (ok && changed) ok = candidate.fromJson(input["config"].as<JsonObjectConst>(), error);
    if (ok && changed) ok = configStore.save(candidate, error);
    if (ok && changed) {
      storeConfigRevision(revision);
      applied = true;
    }
    response["ok"] = ok;
    response["applied"] = applied;
    response["message"] = applied ? "Konfiguration gespeichert; Neustart folgt" : (ok ? error : (parsed ? parsed.c_str() : error));
    response["imei"] = modem.imei();
    response["revision"] = revision;
    String body;
    serializeJson(response, body);
    String resultTopic = root + "/app/config/result";
    mqtt.publish(resultTopic.c_str(), body.c_str(), false);
    if (applied) restartAt = millis() + 1000;
    return;
  }
  return;
}

void setMqttConnectionStatus(int code, const String &message, const String &transport = "") {
  if (mqttConnectionCode == code && mqttConnectionMessage == message && mqttConnectionTransport == transport) return;
  mqttConnectionCode = code;
  mqttConnectionMessage = message;
  mqttConnectionTransport = transport;
  queueSystemLog("MQTT", "code=" + String(code) + ",message=" + message + ",transport=" + transport);
}

String mqttErrorMessage(int state) {
  switch (state) {
    case MQTT_CONNECTION_TIMEOUT: return "Zeitueberschreitung beim Verbindungsaufbau";
    case MQTT_CONNECTION_LOST: return "Verbindung zum Broker verloren";
    case MQTT_CONNECT_FAILED: return "TCP-Verbindung zum Broker fehlgeschlagen";
    case MQTT_DISCONNECTED: return "Brokerverbindung getrennt";
    case MQTT_CONNECT_BAD_PROTOCOL: return "Nicht unterstuetzte MQTT-Protokollversion";
    case MQTT_CONNECT_BAD_CLIENT_ID: return "Broker hat die Client-ID abgelehnt";
    case MQTT_CONNECT_UNAVAILABLE: return "MQTT-Broker ist nicht verfuegbar";
    case MQTT_CONNECT_BAD_CREDENTIALS: return "MQTT-Benutzername oder Passwort ist falsch";
    case MQTT_CONNECT_UNAUTHORIZED: return "MQTT-Verbindung wurde nicht autorisiert";
    default: return "Unbekannter MQTT-Fehler";
  }
}

void maintainMqtt() {
  if (!config.mqttEnabled) {
    setMqttConnectionStatus(-10, "MQTT ist in der Konfiguration deaktiviert");
    return;
  }
  if (config.mqttHost.isEmpty()) {
    setMqttConnectionStatus(-11, "Broker oder Hostname fehlt");
    return;
  }
  if (mqtt.connected()) {
    String transport = ethernetReady && Ethernet.linkStatus() == LinkON ? "Ethernet" : "WLAN";
    if (mqtt.loop()) setMqttConnectionStatus(MQTT_CONNECTED, "Mit dem MQTT-Broker verbunden", transport);
    else setMqttConnectionStatus(mqtt.state(), mqttErrorMessage(mqtt.state()), transport);
    return;
  }
  bool wifiUp = WiFi.status() == WL_CONNECTED;
  bool ethernetUp = ethernetReady && Ethernet.linkStatus() == LinkON;
  String root = mqttDeviceRoot();
  if (!wifiUp && !ethernetUp) {
    setMqttConnectionStatus(-12, "Keine WLAN- oder Ethernetverbindung");
    return;
  }
  if (root.isEmpty()) {
    setMqttConnectionStatus(-13, "Modem-IMEI fuer das MQTT-Topic fehlt");
    return;
  }
  if (millis() - lastMqttAttempt < 10000) return;
  lastMqttAttempt = millis();
  String transport = ethernetUp ? "Ethernet" : "WLAN";
  if (ethernetUp) mqtt.setClient(ethernetClient);
  else mqtt.setClient(wifiClient);
  mqtt.setServer(config.mqttHost.c_str(), config.mqttPort);
  String willTopic = root + "/status";
  String clientId = config.deviceId + "-" + modem.imei().substring(9);
  String offline = "{\"online\":false,\"imei\":\"" + modem.imei() + "\"}";
  bool connected = mqtt.connect(clientId.c_str(), config.mqttUser.c_str(), config.mqttPassword.c_str(), willTopic.c_str(), 1, true, offline.c_str());
  if (connected) {
    setMqttConnectionStatus(MQTT_CONNECTED, "Mit dem MQTT-Broker verbunden", transport);
    String online = "{\"online\":true,\"imei\":\"" + modem.imei() + "\"}";
    mqtt.publish(willTopic.c_str(), online.c_str(), true);
    String topic;
    if (!config.mqttUser.isEmpty()) {
      String mioneRoot = config.mqttUser + "/MiOne/config";
      bool subscriptionsOk = true;
      topic = config.mqttUser + "/MiOne/Config/Mobile";
      subscriptionsOk = mqtt.subscribe(topic.c_str(), 1) && subscriptionsOk;
      topic = config.mqttUser + "/MiOne/Config/Mobile/modemImei";
      subscriptionsOk = mqtt.subscribe(topic.c_str(), 1) && subscriptionsOk;
      topic = config.mqttUser + "/MiOne/Heartbeat";
      subscriptionsOk = mqtt.subscribe(topic.c_str(), 1) && subscriptionsOk;
      topic = config.mqttUser + "/MiOne/Alarm";
      subscriptionsOk = mqtt.subscribe(topic.c_str(), 1) && subscriptionsOk;
      topic = config.mqttUser + "/MiOne/Alarme";
      subscriptionsOk = mqtt.subscribe(topic.c_str(), 1) && subscriptionsOk;
      for (uint8_t slot = 1; slot <= 5; ++slot) {
        topic = mioneRoot + "/Mobile Slot " + String(slot);
        subscriptionsOk = mqtt.subscribe(topic.c_str(), 1) && subscriptionsOk;
      }
      topic = mioneRoot + "/Alarme";
      subscriptionsOk = mqtt.subscribe(topic.c_str(), 1) && subscriptionsOk;
      mqttMobileSubscriptionReady = subscriptionsOk;
      if (!subscriptionsOk) mqttMobileSyncMessage = "MQTT-Abonnement der MiOne-Topics fehlgeschlagen";
    }
    topic = root + "/app/config";
    mqtt.subscribe(topic.c_str(), 1);
    topic = root + "/update/check";
    mqtt.subscribe(topic.c_str(), 1);
    topic = root + "/update/approve";
    mqtt.subscribe(topic.c_str(), 1);
  } else {
    setMqttConnectionStatus(mqtt.state(), mqttErrorMessage(mqtt.state()), transport);
  }
}

void publishUpdateState() {
  if (!mqtt.connected()) return;
  DynamicJsonDocument doc(768);
  doc["currentVersion"] = BuildInfo::version;
  doc["currentRecoveryVersion"] = installedRecoveryVersion();
  doc["available"] = updateState.available;
  doc["firmwareAvailable"] = updateState.firmwareAvailable;
  doc["recoveryAvailable"] = updateState.recoveryAvailable;
  doc["version"] = updateState.version;
  doc["recoveryTargetVersion"] = updateState.recoveryVersion;
  doc["installing"] = updateState.installing;
  doc["message"] = updateState.message;
  String body;
  serializeJson(doc, body);
  String root = mqttDeviceRoot();
  if (root.isEmpty()) return;
  String topic = root + "/update/status";
  mqtt.publish(topic.c_str(), body.c_str(), true);
}

bool upDownButtonsPressed(int adcValue) {
  // NORVI S1 (UP, 67k) and S2 (DOWN, 33k) in parallel with the 47k pull-down.
  // The installed controller measures 3600-3700; the margin absorbs ADC noise.
  return adcValue >= 3550 && adcValue <= 3750;
}

void maintainButtons() {
  if (millis() - lastButtonSample < 10) return;
  lastButtonSample = millis();
  buttonSampleSum -= buttonSamples[buttonSampleIndex];
  buttonSamples[buttonSampleIndex] = analogRead(BoardPins::buttonsAdc);
  buttonSampleSum += buttonSamples[buttonSampleIndex];
  buttonSampleIndex = (buttonSampleIndex + 1) % 10;
  currentButtonAdc = static_cast<int>(buttonSampleSum / 10);
  if (upDownButtonsPressed(currentButtonAdc)) {
    if (!chipIdButtonSince) chipIdButtonSince = millis();
    if (!chipIdButtonLatched && millis() - chipIdButtonSince >= 350) {
      chipIdVisibleUntil = millis() + 60000UL;
      chipIdButtonLatched = true;
      lastDisplay = 0;
    }
  } else {
    chipIdButtonSince = 0;
    chipIdButtonLatched = false;
  }
}

void maintainUpdates() {
  if (updateState.approved && !updateState.installing) {
    updateState.approved = false;
    updateState.failed = false;
    String error;
    if (!prepareApprovedUpdate(error)) {
      updateState.failed = true;
      showUpdateStatus("UPDATE FEHLER", error, updateState.progress);
    }
    publishUpdateState();
    return;
  }
  if (updateState.installing || config.updateManifestUrl.isEmpty()) return;
  uint32_t interval = static_cast<uint32_t>(config.updateCheckMinutes) * 60000UL;
  bool periodic = config.updateCheckEnabled && millis() > 30000 && (lastUpdateCheck == 0 || millis() - lastUpdateCheck >= interval);
  if (updateCheckRequested || periodic) {
    updateCheckRequested = false;
    lastUpdateCheck = millis();
    String error;
    if (!checkUpdateManifest(error)) updateState.message = error;
    publishUpdateState();
  }
  int buttonValue = currentButtonAdc;
  if (upDownButtonsPressed(buttonValue)) {
    updateButtonSince = 0;
    return;
  }
  if (!updateState.available) {
    if (abs(buttonValue - updateButtonIdle) < 100) {
      updateButtonIdle = (updateButtonIdle * 15 + buttonValue) / 16;
    }
    updateButtonSince = 0;
  } else if (abs(buttonValue - updateButtonIdle) > 250) {
    if (!updateButtonSince) updateButtonSince = millis();
    else if (millis() - updateButtonSince >= 1500) {
      updateState.approved = true;
      updateButtonSince = 0;
    }
  } else {
    updateButtonSince = 0;
  }
}

uint8_t wifiSignalLevel() {
  if (WiFi.status() != WL_CONNECTED) return 0;
  int rssi = WiFi.RSSI();
  if (rssi >= -55) return 4;
  if (rssi >= -67) return 3;
  if (rssi >= -75) return 2;
  return 1;
}

uint8_t cellularSignalLevel() {
  int quality = modem.signalQuality();
  if (!modem.connected() || quality < 0 || quality == 99) return 0;
  if (quality >= 20) return 4;
  if (quality >= 14) return 3;
  if (quality >= 8) return 2;
  return quality > 0 ? 1 : 0;
}

void drawSignalBars(int16_t x, int16_t bottom, uint8_t level) {
  for (uint8_t bar = 0; bar < 4; ++bar) {
    int16_t height = 3 + bar * 2;
    int16_t left = x + bar * 4;
    if (bar < level) display.fillRect(left, bottom - height, 3, height, SSD1306_WHITE);
    else display.drawRect(left, bottom - height, 3, height, SSD1306_WHITE);
  }
}

void drawHotspotIcon(int16_t x, int16_t y) {
  display.fillCircle(x + 6, y + 8, 2, SSD1306_WHITE);
  display.drawLine(x + 6, y + 5, x + 2, y + 2, SSD1306_WHITE);
  display.drawLine(x + 6, y + 5, x + 10, y + 2, SSD1306_WHITE);
  display.drawLine(x + 6, y + 2, x + 3, y, SSD1306_WHITE);
  display.drawLine(x + 6, y + 2, x + 9, y, SSD1306_WHITE);
}

void drawEthernetIcon(int16_t x, int16_t y) {
  display.drawRect(x + 1, y, 11, 8, SSD1306_WHITE);
  display.drawFastHLine(x + 4, y + 8, 5, SSD1306_WHITE);
  display.drawPixel(x + 4, y + 2, SSD1306_WHITE);
  display.drawPixel(x + 7, y + 2, SSD1306_WHITE);
  display.drawPixel(x + 10, y + 2, SSD1306_WHITE);
}

void drawCellularIcon(int16_t x, int16_t y) {
  display.drawFastVLine(x + 6, y + 1, 8, SSD1306_WHITE);
  display.drawLine(x + 6, y + 1, x + 2, y + 4, SSD1306_WHITE);
  display.drawLine(x + 6, y + 1, x + 10, y + 4, SSD1306_WHITE);
  display.drawFastHLine(x + 3, y + 9, 7, SSD1306_WHITE);
}

void drawWifiIcon(int16_t x, int16_t y) {
  display.fillCircle(x + 6, y + 8, 2, SSD1306_WHITE);
  display.drawLine(x + 6, y + 5, x + 2, y + 2, SSD1306_WHITE);
  display.drawLine(x + 6, y + 5, x + 10, y + 2, SSD1306_WHITE);
  display.drawLine(x + 2, y + 2, x, y, SSD1306_WHITE);
  display.drawLine(x + 10, y + 2, x + 12, y, SSD1306_WHITE);
}

void drawStatusMark(int16_t x, int16_t y, bool connected) {
  display.drawCircle(x + 5, y + 5, 5, SSD1306_WHITE);
  if (connected) {
    display.drawLine(x + 2, y + 5, x + 4, y + 7, SSD1306_WHITE);
    display.drawLine(x + 4, y + 7, x + 8, y + 2, SSD1306_WHITE);
  } else {
    display.drawLine(x + 2, y + 2, x + 8, y + 8, SSD1306_WHITE);
    display.drawLine(x + 8, y + 2, x + 2, y + 8, SSD1306_WHITE);
  }
}

void showBootStatus(const String &step, uint8_t progress, const String &detail = "") {
  if (!displayReady) return;
  progress = min<uint8_t>(progress, 100);
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print("MIONE START");
  display.setCursor(96, 0);
  display.printf("%3u%%", progress);
  display.drawFastHLine(0, 10, 128, SSD1306_WHITE);
  display.setCursor(0, 18);
  display.print(step.substring(0, 21));
  display.setCursor(0, 32);
  display.print(detail.substring(0, 21));
  display.drawRect(0, 50, 128, 11, SSD1306_WHITE);
  if (progress > 0) display.fillRect(2, 52, (124UL * progress) / 100, 7, SSD1306_WHITE);
  display.display();
}

void renderUpdateStatus(const String &step, const String &detail, uint8_t progress) {
  if (!displayReady) return;
  progress = min<uint8_t>(progress, 100);
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print("OTA UPDATE");
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

void showUpdateStatus(const String &step, const String &detail, uint8_t progress) {
  updateState.message = step;
  updateState.detail = detail;
  updateState.progress = min<uint8_t>(progress, 100);
  if (!displayReady || (millis() - lastUpdateDisplayAt < 100 && progress < 100)) return;
  lastUpdateDisplayAt = millis();
  renderUpdateStatus(step, detail, progress);
}

void sendAlarmProgress(const String &body) {
  if (!config.alarmProgressEnabled) return;
  if (mqtt.connected() && !config.mqttUser.isEmpty()) {
    String topic = config.mqttUser + "/MiOne/AlarmStatus";
    mqtt.publish(topic.c_str(), body.c_str(), false);
  }
  if (activeAlarmSocket && activeAlarmSocket->connected()) activeAlarmSocket->println(body);
}

void showAlarmProgress(const String &alarmCode, const String &alarmText,
                       const String &action, const String &number, AlarmProgress progress) {
  String state = progress == AlarmProgress::starting ? "WIRD GESENDET" :
                 (progress == AlarmProgress::succeeded ? "OK" : "FEHLER");
  modemActivity = action + " " + state;
  modemActivityNumber = number;
  modemActivityUntil = millis() + (progress == AlarmProgress::starting ? 60000UL : 5000UL);
  queueSystemLog(progress == AlarmProgress::starting ? "ALARM_SEND_START" :
                 (progress == AlarmProgress::succeeded ? "ALARM_SEND_OK" : "ALARM_SEND_ERROR"),
                 "alarm=" + alarmCode + ",type=" + action + ",number=" + number);
  DynamicJsonDocument event(1024);
  event["type"] = "alarmProgress";
  event["modemImei"] = modem.imei();
  event["alarmCode"] = alarmCode;
  event["alarmText"] = alarmText;
  event["number"] = number;
  event["action"] = action;
  event["status"] = progress == AlarmProgress::starting ? "starting" :
                    (progress == AlarmProgress::succeeded ? "succeeded" : "failed");
  event["timestamp"] = clockText();
  String body;
  serializeJson(event, body);
  sendAlarmProgress(body);
  if (!displayReady) return;
  display.fillRect(0, 34, 128, 30, SSD1306_BLACK);
  display.drawFastHLine(0, 34, 128, SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 38);
  display.print(modemActivity.substring(0, 21));
  display.setCursor(0, 51);
  display.print(number.substring(0, 21));
  display.display();
}

void beginOfflineTcp() {
  if (!config.offlineTcpEnabled || !config.offlineTcpPort) return;
  wifiAlarmServer = new WiFiServer(config.offlineTcpPort);
  wifiAlarmServer->begin();
  if (config.ethernetEnabled) {
    ethernetAlarmServer = new Esp32EthernetServer(config.offlineTcpPort);
    ethernetAlarmServer->begin();
  }
  queueSystemLog("TCP_SERVER", "port=" + String(config.offlineTcpPort));
}

void closeAlarmSocket() {
  if (activeAlarmSocket) activeAlarmSocket->stop();
  activeAlarmSocket = nullptr;
  alarmSocketInput = "";
}

void processAlarmSocketLine(const String &line) {
  DynamicJsonDocument input(6144);
  DeserializationError parsed = deserializeJson(input, line);
  DynamicJsonDocument response(768);
  response["type"] = "alarmResult";
  response["modemImei"] = modem.imei();
  String result;
  bool ok = false;
  if (parsed) {
    result = String("JSON ungueltig: ") + parsed.c_str();
  } else if (input["mobile"].is<JsonArrayConst>()) {
    ok = alarmRouter.updateMobileSlots(input.as<JsonObjectConst>(), modem.imei(), result);
    response["type"] = "configResult";
  } else if (String(input["type"] | "") == "heartbeat") {
    String payloadImei = input["modemImei"] | "";
    if (payloadImei.isEmpty()) payloadImei = input["imei"] | "";
    ok = recordMioneHeartbeat(payloadImei, input["value"] | true, input["timestampUtc"] | "", "TCP", result);
    response["type"] = "heartbeatResult";
  } else {
    DateTime now = rtcReady ? rtc.now() : DateTime(2000, 1, 1, 0, 0, 0);
    uint32_t secondsOfDay = rtcReady ? now.hour() * 3600UL + now.minute() * 60UL + now.second() : UINT32_MAX;
    ok = alarmRouter.processAlarmPayload(input.as<JsonObjectConst>(), modem.imei(),
                                         config.commandSecret, secondsOfDay, result);
  }
  response["ok"] = ok;
  response["message"] = result;
  String body;
  serializeJson(response, body);
  if (activeAlarmSocket && activeAlarmSocket->connected()) activeAlarmSocket->println(body);
  queueSystemLog(ok ? "TCP_COMMAND" : "TCP_COMMAND_ERROR", result);
}

void maintainOfflineTcp() {
  if (!config.offlineTcpEnabled || !wifiAlarmServer) return;
  if (activeAlarmSocket && !activeAlarmSocket->connected()) closeAlarmSocket();
  if (!activeAlarmSocket) {
    WiFiClient incomingWifi = wifiAlarmServer->available();
    if (incomingWifi) {
      wifiAlarmClient = incomingWifi;
      activeAlarmSocket = &wifiAlarmClient;
    } else if (ethernetReady && ethernetAlarmServer) {
      EthernetClient incomingEthernet = ethernetAlarmServer->available();
      if (incomingEthernet) {
        ethernetAlarmClient = incomingEthernet;
        activeAlarmSocket = &ethernetAlarmClient;
      }
    }
  }
  if (!activeAlarmSocket) return;
  while (activeAlarmSocket->available()) {
    char value = static_cast<char>(activeAlarmSocket->read());
    if (value == '\n') {
      alarmSocketInput.trim();
      if (!alarmSocketInput.isEmpty()) processAlarmSocketLine(alarmSocketInput);
      alarmSocketInput = "";
    } else if (value != '\r') {
      if (alarmSocketInput.length() >= 6144) {
        activeAlarmSocket->println("{\"type\":\"alarmResult\",\"ok\":false,\"message\":\"Nachricht zu gross\"}");
        closeAlarmSocket();
        return;
      }
      alarmSocketInput += value;
    }
  }
}

void updateDisplay() {
  uint16_t refreshInterval = abs(currentButtonAdc - updateButtonIdle) > 100 ? 150 : 1000;
  if (!displayReady || millis() - lastDisplay < refreshInterval) return;
  lastDisplay = millis();
  display.clearDisplay();
  int32_t chipIdRemainingMs = static_cast<int32_t>(chipIdVisibleUntil - millis());
  if (chipIdRemainingMs > 0) {
    uint8_t remainingSeconds = static_cast<uint8_t>((chipIdRemainingMs + 999) / 1000);
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print("CHIP-ID / WEB-ZUGANG");
    display.drawFastHLine(0, 10, 128, SSD1306_WHITE);
    display.setCursor(27, 20);
    display.setTextSize(1);
    display.print(chipId());
    display.setCursor(0, 35);
    display.printf("Anzeige: %u Sekunden", remainingSeconds);
    display.drawRect(0, 52, 128, 9, SSD1306_WHITE);
    display.fillRect(2, 54, (124UL * remainingSeconds) / 60, 5, SSD1306_WHITE);
    display.display();
    return;
  }
  chipIdVisibleUntil = 0;
  display.setCursor(0, 0);
  display.printf("%s\n", clockText().c_str());
  display.drawFastHLine(0, 9, 128, SSD1306_WHITE);
  if (updateState.installing || updateState.failed) {
    renderUpdateStatus(updateState.message, updateState.detail, updateState.progress);
    return;
  }
  if (updateState.available) {
    String target = updateState.firmwareAvailable && updateState.recoveryAvailable ? "FW " + updateState.version + " REC " + updateState.recoveryVersion :
                    updateState.firmwareAvailable ? "Firmware " + updateState.version :
                    "Recovery " + updateState.recoveryVersion;
    display.setCursor(0, 13);
    display.printf("UPDATE VERFUEGBAR\n%s\nTaste 2s halten\nWeb / MQTT", target.c_str());
    display.display();
    return;
  }

  String networkName = "OFFLINE";
  String ip = "0.0.0.0";
  uint8_t networkSignal = 0;
  if (accessPoint) {
    networkName = "HOTSPOT";
    ip = WiFi.softAPIP().toString();
    drawHotspotIcon(0, 13);
  } else if (ethernetReady && Ethernet.linkStatus() == LinkON) {
    networkName = "ETHERNET";
    ip = Ethernet.localIP().toString();
    drawEthernetIcon(0, 13);
  } else if (WiFi.status() == WL_CONNECTED) {
    networkName = "WLAN";
    ip = WiFi.localIP().toString();
    networkSignal = wifiSignalLevel();
    drawWifiIcon(0, 13);
  } else if (modem.packetDataConnected()) {
    networkName = "MOBILFUNK";
    networkSignal = cellularSignalLevel();
    drawCellularIcon(0, 13);
  } else {
    drawStatusMark(1, 12, false);
  }
  display.setCursor(17, 13);
  display.print(networkName);
  if (networkSignal) drawSignalBars(110, 22, networkSignal);
  display.setCursor(0, 25);
  display.printf("IP %s", ip.c_str());

  if (static_cast<int32_t>(modemActivityUntil - millis()) > 0) {
    display.drawFastHLine(0, 35, 128, SSD1306_WHITE);
    display.setCursor(0, 38);
    display.print(modemActivity.substring(0, 21));
    display.setCursor(0, 51);
    display.print(modemActivityNumber.substring(0, 21));
    display.display();
    return;
  }
  modemActivityUntil = 0;

  display.setCursor(0, 38);
  display.print("LTE");
  drawSignalBars(24, 47, cellularSignalLevel());
  display.setCursor(57, 38);
  display.print("MQTT");
  drawStatusMark(92, 37, mqtt.connected());

  display.setCursor(0, 52);
  display.printf("SD:%s RTC:%s", sdReady ? "OK" : "--", rtcReady ? "OK" : "--");
  display.display();
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(100);
  config.setDefaults(chipId());
  String error;
  if (!configStore.begin(config, error)) Serial.println(error);
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();
  pinMode(BoardPins::buttonsAdc, INPUT);
  buttonSampleSum = 0;
  for (uint8_t i = 0; i < 10; ++i) {
    buttonSamples[i] = analogRead(BoardPins::buttonsAdc);
    buttonSampleSum += buttonSamples[i];
    delay(2);
  }
  currentButtonAdc = static_cast<int>(buttonSampleSum / 10);
  updateButtonIdle = currentButtonAdc;

  Wire.begin(BoardPins::i2cSda, BoardPins::i2cScl);
  if (config.displayEnabled) {
    displayReady = display.begin(SSD1306_SWITCHCAPVCC, BoardPins::oledAddress);
    if (displayReady) {
      display.setTextColor(SSD1306_WHITE);
      display.setTextSize(1);
      showBootStatus("Konfiguration", 8, "geladen");
    }
  }

  showBootStatus("Schnittstellen", 15, "SPI starten");
  beginSharedSpi();
  showBootStatus("SD-Karte", 22, config.sdEnabled ? "wird geprueft" : "deaktiviert");
  beginSd();
  recoverInterruptedWebUpdate();
  showBootStatus("SD-Karte", 30, !config.sdEnabled ? "deaktiviert" : (sdReady ? "bereit" : "FEHLER"));

  showBootStatus("WLAN / Hotspot", 37, "wird gestartet");
  beginWifi();
  showBootStatus("WLAN / Hotspot", 44,
                 accessPoint ? "Hotspot aktiv" : (config.wifiEnabled ? "WLAN gestartet" : "deaktiviert"));

  showBootStatus("Ethernet", 51, config.ethernetEnabled ? "wird gestartet" : "deaktiviert");
  beginEthernet();
  showBootStatus("Ethernet", 58,
                 !config.ethernetEnabled ? "deaktiviert" : (ethernetReady ? "bereit" : "nicht verbunden"));

  showBootStatus("Echtzeituhr", 64, "wird geprueft");
  rtcReady = rtc.begin();
  if (rtcReady && rtc.lostPower()) rtc.adjust(DateTime(__DATE__, __TIME__));
  showBootStatus("Echtzeituhr", 70, rtcReady ? "bereit" : "FEHLER");

  showBootStatus("Mobilfunkmodem", 76, config.cellularEnabled ? "wird erkannt" : "deaktiviert");
  modem.begin(config);
  showBootStatus("Mobilfunkmodem", 82,
                 !config.cellularEnabled ? "deaktiviert" :
                 (!modem.model().isEmpty() ? modem.model() + " erkannt" : "nicht erkannt"));

  showBootStatus("Alarmsteuerung", 87, "Daten laden");
  alarmRouter.setProgressCallback(showAlarmProgress);
  alarmRouter.begin();
  mqtt.setCallback(onMqtt);
  mqtt.setBufferSize(6144);
  beginOfflineTcp();
  showBootStatus("Webserver", 93, "wird gestartet");
  setupWeb();
  MDNS.begin(config.deviceId.c_str());
  showBootStatus("System bereit", 100, accessPoint ? "Config-Hotspot aktiv" : "Alarmierung aktiv");
  Preferences logPrefs;
  uint32_t bootCount = 1;
  if (logPrefs.begin("system-log", false)) {
    bootCount = logPrefs.getULong("bootCount", 0) + 1;
    logPrefs.putULong("bootCount", bootCount);
    logPrefs.end();
  }
  queueSystemLog("BOOT", "count=" + String(bootCount) +
                         ",reason=" + String(resetReasonName(esp_reset_reason())) +
                         ",firmware=" + BuildInfo::version + ",imei=" + modem.imei() +
                         ",model=" + modem.model());
  delay(500);
  Serial.printf("Firmware %s gestartet\n", BuildInfo::version);
}

void loop() {
  if (accessPoint) captiveDns.processNextRequest();
  web.handleClient();
  maintainOfflineTcp();
  modem.loop();
  bool primaryNetwork = WiFi.status() == WL_CONNECTED ||
                        (ethernetReady && Ethernet.linkStatus() == LinkON);
  modem.maintainDataFallback(!primaryNetwork && millis() > 30000);
  maintainMqtt();
  maintainButtons();
  maintainUpdates();
  maintainClock();
  if (ethernetReady) Ethernet.maintain();
  updateDisplay();
  maintainSystemLog();
  if (restartAt && static_cast<int32_t>(millis() - restartAt) >= 0) {
    queueSystemLog("REBOOT", "Geplanter Neustart");
    maintainSystemLog(true);
    ESP.restart();
  }
  delay(2);
}
