#include "ModemService.h"

#include "BoardPins.h"
#include "SystemRuntime.h"
#include <SD.h>

namespace {
String extractImei(const String &answer) {
  String digits;
  for (size_t i = 0; i < answer.length(); ++i) {
    char value = answer[i];
    if (value >= '0' && value <= '9') {
      digits += value;
      if (digits.length() == 15) return digits;
    } else {
      digits = "";
    }
  }
  return "";
}

int httpStatusFromUrc(const String &answer, const String &prefix) {
  int marker = answer.indexOf(prefix);
  if (marker < 0) return -1;
  int firstComma = answer.indexOf(',', marker);
  int secondComma = firstComma >= 0 ? answer.indexOf(',', firstComma + 1) : -1;
  if (firstComma < 0 || secondComma < 0) return -1;
  return answer.substring(firstComma + 1, secondComma).toInt();
}

int payloadSizeFromUrc(const String &answer, const String &prefix) {
  int marker = answer.indexOf(prefix);
  if (marker < 0) return -1;
  int lastComma = answer.indexOf(',', marker);
  while (lastComma >= 0) {
    int next = answer.indexOf(',', lastComma + 1);
    if (next < 0) break;
    lastComma = next;
  }
  if (lastComma < 0) return -1;
  return answer.substring(lastComma + 1).toInt();
}

int simReadSize(const String &answer) {
  int marker = answer.indexOf("+HTTPREAD:");
  if (marker < 0) return -1;
  int end = answer.indexOf('\n', marker);
  String value = end > marker ? answer.substring(marker + 10, end) : answer.substring(marker + 10);
  value.trim();
  return value.toInt();
}

String mqttSafe(String value) {
  value.replace("\\", "\\\\");
  value.replace("\"", "\\\"");
  value.replace("\r", " ");
  value.replace("\n", " ");
  return value;
}

String detectModemType(const String &answer) {
  String detected = answer;
  detected.toUpperCase();
  if (detected.indexOf("EC25") >= 0) return "EC25";
  if (detected.indexOf("SIM7500") >= 0) return "SIM7500";
  return "";
}
}  // namespace

void ModemService::begin(const DeviceConfig &config) {
  enabled_ = config.cellularEnabled;
  apn_ = config.apn;
  apnUser_ = config.apnUser;
  apnPassword_ = config.apnPassword;
  if (!enabled_) return;
  pinMode(BoardPins::modemReset, OUTPUT);
  digitalWrite(BoardPins::modemReset, HIGH);
  serial_.setRxBufferSize(16384);
  serial_.begin(BoardPins::modemBaud, SERIAL_8N1, BoardPins::modemRx, BoardPins::modemTx);
  serial_.setTimeout(3000);
  uint32_t startedAt = millis();
  while (modemType_.isEmpty() && millis() - startedAt < 20000) {
    if (!command("AT", "OK", 1000)) {
      delay(250);
      SystemRuntime::kickWatchdog();
      continue;
    }
    command("ATE0", "OK", 1000);
    command("AT+CMGF=1", "OK", 1000);
    serial_.println("ATI");
    modemName_ = readUntil(1500);
    modemName_.replace("\r", " ");
    modemName_.replace("\n", " ");
    modemType_ = detectModemType(modemName_);
    if (!modemType_.isEmpty()) break;
    delay(500);
    SystemRuntime::kickWatchdog();
  }
  refreshImei(2000);
  pollStatus();
}

void ModemService::maintainDataFallback(bool required) {
  if (!enabled_ || !registered_ || modemType_.isEmpty()) return;
  if (!required) return;
  if (packetDataConnected_ || millis() - lastDataAttempt_ < 60000) return;
  lastDataAttempt_ = millis();

  if (modemType_ == "EC25") {
    if (command("AT+QIACT?", "+QIACT: 1", 3000)) {
      packetDataConnected_ = true;
      return;
    }
    String context = "AT+QICSGP=1,1,\"" + apn_ + "\",\"" + apnUser_ + "\",\"" + apnPassword_ + "\",1";
    packetDataConnected_ = command(context, "OK", 5000) && command("AT+QIACT=1", "OK", 30000) &&
                           command("AT+QIACT?", "+QIACT: 1", 3000);
  } else {
    if (command("AT+NETOPEN?", "+NETOPEN: 1", 3000)) {
      packetDataConnected_ = true;
      return;
    }
    String context = "AT+CGDCONT=1,\"IP\",\"" + apn_ + "\"";
    packetDataConnected_ = command(context, "OK", 5000) && command("AT+NETOPEN", "OK", 30000) &&
                           command("AT+NETOPEN?", "+NETOPEN: 1", 3000);
  }
}

