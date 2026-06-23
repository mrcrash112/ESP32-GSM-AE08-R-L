#include "DeviceConfig.h"

#include <IPAddress.h>

namespace {
void ipToJson(JsonObject out, const IPv4Config &ip) {
  out["dhcp"] = ip.dhcp;
  out["address"] = ip.address;
  out["gateway"] = ip.gateway;
  out["subnet"] = ip.subnet;
  out["dns"] = ip.dns;
}

void ipFromJson(JsonObjectConst in, IPv4Config &ip) {
  if (in.isNull()) return;
  ip.dhcp = in["dhcp"] | ip.dhcp;
  ip.address = in["address"] | ip.address;
  ip.gateway = in["gateway"] | ip.gateway;
  ip.subnet = in["subnet"] | ip.subnet;
  ip.dns = in["dns"] | ip.dns;
}

bool validIp(const String &value) {
  IPAddress parsed;
  return parsed.fromString(value);
}

bool validSystemId(const String &value) {
  if (value.isEmpty()) return false;
  if (value.indexOf('/') >= 0) return false;
  if (value.indexOf('#') >= 0 || value.indexOf('+') >= 0) return false;
  return true;
}

bool validMqttTopicRoot(const String &value) {
  if (value.isEmpty()) return true;
  if (value.startsWith("/") || value.endsWith("/")) return false;
  if (value.indexOf('#') >= 0 || value.indexOf('+') >= 0) return false;
  if (value.indexOf("//") >= 0) return false;
  return true;
}

bool safeAtValue(const String &value) {
  return value.indexOf('"') < 0 && value.indexOf('\r') < 0 && value.indexOf('\n') < 0;
}

String timeText(uint16_t minuteOfDay) {
  char value[8];
  snprintf(value, sizeof(value), "%02u:%02u", minuteOfDay / 60, minuteOfDay % 60);
  return value;
}

bool parseTimeText(const String &value, uint16_t &minuteOfDay) {
  if (value.length() != 5 || value.charAt(2) != ':') return false;
  int hour = value.substring(0, 2).toInt();
  int minute = value.substring(3, 5).toInt();
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59) return false;
  minuteOfDay = static_cast<uint16_t>(hour * 60 + minute);
  return true;
}

void secret(JsonObject out, const char *key, const String &value, bool include) {
  out[key] = include ? value : (value.isEmpty() ? "" : "***");
}

bool validInputDelay(uint16_t seconds) {
  return seconds == 0 || seconds == 300 || seconds == 600 || seconds == 900;
}
}  // namespace

void DeviceConfig::setDefaults(const String &chipId) {
  deviceId = "alarm-" + chipId;
  webPassword = chipId;
  commandSecret = chipId;
  mqttHost = "194.164.51.139";
  mqttPort = 1883;
  mqttUser = "mqtt_master_key";
  mqttPassword = "dyjpaz-xakfip-1guwdI";
  updateManifestUrl = "https://github.com/mrcrash112/ESP32-GSM-AE08-R-L/releases/latest/download/firmware.json";
}

void DeviceConfig::toJson(JsonObject root, bool includeSecrets) const {
  root["schema"] = schema;
  root["deviceId"] = deviceId;
  root["provisioned"] = provisioned;

  JsonObject wifi = root.createNestedObject("wifi");
  wifi["enabled"] = wifiEnabled;
  wifi["ssid"] = wifiSsid;
  secret(wifi, "password", wifiPassword, includeSecrets);
  ipToJson(wifi.createNestedObject("ip"), wifiIp);

  JsonObject ethernet = root.createNestedObject("ethernet");
  ethernet["enabled"] = ethernetEnabled;
  ipToJson(ethernet.createNestedObject("ip"), ethernetIp);

  JsonObject hardware = root.createNestedObject("hardware");
  hardware["sd"] = sdEnabled;
  hardware["rtc"] = rtcEnabled;
  hardware["display"] = displayEnabled;

  JsonObject logging = root.createNestedObject("logging");
  logging["intervalSeconds"] = logIntervalSeconds;

  JsonObject cellular = root.createNestedObject("cellular");
  cellular["enabled"] = cellularEnabled;
  secret(cellular, "simPin", simPin, includeSecrets);
  cellular["apn"] = apn;
  cellular["user"] = apnUser;
  secret(cellular, "password", apnPassword, includeSecrets);

  JsonObject mqtt = root.createNestedObject("mqtt");
  mqtt["enabled"] = mqttEnabled;
  mqtt["host"] = mqttHost;
  mqtt["port"] = mqttPort;
  mqtt["user"] = mqttUser;
  secret(mqtt, "password", mqttPassword, includeSecrets);
  mqtt["baseTopic"] = mqttBaseTopic;
  mqtt["topTopic"] = mqttTopTopic;

  JsonObject tcp = root.createNestedObject("offlineTcp");
  tcp["enabled"] = offlineTcpEnabled;
  tcp["host"] = offlineTcpHost;
  tcp["port"] = offlineTcpPort;
  secret(tcp, "secret", commandSecret, includeSecrets);

  JsonObject notifications = root.createNestedObject("notifications");
  notifications["alarmProgress"] = alarmProgressEnabled;

  JsonArray alarmInputs = root.createNestedArray("alarmInputs");
  for (uint8_t i = 0; i < 4; ++i) {
    JsonObject input = alarmInputs.createNestedObject();
    input["index"] = i + 1;
    input["enabled"] = inputAlarms[i].enabled;
    input["trigger"] = inputAlarms[i].alarmOnClose ? "close" : "open";
    input["text"] = inputAlarms[i].text;
    input["delaySeconds"] = inputAlarms[i].delaySeconds;
    JsonArray recipients = input.createNestedArray("recipients");
    for (uint8_t slot = 0; slot < 5; ++slot) {
      JsonObject recipient = recipients.createNestedObject();
      recipient["number"] = inputAlarms[i].recipients[slot].number;
      recipient["delivery"] = inputAlarms[i].recipients[slot].delivery;
    }
  }

  JsonObject web = root.createNestedObject("web");
  web["user"] = webUser;
  secret(web, "password", webPassword, includeSecrets);

  JsonObject update = root.createNestedObject("update");
  update["checkEnabled"] = updateCheckEnabled;
  update["cellularDownloads"] = updateCellularDownloads;
  update["autoInstall"] = updateAutoInstall;
  update["installTime"] = timeText(updateInstallMinute);
  update["channel"] = updateChannel;
  update["manifestUrl"] = updateManifestUrl;
  update["checkMinutes"] = updateCheckMinutes;
}

