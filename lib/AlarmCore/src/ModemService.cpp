#include "ModemService.h"

#include "BoardPins.h"
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
}  // namespace

void ModemService::begin(const DeviceConfig &config) {
  enabled_ = config.cellularEnabled;
  apn_ = config.apn;
  apnUser_ = config.apnUser;
  apnPassword_ = config.apnPassword;
  if (!enabled_) return;
  pinMode(BoardPins::modemReset, OUTPUT);
  digitalWrite(BoardPins::modemReset, HIGH);
  serial_.begin(BoardPins::modemBaud, SERIAL_8N1, BoardPins::modemRx, BoardPins::modemTx);
  delay(200);
  command("AT", "OK", 1000);
  command("ATE0", "OK", 1000);
  command("AT+CMGF=1", "OK", 1000);
  serial_.println("ATI");
  modemName_ = readUntil(1500);
  modemName_.replace("\r", " ");
  modemName_.replace("\n", " ");
  String detectedName = modemName_;
  detectedName.toUpperCase();
  if (detectedName.indexOf("EC25") >= 0) modemType_ = "EC25";
  else if (detectedName.indexOf("SIM7500") >= 0) modemType_ = "SIM7500";
  else modemType_ = "";
  serial_.println("AT+GSN");
  imei_ = extractImei(readUntil(1500));
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

bool ModemService::restoreFactoryDefaults(String &error) {
  if (!enabled_ || modemType_.isEmpty()) { error = "Mobilfunkmodem ist nicht bereit"; return false; }
  packetDataConnected_ = false;
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
  serial_.println("AT+GSN");
  imei_ = extractImei(readUntil(1500));
  pollStatus();
  return true;
}

void ModemService::loop() {
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
  delay((seconds > 60 ? 60 : seconds) * 1000UL);
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
    delay(2);
  }
  return false;
}

bool ModemService::readBinaryToFile(const char *path, size_t size, String &error) {
  SD.remove(path);
  File target = SD.open(path, FILE_WRITE);
  if (!target) { error = "Zieldatei konnte nicht erstellt werden"; return false; }
  uint8_t buffer[256];
  size_t written = 0;
  uint32_t lastByte = millis();
  while (written < size && millis() - lastByte < 30000) {
    size_t available = serial_.available();
    if (!available) {
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
    int requested = min(1024, total - offset);
    serial_.println("AT+HTTPREAD=" + String(offset) + "," + String(requested));
    if (!waitForLine("+HTTPREAD:", answer, 10000)) { error = "SIM7500 HTTPREAD ohne Antwort"; break; }
    int chunk = simReadSize(answer);
    if (chunk <= 0) { error = "SIM7500 HTTPREAD leer"; break; }
    size_t read = 0;
    uint32_t lastByte = millis();
    while (read < static_cast<size_t>(chunk) && millis() - lastByte < 30000) {
      size_t available = serial_.available();
      if (!available) { delay(2); continue; }
      size_t count = min<size_t>(min(sizeof(buffer), available), chunk - read);
      size_t received = serial_.readBytes(buffer, count);
      if (!received || target.write(buffer, received) != received) { error = "SIM7500 Daten konnten nicht gespeichert werden"; break; }
      read += received;
      lastByte = millis();
    }
    if (!error.isEmpty()) break;
    waitFor("OK", answer, 10000);
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
    delay(2);
  }
  return answer;
}

void ModemService::pollStatus() {
  lastPoll_ = millis();
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
