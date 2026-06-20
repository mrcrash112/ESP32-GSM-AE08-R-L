#pragma once

#include <Arduino.h>

#include "DeviceConfig.h"

class ModemService {
 public:
  void begin(const DeviceConfig &config);
  void loop();
  void maintainDataFallback(bool required);
  bool getNetworkTime(String &response);
  bool downloadToFile(const String &url, const char *path, String &error);
  bool sendSms(const String &number, const String &message);
  bool ring(const String &number, uint16_t seconds);
  bool connected() const { return registered_; }
  bool packetDataConnected() const { return packetDataConnected_; }
  int signalQuality() const { return signalQuality_; }
  String modemName() const { return modemName_; }
  String model() const { return modemType_; }
  String imei() const { return imei_; }
  String networkOperator() const { return networkOperator_; }

 private:
  bool command(const String &value, const char *expected, uint32_t timeout = 2000);
  String readUntil(uint32_t timeout);
  bool waitFor(const String &token, String &answer, uint32_t timeout);
  bool readBinaryToFile(const char *path, size_t size, String &error);
  bool downloadEc25(const String &url, const char *path, String &error);
  bool downloadSim7500(const String &url, const char *path, String &error);
  void pollStatus();

  HardwareSerial serial_{2};
  bool enabled_ = false;
  bool registered_ = false;
  bool packetDataConnected_ = false;
  int signalQuality_ = -1;
  String modemName_;
  String imei_;
  String modemType_;
  String networkOperator_;
  String apn_;
  String apnUser_;
  String apnPassword_;
  uint32_t lastPoll_ = 0;
  uint32_t lastDataAttempt_ = 0;
};