bool DeviceConfig::fromJson(JsonObjectConst root, String &error) {
  schema = root["schema"] | schema;
  deviceId = root["deviceId"] | deviceId;
  provisioned = root["provisioned"] | provisioned;

  JsonObjectConst wifi = root["wifi"];
  wifiEnabled = wifi["enabled"] | wifiEnabled;
  wifiSsid = wifi["ssid"] | wifiSsid;
  if (wifi["password"].is<const char *>() && wifi["password"] != "***") wifiPassword = wifi["password"].as<String>();
  ipFromJson(wifi["ip"], wifiIp);

  JsonObjectConst ethernet = root["ethernet"];
  ethernetEnabled = ethernet["enabled"] | ethernetEnabled;
  ipFromJson(ethernet["ip"], ethernetIp);

  JsonObjectConst hardware = root["hardware"];
  sdEnabled = hardware["sd"] | sdEnabled;
  rtcEnabled = hardware["rtc"] | rtcEnabled;
  displayEnabled = hardware["display"] | displayEnabled;

  JsonObjectConst logging = root["logging"];
  logIntervalSeconds = logging["intervalSeconds"] | logIntervalSeconds;

  JsonObjectConst cellular = root["cellular"];
  cellularEnabled = true;
  if (cellular["simPin"].is<const char *>() && cellular["simPin"] != "***") simPin = cellular["simPin"].as<String>();
  apn = cellular["apn"] | apn;
  apnUser = cellular["user"] | apnUser;
  if (cellular["password"].is<const char *>() && cellular["password"] != "***") apnPassword = cellular["password"].as<String>();

  JsonObjectConst mqtt = root["mqtt"];
  mqttEnabled = mqtt["enabled"] | mqttEnabled;
  mqttHost = mqtt["host"] | mqttHost;
  mqttPort = mqtt["port"] | mqttPort;
  mqttUser = mqtt["user"] | mqttUser;
  if (mqtt["password"].is<const char *>() && mqtt["password"] != "***") mqttPassword = mqtt["password"].as<String>();
  mqttBaseTopic = mqtt["baseTopic"] | mqttBaseTopic;
  mqttBaseTopic.trim();
  mqttTopTopic = mqtt["topTopic"] | mqttTopTopic;
  mqttTopTopic.trim();

  JsonObjectConst tcp = root["offlineTcp"];
  offlineTcpEnabled = tcp["enabled"] | offlineTcpEnabled;
  offlineTcpHost = tcp["host"] | offlineTcpHost;
  offlineTcpPort = tcp["port"] | offlineTcpPort;
  if (tcp["secret"].is<const char *>() && tcp["secret"] != "***") commandSecret = tcp["secret"].as<String>();

  JsonObjectConst notifications = root["notifications"];
  alarmProgressEnabled = notifications["alarmProgress"] | alarmProgressEnabled;

  JsonArrayConst alarmInputs = root["alarmInputs"];
  if (!alarmInputs.isNull()) {
    for (uint8_t i = 0; i < 4; ++i) inputAlarms[i] = InputAlarmConfig();
    for (JsonObjectConst input : alarmInputs) {
      int index = input["index"] | 0;
      if (index < 1 || index > 4) continue;
      InputAlarmConfig &target = inputAlarms[index - 1];
      target.enabled = input["enabled"] | target.enabled;
      String trigger = input["trigger"] | "";
      trigger.toLowerCase();
      target.alarmOnClose = trigger == "open" ? false : true;
      target.text = input["text"] | "";
      if (target.text.length() > 160) target.text = target.text.substring(0, 160);
      target.delaySeconds = input["delaySeconds"] | target.delaySeconds;
      JsonArrayConst recipients = input["recipients"];
      uint8_t slot = 0;
      for (JsonObjectConst recipient : recipients) {
        if (slot >= 5) break;
        target.recipients[slot].number = recipient["number"] | "";
        int delivery = recipient["delivery"] | 0;
        target.recipients[slot].delivery = delivery < 0 || delivery > 3 ? 0 : static_cast<uint8_t>(delivery);
        ++slot;
      }
    }
  }

  JsonObjectConst web = root["web"];
  webUser = web["user"] | webUser;
  if (web["password"].is<const char *>() && web["password"] != "***") webPassword = web["password"].as<String>();

  JsonObjectConst update = root["update"];
  updateCheckEnabled = update["checkEnabled"] | updateCheckEnabled;
  updateCellularDownloads = update["cellularDownloads"] | updateCellularDownloads;
  updateAutoInstall = update["autoInstall"] | updateAutoInstall;
  if (update["installTime"].is<const char *>()) {
    uint16_t parsedMinute = updateInstallMinute;
    if (parseTimeText(update["installTime"].as<String>(), parsedMinute)) updateInstallMinute = parsedMinute;
  }
  updateChannel = update["channel"] | updateChannel;
  updateManifestUrl = update["manifestUrl"] | updateManifestUrl;
  updateCheckMinutes = update["checkMinutes"] | updateCheckMinutes;
  return validate(error);
}