bool ModemService::getNetworkTime(String &response) {
  if (!enabled_ || !registered_ || modemType_.isEmpty()) return false;
  while (serial_.available()) serial_.read();
  serial_.println(modemType_ == "EC25" ? "AT+QLTS=2" : "AT+CCLK?");
  response = readUntil(3000);
  return response.indexOf(modemType_ == "EC25" ? "+QLTS:" : "+CCLK:") >= 0;
}

bool ModemService::downloadToFile(const String &url, const char *path, String &error) {
  if (!enabled_ || !registered_ || modemType_.isEmpty()) { error = "Mobilfunkmodem ist nicht bereit"; return false; }
  maintainDataFallback(true);
  if (!packetDataConnected_) { error = "Mobilfunk-Datenkontext ist nicht aktiv"; return false; }
  if (!url.startsWith("https://")) { error = "Mobilfunk-OTA unterstuetzt nur HTTPS"; return false; }
  if (modemType_ == "EC25") return downloadEc25(url, path, error);
  if (modemType_ == "SIM7500") return downloadSim7500(url, path, error);
  error = "Modemtyp fuer Mobilfunk-OTA nicht unterstuetzt";
  return false;
}

bool ModemService::mqttConnect(const String &host, uint16_t port, const String &clientId,
                               const String &user, const String &password, String &error) {
  if (!enabled_ || !registered_ || modemType_.isEmpty()) { error = "Mobilfunkmodem ist nicht bereit"; return false; }
  if (host.isEmpty() || port == 0 || clientId.isEmpty()) { error = "MQTT-Zugangsdaten fuer Mobilfunk sind unvollstaendig"; return false; }
  maintainDataFallback(true);
  if (!packetDataConnected_) { error = "Mobilfunk-Datenkontext ist nicht aktiv"; return false; }
  if (mqttConnected_) return true;
  if (modemType_ == "EC25") return mqttConnectEc25(host, port, clientId, user, password, error);
  if (modemType_ == "SIM7500") return mqttConnectSim7500(host, port, clientId, user, password, error);
  error = "Modemtyp fuer Mobilfunk-MQTT nicht unterstuetzt";
  return false;
}

bool ModemService::mqttPublish(const String &topic, const String &payload, bool retain, String &error) {
  if (!mqttConnected_) { error = "Mobilfunk-MQTT ist nicht verbunden"; return false; }
  if (topic.isEmpty() || payload.length() > 4096) { error = "MQTT-Nachricht ist ungueltig oder zu gross"; return false; }
  if (modemType_ == "EC25") return mqttPublishEc25(topic, payload, retain, error);
  if (modemType_ == "SIM7500") return mqttPublishSim7500(topic, payload, retain, error);
  error = "Modemtyp fuer Mobilfunk-MQTT nicht unterstuetzt";
  mqttConnected_ = false;
  return false;
}

void ModemService::mqttDisconnect() {
  if (!enabled_ || modemType_.isEmpty()) {
    mqttConnected_ = false;
    return;
  }
  if (modemType_ == "EC25") {
    command("AT+QMTDISC=0", "OK", 5000);
    command("AT+QMTCLOSE=0", "OK", 5000);
  } else if (modemType_ == "SIM7500") {
    command("AT+CMQTTDISC=0,60", "OK", 10000);
    command("AT+CMQTTREL=0", "OK", 5000);
    command("AT+CMQTTSTOP", "OK", 5000);
    simMqttStarted_ = false;
  }
  mqttConnected_ = false;
}

