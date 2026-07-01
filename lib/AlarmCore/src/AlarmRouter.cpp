#include "AlarmRouter.h"

#include <Preferences.h>

namespace {
constexpr const char *preferencesNamespace = "alarm-route";

String firstString(JsonObjectConst object, const char *a, const char *b = nullptr,
                   const char *c = nullptr, const char *d = nullptr) {
  const char *keys[] = {a, b, c, d};
  for (const char *key : keys) {
    if (!key || object[key].isNull()) continue;
    JsonVariantConst value = object[key];
    if (value.is<const char *>() || value.is<long>() || value.is<unsigned long>() ||
        value.is<long long>() || value.is<unsigned long long>()) return value.as<String>();
  }
  return "";
}

bool firstBool(JsonObjectConst object, bool fallback, const char *a, const char *b = nullptr,
               const char *c = nullptr) {
  const char *keys[] = {a, b, c};
  for (const char *key : keys) {
    if (!key || object[key].isNull()) continue;
    JsonVariantConst value = object[key];
    if (value.is<bool>()) return value.as<bool>();
    if (value.is<int>()) return value.as<int>() != 0;
    if (value.is<const char *>()) {
      String text = value.as<String>();
      text.trim();
      text.toLowerCase();
      return text == "true" || text == "1" || text == "aktiv" || text == "active" || text == "on";
    }
  }
  return fallback;
}

bool validPhoneNumber(const String &number) {
  if (number.length() < 3 || number.length() > 24) return false;
  for (size_t i = 0; i < number.length(); ++i) {
    char value = number[i];
    if ((value < '0' || value > '9') && !(i == 0 && value == '+')) return false;
  }
  return true;
}

String normalizePhoneNumber(String number) {
  number.trim();
  String normalized;
  normalized.reserve(number.length());
  for (size_t i = 0; i < number.length(); ++i) {
    char value = number[i];
    if (value >= '0' && value <= '9') normalized += value;
    else if (value == '+' && normalized.isEmpty()) normalized += value;
    else if (value != ' ' && value != '-' && value != '/' && value != '(' && value != ')') return number;
  }
  return normalized;
}

AlarmDelivery parseDelivery(String value) {
  value.trim();
  value.toLowerCase();
  if (value == "sms" || value == "nachricht") return AlarmDelivery::sms;
  if (value == "anruf" || value == "anrufen" || value == "call") return AlarmDelivery::call;
  if (value == "beides" || value == "both" || value == "sms+anruf" || value == "sms+call" ||
      value == "anruf/nachricht") return AlarmDelivery::both;
  return AlarmDelivery::none;
}

AlarmDelivery parseAlarmsTo(int value) {
  if (value == 0) return AlarmDelivery::call;
  if (value == 1) return AlarmDelivery::sms;
  if (value == 2) return AlarmDelivery::both;
  return AlarmDelivery::none;
}

const char *deliveryName(AlarmDelivery delivery) {
  switch (delivery) {
    case AlarmDelivery::sms: return "sms";
    case AlarmDelivery::call: return "Anrufen";
    case AlarmDelivery::both: return "beides";
    default: return "aus";
  }
}

String phoneText(const String &number) { return number.isEmpty() ? "leer" : number; }

String timeText(uint32_t seconds) {
  if (seconds >= 86400) seconds = 0;
  char value[9];
  snprintf(value, sizeof(value), "%02lu:%02lu:%02lu",
           static_cast<unsigned long>(seconds / 3600),
           static_cast<unsigned long>((seconds % 3600) / 60),
           static_cast<unsigned long>(seconds % 60));
  return value;
}

void appendChange(String &changes, const String &change) {
  if (!changes.isEmpty()) changes += " | ";
  changes += change;
}

String normalizePriority(JsonObjectConst alarm) {
  String value = firstString(alarm, "priority", "Priority", "alarmType", "AlarmType");
  if (value.isEmpty()) value = firstString(alarm, "type", "Typ");
  if (value.isEmpty()) value = firstString(alarm, "prioritaet");
  value.trim();
  value.toLowerCase();
  return value;
}
}  // namespace

