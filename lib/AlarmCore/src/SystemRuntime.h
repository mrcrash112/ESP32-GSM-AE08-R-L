#pragma once

#include <Arduino.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

namespace SystemRuntime {
constexpr uint32_t kWatchdogTimeoutSeconds = 30;

void initWatchdog(uint32_t timeoutSeconds = kWatchdogTimeoutSeconds);
void kickWatchdog();

void requestReboot(const String &reason, const String &detail = "");
bool consumeRebootRequest(String &reason, String &detail);

const char *resetReasonName(esp_reset_reason_t reason);
}  // namespace SystemRuntime
