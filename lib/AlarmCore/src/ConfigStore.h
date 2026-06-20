#pragma once

#include "DeviceConfig.h"

class ConfigStore {
 public:
  bool begin(DeviceConfig &config, String &error);
  bool save(const DeviceConfig &config, String &error);
  void clear();
};