void AlarmRouter::begin() {
  loadSlots();
  Preferences prefs;
  if (prefs.begin(preferencesNamespace, true)) {
    technicalFrom_ = prefs.getULong("techFrom", 0);
    technicalUntil_ = prefs.getULong("techUntil", 86400);
    prefs.end();
  }
  loadSeen();
}

bool AlarmRouter::updateMobileSlots(JsonObjectConst payload, const String &expectedImei,
                                    String &result, bool *changed) {
  if (changed) *changed = false;
  String payloadImei = payload["modemImei"] | "";
  if (expectedImei.isEmpty() || payloadImei != expectedImei) {
    result = "Modem-IMEI der MiOne-Konfiguration stimmt nicht";
    return false;
  }
  JsonArrayConst mobiles = payload["mobile"].as<JsonArrayConst>();
  if (mobiles.isNull()) {
    result = "MiOne-Mobile-Array fehlt";
    return false;
  }

  MobileSlot imported[5];
  bool found[5] = {};
  uint8_t count = 0;
  bool windowFound = false;
  uint32_t importedFrom = technicalFrom_;
  uint32_t importedUntil = technicalUntil_;
  for (JsonObjectConst source : mobiles) {
    int slot = source["slot"] | 0;
    if (slot < 1 || slot > 5 || found[slot - 1]) continue;
    MobileSlot &candidate = imported[slot - 1];
    candidate.active = source["aktiv"] | false;
    candidate.number = normalizePhoneNumber(source["nummer"] | "");
    candidate.delivery = source["alarmsTo"].is<int>()
                             ? parseAlarmsTo(source["alarmsTo"].as<int>())
                             : parseDelivery(source["alarmierung"] | "");
    if (candidate.active && !validPhoneNumber(candidate.number)) {
      result = "Ungueltige Rufnummer in Mobile Slot " + String(slot);
      return false;
    }
    uint32_t from = source["technicalAlarmMessagingFrom"] | importedFrom;
    uint32_t until = source["technicalAlarmMessagingUntil"] | importedUntil;
    if (from > 86399 || until > 86400) {
      result = "Technical-Zeitfenster ist ungueltig";
      return false;
    }
    if (!windowFound) {
      importedFrom = from;
      importedUntil = until;
      windowFound = true;
    }
    found[slot - 1] = true;
    ++count;
  }
  if (!count) {
    result = "MiOne-Konfiguration enthaelt keine gueltigen Slots";
    return false;
  }
  String changes;
  for (uint8_t i = 0; i < 5; ++i) {
    MobileSlot next = found[i] ? imported[i] : MobileSlot();
    MobileSlot &current = slots_[i];
    if (current.active == next.active && current.number == next.number &&
        current.delivery == next.delivery) continue;
    String detail = "Slot " + String(i + 1) + ":";
    bool hasDetail = false;
    if (current.number != next.number) {
      detail += " Nummer " + phoneText(current.number) + " -> " + phoneText(next.number);
      hasDetail = true;
    }
    if (current.active != next.active) {
      detail += String(hasDetail ? "," : "") + " Aktiv " +
                (current.active ? "ja" : "nein") + " -> " + (next.active ? "ja" : "nein");
      hasDetail = true;
    }
    if (current.delivery != next.delivery) {
      detail += String(hasDetail ? "," : "") + " Alarmierung " +
                deliveryName(current.delivery) + " -> " + deliveryName(next.delivery);
    }
    appendChange(changes, detail);
    current = next;
    saveSlot(i);
  }
  if (technicalFrom_ != importedFrom || technicalUntil_ != importedUntil) {
    appendChange(changes, "Technical-Zeitfenster: " + timeText(technicalFrom_) + "-" +
                              timeText(technicalUntil_) + " -> " + timeText(importedFrom) + "-" +
                              timeText(importedUntil));
    technicalFrom_ = importedFrom;
    technicalUntil_ = importedUntil;
    saveTechnicalWindow();
  }
  if (changes.isEmpty()) {
    result = "Keine Aenderung; " + String(count) + " Mobile Slots geprueft";
  } else {
    result = changes;
    if (changed) *changed = true;
  }
  return true;
}

