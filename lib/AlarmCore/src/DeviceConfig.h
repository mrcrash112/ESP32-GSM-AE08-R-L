#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

struct IPv4Config {
  bool dhcp = true;
  String address;
  String gateway;
  String subnet = "255.255.255.0";
  String dns = "1.1.1.1";
};

struct InputAlarmRecipient {
  String number;
  uint8_t delivery = 0;
};

struct InputAlarmConfig {
  bool enabled = false;
  bool alarmOnClose = true;
  String text;
  uint16_t delaySeconds = 0;
  InputAlarmRecipient recipients[5];
};

struct DeviceConfig {
  uint16_t schema = 1;
  String deviceId;
  bool provisioned = false;

  bool wifiEnabled = false;
  String wifiSsid;
  String wifiPassword;
  IPv4Config wifiIp;

  bool ethernetEnabled = false;
  IPv4Config ethernetIp;

  bool sdEnabled = false;
  bool rtcEnabled = false;
  bool displayEnabled = false;
  uint16_t logIntervalSeconds = 10;
  bool cellularEnabled = true;
  String simPin;
  String apn;
  String apnUser;
  String apnPassword;

  bool mqttEnabled = false;
  String mqttHost;
  uint16_t mqttPort = 1883;
  String mqttUser;
  String mqttPassword;
  String mqttBaseTopic = "mione";

  bool offlineTcpEnabled = false;
  String offlineTcpHost;
  uint16_t offlineTcpPort = 0;
  String commandSecret;
  bool alarmProgressEnabled = false;

  InputAlarmConfig inputAlarms[4];

  String webUser = "admin";
  String webPassword;

  bool updateCheckEnabled = false;
  bool updateCellularDownloads = false;
  bool updateAutoInstall = false;
  uint16_t updateInstallMinute = 180;
  String updateChannel = "stable";
  String updateManifestUrl;
  uint16_t updateCheckMinutes = 360;

  void setDefaults(const String &chipId);
  void toJson(JsonObject root, bool includeSecrets) const;
  bool fromJson(JsonObjectConst root, String &error);
  bool validate(String &error) const;
};