bool DeviceConfig::validate(String &error) const {
  if (schema != 1) error = "Nicht unterstuetzte Config-Version";
  else if (deviceId.isEmpty() || deviceId.length() > 48) error = "deviceId ist ungueltig";
  else if (!wifiIp.dhcp && (!validIp(wifiIp.address) || !validIp(wifiIp.gateway) || !validIp(wifiIp.subnet))) error = "Statische WLAN-IP ist ungueltig";
  else if (!ethernetIp.dhcp && (!validIp(ethernetIp.address) || !validIp(ethernetIp.gateway) || !validIp(ethernetIp.subnet))) error = "Statische Ethernet-IP ist ungueltig";
  else if (mqttEnabled && (!mqttHost.isEmpty() && mqttPort == 0)) error = "MQTT-Port ist ungueltig";
  else if (mqttUser.indexOf('#') >= 0 || mqttUser.indexOf('+') >= 0 || mqttUser.indexOf('/') >= 0) error = "MQTT-Benutzername darf keine Topic-Sonderzeichen enthalten";
  else if (mqttEnabled && !validSystemId(mqttBaseTopic)) error = "System-ID fehlt oder ist ungueltig";
  else if (!validMqttTopicRoot(mqttTopTopic)) error = "TopTopic darf keine Topic-Sonderzeichen enthalten";
  else if (offlineTcpEnabled && offlineTcpPort == 0) error = "Offline-TCP-Port fehlt";
  else if (!safeAtValue(apn) || !safeAtValue(apnUser) || !safeAtValue(apnPassword)) error = "Mobilfunk-Zugangsdaten enthalten ungueltige Zeichen";
  else if (logIntervalSeconds < 10 || logIntervalSeconds > 3600) error = "Log-Intervall muss zwischen 10 und 3600 Sekunden liegen";
  else {
    for (uint8_t i = 0; i < 4; ++i) {
      if (inputAlarms[i].text.length() > 160) {
        error = "Alarmtext fuer Digitaleingang " + String(i + 1) + " ist zu lang";
        return false;
      }
      if (!validInputDelay(inputAlarms[i].delaySeconds)) {
        error = "Verzoegerungszeit fuer Digitaleingang " + String(i + 1) + " ist ungueltig";
        return false;
      }
      for (uint8_t slot = 0; slot < 5; ++slot) {
        const InputAlarmRecipient &recipient = inputAlarms[i].recipients[slot];
        if (recipient.delivery > 3) {
          error = "Alarmierungsweg fuer Digitaleingang " + String(i + 1) + " ist ungueltig";
          return false;
        }
        if (recipient.number.length() > 32) {
          error = "Rufnummer fuer Digitaleingang " + String(i + 1) + " ist zu lang";
          return false;
        }
      }
    }
    if (webUser.isEmpty() || webPassword.length() < 8) error = "Web-Zugang benoetigt ein Passwort mit mindestens 8 Zeichen";
    else if (commandSecret.length() < 8) error = "Befehls-Secret muss mindestens 8 Zeichen lang sein";
    else if (updateInstallMinute >= 1440) error = "Installationszeit fuer Updates ist ungueltig";
    else if (updateChannel != "stable" && updateChannel != "beta") error = "Firmware-Kanal muss stable oder beta sein";
    else if (updateCheckEnabled && !updateManifestUrl.isEmpty() && !updateManifestUrl.startsWith("https://github.com/") && !updateManifestUrl.startsWith("https://raw.githubusercontent.com/")) error = "Update-Manifest muss von GitHub per HTTPS geladen werden";
    else if (updateCheckMinutes < 5) error = "Update-Pruefintervall muss mindestens 5 Minuten betragen";
    else return true;
    return false;
  }
  return false;
}