bool AlarmRouter::updateMobileSlot(uint8_t slot, JsonObjectConst payload,
                                   const String &expectedImei, const String &secret,
                                   String &result) {
  if (slot < 1 || slot > 5) {
    result = "Mobile Slot ist ungueltig";
    return false;
  }
  String payloadImei = payload["imei"] | "";
  String payloadSecret = payload["secret"] | "";
  if (expectedImei.isEmpty() || payloadImei != expectedImei || payloadSecret != secret) {
    result = "IMEI oder Secret stimmt nicht";
    return false;
  }

  JsonObjectConst source = payload["mobile"].is<JsonObjectConst>()
                               ? payload["mobile"].as<JsonObjectConst>()
                               : (payload["value"].is<JsonObjectConst>()
                                      ? payload["value"].as<JsonObjectConst>()
                                      : payload);
  MobileSlot candidate;
  candidate.active = firstBool(source, false, "active", "enabled", "Aktiv") ||
                     firstBool(source, false, "isActive", "IsActive");
  candidate.number = firstString(source, "number", "phone", "Mobilnummer", "Nummer");
  if (candidate.number.isEmpty()) candidate.number = firstString(source, "mobile", "MobileNumber", "PhoneNumber");
  candidate.number = normalizePhoneNumber(candidate.number);
  String delivery = firstString(source, "notification", "delivery", "Alarmierung", "Typ");
  if (delivery.isEmpty()) delivery = firstString(source, "notificationType", "MessageType", "type");
  candidate.delivery = parseDelivery(delivery);
  if (candidate.active && (!validPhoneNumber(candidate.number) || candidate.delivery == AlarmDelivery::none)) {
    result = "Aktiver Mobile Slot benoetigt Nummer und Alarmierungsart";
    return false;
  }
  slots_[slot - 1] = candidate;
  saveSlot(slot - 1);
  result = "Mobile Slot " + String(slot) + " gespeichert";
  return true;
}

bool AlarmRouter::processAlarmPayload(JsonObjectConst payload, const String &expectedImei,
                                      const String &secret, uint32_t secondsOfDay,
                                      String &result) {
  String rootImei = payload["imei"] | "";
  if (rootImei.isEmpty()) rootImei = payload["modemImei"] | "";
  if (rootImei.isEmpty()) rootImei = payload["IMEI"] | "";
  String payloadSecret = payload["secret"] | "";
  bool mionePayload = payload["modemImei"].is<const char *>() || payload["IMEI"].is<const char *>();
  bool authorized = mionePayload ? rootImei == expectedImei : payloadSecret == secret;
  if (expectedImei.isEmpty() || !authorized || rootImei != expectedImei) {
    result = "IMEI oder Secret stimmt nicht";
    return false;
  }

  JsonObjectConst settings = payload["int"]["preferences"]["user"]["alarmssettings"];
  if (settings.isNull() && payload["int.preferences.user.alarmssettings"].is<JsonObjectConst>()) {
    settings = payload["int.preferences.user.alarmssettings"].as<JsonObjectConst>();
  }
  if (!settings.isNull()) {
    uint32_t from = settings["TechnicalAlarmMessagingFrom"] | technicalFrom_;
    uint32_t until = settings["TechnicalAlarmMessagingUntil"] | technicalUntil_;
    if (from > 86399 || until > 86400) {
      result = "Technical-Zeitfenster ist ungueltig";
      return false;
    }
    technicalFrom_ = from;
    technicalUntil_ = until;
    saveTechnicalWindow();
  }

  uint16_t sent = 0;
  uint16_t skipped = 0;
  JsonArrayConst alarms = payload["alarms"].is<JsonArrayConst>()
                              ? payload["alarms"].as<JsonArrayConst>()
                              : payload["Alarme"].as<JsonArrayConst>();
  if (!alarms.isNull()) {
    for (JsonObjectConst alarm : alarms) processAlarm(alarm, rootImei, secondsOfDay, sent, skipped);
  } else {
    JsonObjectConst alarmMap = payload["alarms"].is<JsonObjectConst>()
                                   ? payload["alarms"].as<JsonObjectConst>()
                                   : payload["Alarme"].as<JsonObjectConst>();
    if (!alarmMap.isNull()) {
      for (JsonPairConst entry : alarmMap) {
        if (!entry.value().is<JsonObjectConst>()) continue;
        DynamicJsonDocument normalized(1536);
        normalized.set(entry.value());
        if (normalized["id"].isNull()) normalized["id"] = entry.key().c_str();
        processAlarm(normalized.as<JsonObjectConst>(), rootImei, secondsOfDay, sent, skipped);
      }
    } else processAlarm(payload, rootImei, secondsOfDay, sent, skipped);
  }
  result = String(sent) + " Alarmierungen gesendet, " + String(skipped) + " uebersprungen";
  return sent > 0 || skipped > 0;
}