bool ModemService::restoreFactoryDefaults(String &error) {
  if (!enabled_ || modemType_.isEmpty()) { error = "Mobilfunkmodem ist nicht bereit"; return false; }
  packetDataConnected_ = false;
  mqttConnected_ = false;
  simMqttStarted_ = false;
  while (serial_.available()) serial_.read();
  if (!command("AT", "OK", 3000)) { error = "Modem antwortet nicht"; return false; }
  if (!command("AT&F", "OK", 10000)) { error = "Factory-Defaults wurden vom Modem nicht angenommen"; return false; }
  if (!command("AT&W", "OK", 10000)) { error = "Factory-Defaults konnten nicht gespeichert werden"; return false; }
  if (modemType_ == "EC25") command("AT+CFUN=1,1", "OK", 5000);
  else command("AT+CRESET", "OK", 5000);
  delay(4000);
  command("AT", "OK", 3000);
  command("ATE0", "OK", 1000);
  command("AT+CMGF=1", "OK", 1000);
  refreshImei(2000);
  pollStatus();
  return true;
}

void ModemService::loop() {
  SystemRuntime::kickWatchdog();
  if (enabled_ && imei_.isEmpty() && millis() - lastImeiAttempt_ >= 5000) refreshImei();
  if (enabled_ && millis() - lastPoll_ >= 15000) pollStatus();
}

bool ModemService::sendSms(const String &number, const String &message) {
  if (!enabled_ || number.isEmpty() || message.isEmpty()) return false;
  if (!command("AT+CMGF=1", "OK")) return false;
  serial_.print("AT+CMGS=\"");
  serial_.print(number);
  serial_.println("\"");
  if (readUntil(3000).indexOf('>') < 0) return false;
  serial_.print(message);
  serial_.write(0x1A);
  return readUntil(30000).indexOf("OK") >= 0;
}

bool ModemService::ring(const String &number, uint16_t seconds) {
  if (!enabled_ || number.isEmpty()) return false;
  serial_.print("ATD");
  serial_.print(number);
  serial_.println(';');
  String answer = readUntil(5000);
  if (answer.indexOf("OK") < 0 && answer.indexOf("CONNECT") < 0) return false;
  uint32_t remaining = (seconds > 60 ? 60 : seconds) * 1000UL;
  while (remaining > 0) {
    uint32_t chunk = min<uint32_t>(remaining, 250);
    delay(chunk);
    remaining -= chunk;
    SystemRuntime::kickWatchdog();
  }
  return command("ATH", "OK", 5000);
}

bool ModemService::command(const String &value, const char *expected, uint32_t timeout) {
  while (serial_.available()) serial_.read();
  serial_.println(value);
  return readUntil(timeout).indexOf(expected) >= 0;
}

bool ModemService::waitFor(const String &token, String &answer, uint32_t timeout) {
  answer = "";
  uint32_t start = millis();
  while (millis() - start < timeout) {
    while (serial_.available()) {
      answer += static_cast<char>(serial_.read());
      if (answer.indexOf(token) >= 0) return true;
      if (answer.length() > 512) answer.remove(0, answer.length() - 512);
    }
    SystemRuntime::kickWatchdog();
    delay(2);
  }
  return false;
}

bool ModemService::waitForLine(const String &token, String &answer, uint32_t timeout) {
  answer = "";
  String line;
  uint32_t start = millis();
  while (millis() - start < timeout) {
    while (serial_.available()) {
      char value = static_cast<char>(serial_.read());
      answer += value;
      line += value;
      if (value == '\n') {
        if (line.indexOf(token) >= 0) return true;
        line = "";
      }
      if (answer.length() > 512) answer.remove(0, answer.length() - 512);
      if (line.length() > 160) line.remove(0, line.length() - 160);
    }
    SystemRuntime::kickWatchdog();
    delay(2);
  }
  return false;
}

bool ModemService::readBinaryToFile(const char *path, size_t size, String &error) {
  SD.remove(path);
  File target = SD.open(path, FILE_WRITE);
  if (!target) { error = "Zieldatei konnte nicht erstellt werden"; return false; }
  uint8_t buffer[1024];
  size_t written = 0;
  uint32_t lastByte = millis();
  while (written < size && millis() - lastByte < 30000) {
    size_t available = serial_.available();
    if (!available) {
      SystemRuntime::kickWatchdog();
      delay(2);
      continue;
    }
    size_t count = min<size_t>(min(sizeof(buffer), available), size - written);
    size_t received = serial_.readBytes(buffer, count);
    if (!received || target.write(buffer, received) != received) {
      target.close();
      SD.remove(path);
      error = "Mobilfunkdaten konnten nicht gespeichert werden";
      return false;
    }
    written += received;
    lastByte = millis();
    SystemRuntime::kickWatchdog();
  }
  target.close();
  if (written != size) {
    SD.remove(path);
    error = "Mobilfunkdownload ist unvollstaendig";
    return false;
  }
  String trailer;
  waitFor("OK", trailer, 10000);
  return true;
}

