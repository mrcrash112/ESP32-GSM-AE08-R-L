#pragma once

#include <Arduino.h>

namespace BoardPins {
constexpr uint8_t spiSck = 18;
constexpr uint8_t spiMiso = 19;
constexpr uint8_t spiMosi = 23;
constexpr uint8_t sdCs = 15;
constexpr uint8_t ethernetCs = 5;

constexpr uint8_t i2cSda = 16;
constexpr uint8_t i2cScl = 17;
constexpr uint8_t oledAddress = 0x3C;
constexpr uint8_t rtcAddress = 0x68;

constexpr uint8_t modemTx = 32;
constexpr uint8_t modemRx = 33;
constexpr uint8_t modemReset = 21;
constexpr uint32_t modemBaud = 115200;

constexpr uint8_t buttonsAdc = 36;
constexpr uint8_t inputs[] = {34, 35, 14, 13};
constexpr uint8_t relays[] = {12, 2, 27, 4};
}  // namespace BoardPins