bool AlarmRouter::processAlarm(JsonObjectConst alarm, const String &rootImei,
                               uint32_t secondsOfDay, uint16_t &sent, uint16_t &skipped) {
  String alarmImei = alarm["imei"] | rootImei;
  if (alarm["modemImei"].is<const char *>()) alarmImei = alarm["modemImei"].as<String>();
  if (alarm["IMEI"].is<const char *>()) alarmImei = alarm["IMEI"].as<String>();
  if (alarmImei.isEmpty() || alarmImei != modem_.imei()) {
    ++skipped;
    return false;
  }
  if (!firstBool(alarm, true, "active", "triggered", "Aktiv")) {
    ++skipped;
    return false;
  }
  String priority = normalizePriority(alarm);
  if (priority != "urgent" && priority != "technical") {
    ++skipped;
    return false;
  }
  if (priority == "technical" && !technicalAllowed(secondsOfDay)) {
    ++skipped;
    return false;
  }
  String id = firstString(alarm, "id", "alarmId", "AlarmId", "ID");
  if (id.isEmpty()) id = firstString(alarm, "alarmCode");
  String revision = firstString(alarm, "revision", "updatedAt", "Revision");
  if (revision.isEmpty()) {
    String date = firstString(alarm, "datum");
    String time = firstString(alarm, "uhrzeit");
    if (!date.isEmpty() || !time.isEmpty()) revision = date + "T" + time;
  }
  if (id.isEmpty() || id.length() > 64 || revision.length() > 64) {
    ++skipped;
    return false;
  }
  String fingerprint = id + ":" + revision;
  if (alreadyProcessed(fingerprint)) {
    ++skipped;
    return false;
  }
  String message = firstString(alarm, "message", "text", "Nachricht", "title");
  if (message.isEmpty()) message = firstString(alarm, "alarmText");
  if (message.isEmpty() || message.length() > 480 || message.indexOf(static_cast<char>(0x1A)) >= 0) {
    ++skipped;
    return false;
  }

  if (!modem_.alarmDeliveryAvailable()) {
    rememberProcessed(fingerprint);
    ++sent;
    return true;
  }

  bool hasRecipient = false;
  for (const MobileSlot &slot : slots_) {
    if (slot.active && validPhoneNumber(slot.number) && slot.delivery != AlarmDelivery::none) {
      hasRecipient = true;
      break;
    }
  }
  if (!hasRecipient) {
    ++skipped;
    return false;
  }
  bool delivered = false;
  for (const MobileSlot &slot : slots_) {
    if (!slot.active || !validPhoneNumber(slot.number)) continue;
    if (slot.delivery == AlarmDelivery::sms || slot.delivery == AlarmDelivery::both) {
      if (progressCallback_) progressCallback_(id, message, "SMS", slot.number, AlarmProgress::starting);
      bool smsOk = modem_.sendSms(slot.number, message);
      if (progressCallback_) progressCallback_(id, message, "SMS", slot.number,
                                               smsOk ? AlarmProgress::succeeded : AlarmProgress::failed);
      if (smsOk) {
        delivered = true;
        ++sent;
      } else ++skipped;
      delay(300);
    }
    if (slot.delivery == AlarmDelivery::call || slot.delivery == AlarmDelivery::both) {
      if (progressCallback_) progressCallback_(id, message, "ANRUF", slot.number, AlarmProgress::starting);
      bool callOk = modem_.ring(slot.number, 30);
      if (progressCallback_) progressCallback_(id, message, "ANRUF", slot.number,
                                               callOk ? AlarmProgress::succeeded : AlarmProgress::failed);
      if (callOk) {
        delivered = true;
        ++sent;
      } else ++skipped;
      delay(300);
    }
  }
  // Mark the alarm only after every active slot has been attempted.
  rememberProcessed(fingerprint);
  return delivered;
}