bool ModemService::refreshImei(uint32_t timeout) {
  if (!enabled_) return false;
  if (!imei_.isEmpty()) return true;
  if (lastImeiAttempt_ != 0 && millis() - lastImeiAttempt_ < 5000) return false;
  lastImeiAttempt_ = millis();
  if (modemType_.isEmpty()) {
    if (!command("AT", "OK", timeout)) return false;
    serial_.println("ATI");
    modemName_ = readUntil(timeout);
    modemName_.replace("\r", " ");
    modemName_.replace("\n", " ");
    modemType_ = detectModemType(modemName_);
    if (modemType_.isEmpty()) return false;
    command("ATE0", "OK", 1000);
    command("AT+CMGF=1", "OK", 1000);
  }
  for (uint8_t attempt = 0; attempt < 3 && imei_.isEmpty(); ++attempt) {
    while (serial_.available()) serial_.read();
    serial_.println("AT+GSN");
    String response = readUntil(timeout);
    String candidate = extractImei(response);
    if (!candidate.isEmpty()) {
      imei_ = candidate;
      return true;
    }
    SystemRuntime::kickWatchdog();
    delay(500);
  }
  return false;
}

bool ModemService::writePromptPayload(const String &commandText, const String &payload, String &answer,
                                      uint32_t promptTimeout, uint32_t finalTimeout) {
  while (serial_.available()) serial_.read();
  serial_.println(commandText);
  answer = readUntil(promptTimeout);
  if (answer.indexOf('>') < 0) return false;
  serial_.print(payload);
  answer = readUntil(finalTimeout);
  return answer.indexOf("OK") >= 0;
}

bool ModemService::mqttConnectEc25(const String &host, uint16_t port, const String &clientId,
                                   const String &user, const String &password, String &error) {
  mqttDisconnect();
  command("AT+QMTCFG=\"recv/mode\",0,0,1", "OK", 3000);
  String answer;
  serial_.println("AT+QMTOPEN=0,\"" + mqttSafe(host) + "\"," + String(port));
  if (!waitForLine("+QMTOPEN:", answer, 65000) || answer.indexOf("+QMTOPEN: 0,0") < 0) {
    error = "EC25 MQTT OPEN fehlgeschlagen";
    mqttConnected_ = false;
    return false;
  }
  String connect = "AT+QMTCONN=0,\"" + mqttSafe(clientId) + "\"";
  if (!user.isEmpty()) connect += ",\"" + mqttSafe(user) + "\",\"" + mqttSafe(password) + "\"";
  serial_.println(connect);
  if (!waitForLine("+QMTCONN:", answer, 30000) ||
      (answer.indexOf("+QMTCONN: 0,0,0") < 0 && answer.indexOf("+QMTCONN: 0,0") < 0)) {
    error = "EC25 MQTT CONN fehlgeschlagen";
    mqttDisconnect();
    return false;
  }
  mqttConnected_ = true;
  return true;
}

bool ModemService::mqttPublishEc25(const String &topic, const String &payload, bool retain, String &error) {
  String answer;
  String commandText = "AT+QMTPUBEX=0,0,0," + String(retain ? 1 : 0) + ",\"" + mqttSafe(topic) + "\"," + String(payload.length());
  if (!writePromptPayload(commandText, payload, answer, 10000, 20000) ||
      (answer.indexOf("+QMTPUBEX: 0,0,0") < 0 && answer.indexOf("OK") < 0)) {
    error = "EC25 MQTT Publish fehlgeschlagen";
    mqttConnected_ = false;
    return false;
  }
  return true;
}

