#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include "ModemService.h"

enum class AlarmDelivery : uint8_t { none, sms, call, both };
enum class AlarmProgress : uint8_t { starting, succeeded, failed };
using AlarmProgressCallback = void (*)(const String &alarmCode, const String &alarmText,
                                       const String &action, const String &number,
                                       AlarmProgress progress);

struct MobileSlot {
  bool active = false;
  String number;
  AlarmDelivery delivery = AlarmDelivery::none;
};

class AlarmRouter {
 public:
  explicit AlarmRouter(ModemService &modem) : modem_(modem) {}

  void begin();
  void setProgressCallback(AlarmProgressCallback callback) { progressCallback_ = callback; }
  bool updateMobileSlots(JsonObjectConst payload, const String &expectedImei, String &result,
                         bool *changed = nullptr);
  bool updateMobileSlot(uint8_t slot, JsonObjectConst payload, const String &expectedImei,
                        const String &secret, String &result);
  bool processAlarmPayload(JsonObjectConst payload, const String &expectedImei,
                           const String &secret, uint32_t secondsOfDay, String &result);
  void toJson(JsonObject output) const;

 private:
  bool processAlarm(JsonObjectConst alarm, const String &rootImei, uint32_t secondsOfDay,
                    uint16_t &sent, uint16_t &skipped);
  bool technicalAllowed(uint32_t secondsOfDay) const;
  bool alreadyProcessed(const String &fingerprint) const;
  void rememberProcessed(const String &fingerprint);
  void loadSlots();
  void saveSlot(uint8_t index);
  void saveTechnicalWindow();
  void loadSeen();
  void saveSeen();

  ModemService &modem_;
  AlarmProgressCallback progressCallback_ = nullptr;
  MobileSlot slots_[5];
  uint32_t technicalFrom_ = 0;
  uint32_t technicalUntil_ = 86400;
  String seen_[12];
  uint8_t seenCount_ = 0;
  uint8_t seenNext_ = 0;
};