bool AlarmRouter::technicalAllowed(uint32_t secondsOfDay) const {
  if (secondsOfDay > 86399) return false;
  if (technicalFrom_ == technicalUntil_) return true;
  if (technicalFrom_ < technicalUntil_) return secondsOfDay >= technicalFrom_ && secondsOfDay < technicalUntil_;
  return secondsOfDay >= technicalFrom_ || secondsOfDay < technicalUntil_;
}

bool AlarmRouter::alreadyProcessed(const String &fingerprint) const {
  for (uint8_t i = 0; i < seenCount_; ++i) if (seen_[i] == fingerprint) return true;
  return false;
}

void AlarmRouter::rememberProcessed(const String &fingerprint) {
  seen_[seenNext_] = fingerprint;
  seenNext_ = (seenNext_ + 1) % 12;
  if (seenCount_ < 12) ++seenCount_;
  saveSeen();
}

void AlarmRouter::loadSlots() {
  Preferences prefs;
  if (!prefs.begin(preferencesNamespace, true)) return;
  for (uint8_t i = 0; i < 5; ++i) {
    String json = prefs.getString(("slot" + String(i + 1)).c_str(), "");
    StaticJsonDocument<256> doc;
    if (!json.isEmpty() && !deserializeJson(doc, json)) {
      slots_[i].active = doc["active"] | false;
      slots_[i].number = doc["number"] | "";
      slots_[i].delivery = static_cast<AlarmDelivery>(doc["delivery"] | 0);
    }
  }
  prefs.end();
}

void AlarmRouter::saveSlot(uint8_t index) {
  StaticJsonDocument<256> doc;
  doc["active"] = slots_[index].active;
  doc["number"] = slots_[index].number;
  doc["delivery"] = static_cast<uint8_t>(slots_[index].delivery);
  String json;
  serializeJson(doc, json);
  Preferences prefs;
  if (prefs.begin(preferencesNamespace, false)) {
    prefs.putString(("slot" + String(index + 1)).c_str(), json);
    prefs.end();
  }
}

void AlarmRouter::saveTechnicalWindow() {
  Preferences prefs;
  if (prefs.begin(preferencesNamespace, false)) {
    prefs.putULong("techFrom", technicalFrom_);
    prefs.putULong("techUntil", technicalUntil_);
    prefs.end();
  }
}

void AlarmRouter::loadSeen() {
  Preferences prefs;
  if (!prefs.begin(preferencesNamespace, true)) return;
  String json = prefs.getString("seen", "");
  prefs.end();
  StaticJsonDocument<2048> doc;
  if (json.isEmpty() || deserializeJson(doc, json)) return;
  for (JsonVariantConst value : doc.as<JsonArrayConst>()) {
    if (seenCount_ >= 12) break;
    seen_[seenCount_++] = value.as<String>();
  }
  seenNext_ = seenCount_ % 12;
}

void AlarmRouter::saveSeen() {
  StaticJsonDocument<2048> doc;
  JsonArray values = doc.to<JsonArray>();
  for (uint8_t i = 0; i < seenCount_; ++i) values.add(seen_[i]);
  String json;
  serializeJson(doc, json);
  Preferences prefs;
  if (prefs.begin(preferencesNamespace, false)) {
    prefs.putString("seen", json);
    prefs.end();
  }
}

void AlarmRouter::toJson(JsonObject output) const {
  output["technicalFrom"] = technicalFrom_;
  output["technicalUntil"] = technicalUntil_;
  JsonArray slots = output.createNestedArray("mobileSlots");
  for (uint8_t i = 0; i < 5; ++i) {
    JsonObject slot = slots.createNestedObject();
    slot["slot"] = i + 1;
    slot["active"] = slots_[i].active;
    slot["number"] = slots_[i].number;
    slot["delivery"] = deliveryName(slots_[i].delivery);
  }
}