bool ModemService::mqttConnectSim7500(const String &host, uint16_t port, const String &clientId,
                                      const String &user, const String &password, String &error) {
  mqttDisconnect();
  String answer;
  if (!simMqttStarted_) {
    serial_.println("AT+CMQTTSTART");
    if (!waitForLine("+CMQTTSTART:", answer, 30000) || answer.indexOf("+CMQTTSTART: 0") < 0) {
      error = "SIM7500 MQTT START fehlgeschlagen";
      simMqttStarted_ = false;
      return false;
    }
    simMqttStarted_ = true;
  }
  if (!command("AT+CMQTTACCQ=0,\"" + mqttSafe(clientId) + "\",0", "OK", 10000)) {
    error = "SIM7500 MQTT Client konnte nicht angelegt werden";
    mqttDisconnect();
    return false;
  }
  String connect = "AT+CMQTTCONNECT=0,\"tcp://" + mqttSafe(host) + ":" + String(port) + "\",60,1";
  if (!user.isEmpty()) connect += ",\"" + mqttSafe(user) + "\",\"" + mqttSafe(password) + "\"";
  serial_.println(connect);
  if (!waitForLine("+CMQTTCONNECT:", answer, 65000) || answer.indexOf("+CMQTTCONNECT: 0,0") < 0) {
    error = "SIM7500 MQTT CONNECT fehlgeschlagen";
    mqttDisconnect();
    return false;
  }
  mqttConnected_ = true;
  return true;
}

bool ModemService::mqttPublishSim7500(const String &topic, const String &payload, bool retain, String &error) {
  String answer;
  if (!writePromptPayload("AT+CMQTTTOPIC=0," + String(topic.length()), topic, answer, 10000, 10000)) {
    error = "SIM7500 MQTT Topic wurde nicht angenommen";
    mqttConnected_ = false;
    return false;
  }
  if (!writePromptPayload("AT+CMQTTPAYLOAD=0," + String(payload.length()), payload, answer, 10000, 10000)) {
    error = "SIM7500 MQTT Payload wurde nicht angenommen";
    mqttConnected_ = false;
    return false;
  }
  serial_.println("AT+CMQTTPUB=0,0," + String(retain ? 1 : 0) + ",60");
  if (!waitForLine("+CMQTTPUB:", answer, 30000) || answer.indexOf("+CMQTTPUB: 0,0") < 0) {
    error = "SIM7500 MQTT Publish fehlgeschlagen";
    mqttConnected_ = false;
    return false;
  }
  return true;
}

bool ModemService::downloadEc25(const String &url, const char *path, String &error) {
  command("AT+QHTTPSTOP", "OK", 5000);
  command("AT+QHTTPCFG=\"contextid\",1", "OK", 3000);
  command("AT+QHTTPCFG=\"requestheader\",0", "OK", 3000);
  command("AT+QHTTPCFG=\"responseheader\",0", "OK", 3000);
  command("AT+QHTTPCFG=\"redirect\",1", "OK", 3000);
  command("AT+QHTTPCFG=\"sslctxid\",1", "OK", 3000);
  command("AT+QSSLCFG=\"sslversion\",1,4", "OK", 3000);
  command("AT+QSSLCFG=\"seclevel\",1,0", "OK", 3000);
  while (serial_.available()) serial_.read();
  serial_.println("AT+QHTTPURL=" + String(url.length()) + ",80");
  String answer;
  if (!waitFor("CONNECT", answer, 10000)) { error = "EC25 URL-Eingabe nicht moeglich"; return false; }
  serial_.print(url);
  if (!waitFor("OK", answer, 10000)) { error = "EC25 URL wurde nicht angenommen"; return false; }
  serial_.println("AT+QHTTPGET=120");
  if (!waitForLine("+QHTTPGET:", answer, 130000)) { error = "EC25 HTTP GET ohne Antwort"; return false; }
  int status = httpStatusFromUrc(answer, "+QHTTPGET:");
  int size = payloadSizeFromUrc(answer, "+QHTTPGET:");
  if (status != 200 || size <= 0) { error = "EC25 HTTP Status " + String(status); return false; }
  serial_.println("AT+QHTTPREAD=120");
  if (!waitForLine("CONNECT", answer, 10000)) { error = "EC25 HTTPREAD nicht gestartet"; return false; }
  return readBinaryToFile(path, static_cast<size_t>(size), error);
}

