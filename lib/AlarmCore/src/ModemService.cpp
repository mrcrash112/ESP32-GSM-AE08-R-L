#include "ModemService.h"

#include "BoardPins.h"

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
