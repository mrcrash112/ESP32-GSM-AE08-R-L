#include "ConfigStore.h"

#include <Preferences.h>

namespace {
constexpr const char *nameSpace = "alarm-config";
constexpr const char *configKey = "json";
}

bool ConfigStore::begin(DeviceConfig &config, String &error) {
  Preferences prefs;
  if (!prefs.begin(nameSpace, true)) {
    error = "NVS konnte nicht geoeffnet werden";
    return false;
  }
  String json = prefs.getString(configKey, "");
  prefs.end();
  if (json.isEmpty()) return true;

  DynamicJsonDocument doc(4096);
  DeserializationError result = deserializeJson(doc, json);
  if (result) {
    error = String("Config-JSON defekt: ") + result.c_str();
    return false;
  }
  return config.fromJson(doc.as<JsonObjectConst>(), error);
}

bool ConfigStore::save(const DeviceConfig &config, String &error) {
  if (!config.validate(error)) return false;
  DynamicJsonDocument doc(4096);
  config.toJson(doc.to<JsonObject>(), true);
  String json;
  serializeJson(doc, json);
  Preferences prefs;
  if (!prefs.begin(nameSpace, false)) {
    error = "NVS konnte nicht geoeffnet werden";
    return false;
  }
  size_t written = prefs.putString(configKey, json);
  prefs.end();
  if (written != json.length()) {
    error = "Config konnte nicht vollstaendig gespeichert werden";
    return false;
  }
  return true;
}

void ConfigStore::clear() {
  Preferences prefs;
  if (prefs.begin(nameSpace, false)) {
    prefs.clear();
    prefs.end();
  }
}