bool ModemService::downloadSim7500(const String &url, const char *path, String &error) {
  command("AT+HTTPTERM", "OK", 3000);
  if (!command("AT+HTTPINIT", "OK", 5000)) { error = "SIM7500 HTTPINIT fehlgeschlagen"; return false; }
  command("AT+HTTPPARA=\"CID\",1", "OK", 3000);
  command("AT+HTTPSSL=1", "OK", 3000);
  command("AT+HTTPPARA=\"REDIR\",1", "OK", 3000);
  if (!command("AT+HTTPPARA=\"URL\",\"" + url + "\"", "OK", 10000)) { error = "SIM7500 URL wurde nicht angenommen"; command("AT+HTTPTERM", "OK", 3000); return false; }
  serial_.println("AT+HTTPACTION=0");
  String answer;
  if (!waitForLine("+HTTPACTION:", answer, 130000)) { error = "SIM7500 HTTPACTION ohne Antwort"; command("AT+HTTPTERM", "OK", 3000); return false; }
  int status = httpStatusFromUrc(answer, "+HTTPACTION:");
  int total = payloadSizeFromUrc(answer, "+HTTPACTION:");
  if (status != 200 || total <= 0) { error = "SIM7500 HTTP Status " + String(status); command("AT+HTTPTERM", "OK", 3000); return false; }
  SD.remove(path);
  File target = SD.open(path, FILE_WRITE);
  if (!target) { error = "Zieldatei konnte nicht erstellt werden"; command("AT+HTTPTERM", "OK", 3000); return false; }
  uint8_t buffer[256];
  int offset = 0;
  while (offset < total) {
    int requested = min(512, total - offset);
    serial_.println("AT+HTTPREAD=" + String(offset) + "," + String(requested));
    if (!waitForLine("+HTTPREAD:", answer, 10000)) { error = "SIM7500 HTTPREAD ohne Antwort"; break; }
    int chunk = simReadSize(answer);
    if (chunk <= 0) { error = "SIM7500 HTTPREAD leer"; break; }
    size_t read = 0;
    uint32_t lastByte = millis();
    while (read < static_cast<size_t>(chunk) && millis() - lastByte < 30000) {
      size_t available = serial_.available();
      if (!available) { SystemRuntime::kickWatchdog(); delay(2); continue; }
      size_t count = min<size_t>(min(sizeof(buffer), available), chunk - read);
      size_t received = serial_.readBytes(buffer, count);
      if (!received || target.write(buffer, received) != received) { error = "SIM7500 Daten konnten nicht gespeichert werden"; break; }
      read += received;
      lastByte = millis();
      SystemRuntime::kickWatchdog();
    }
    if (!error.isEmpty()) break;
    if (!waitForLine("OK", answer, 10000)) { error = "SIM7500 HTTPREAD nicht abgeschlossen"; break; }
    offset += chunk;
  }
  target.close();
  command("AT+HTTPTERM", "OK", 3000);
  if (!error.isEmpty() || offset != total) {
    SD.remove(path);
    if (error.isEmpty()) error = "SIM7500 Download ist unvollstaendig";
    return false;
  }
  return true;
}

String ModemService::readUntil(uint32_t timeout) {
  String answer;
  uint32_t lastByte = millis();
  while (millis() - lastByte < timeout) {
    while (serial_.available()) {
      answer += static_cast<char>(serial_.read());
      lastByte = millis();
    }
    if (answer.indexOf("\r\nOK\r\n") >= 0 || answer.indexOf("\r\nERROR\r\n") >= 0 || answer.indexOf('>') >= 0) break;
    SystemRuntime::kickWatchdog();
    delay(2);
  }
  return answer;
}

void ModemService::pollStatus() {
  lastPoll_ = millis();
  if (modemType_.isEmpty()) {
    refreshImei();
    if (modemType_.isEmpty()) return;
  }
  if (imei_.isEmpty()) refreshImei();
  serial_.println("AT+CSQ");
  String csq = readUntil(1500);
  int marker = csq.indexOf("+CSQ:");
  if (marker >= 0) signalQuality_ = csq.substring(marker + 5).toInt();
  serial_.println("AT+CEREG?");
  String registration = readUntil(1500);
  registered_ = registration.indexOf(",1") >= 0 || registration.indexOf(",5") >= 0;
  serial_.println("AT+COPS?");
  String provider = readUntil(2000);
  int firstQuote = provider.indexOf('"');
  int secondQuote = firstQuote >= 0 ? provider.indexOf('"', firstQuote + 1) : -1;
  if (firstQuote >= 0 && secondQuote > firstQuote + 1) {
    networkOperator_ = provider.substring(firstQuote + 1, secondQuote);
  }
}
