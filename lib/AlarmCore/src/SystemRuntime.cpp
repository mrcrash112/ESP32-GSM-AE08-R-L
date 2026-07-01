#include "SystemRuntime.h"

#include <cstring>

namespace {
RTC_DATA_ATTR uint32_t rebootRequestMagic = 0;
RTC_DATA_ATTR char rebootRequestReason[32] = {};
RTC_DATA_ATTR char rebootRequestDetail[128] = {};
constexpr uint32_t kRebootRequestMagic = 0x52454254;
}  // namespace

namespace SystemRuntime {
void initWatchdog(uint32_t timeoutSeconds) {
  if (timeoutSeconds == 0) timeoutSeconds = kWatchdogTimeoutSeconds;
  esp_err_t result = esp_task_wdt_init(timeoutSeconds, true);
  if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) return;
  esp_task_wdt_add(nullptr);
}

void kickWatchdog() {
  esp_task_wdt_reset();
}

void requestReboot(const String &reason, const String &detail) {
  String safeReason = reason.isEmpty() ? "manual" : reason;
  String safeDetail = detail;
  strlcpy(rebootRequestReason, safeReason.c_str(), sizeof(rebootRequestReason));
  strlcpy(rebootRequestDetail, safeDetail.c_str(), sizeof(rebootRequestDetail));
  rebootRequestMagic = kRebootRequestMagic;
}

bool consumeRebootRequest(String &reason, String &detail) {
  if (rebootRequestMagic != kRebootRequestMagic) {
    reason = "";
    detail = "";
    return false;
  }
  reason = rebootRequestReason;
  detail = rebootRequestDetail;
  rebootRequestMagic = 0;
  rebootRequestReason[0] = '\0';
  rebootRequestDetail[0] = '\0';
  return true;
}

const char *resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt-watchdog";
    case ESP_RST_TASK_WDT: return "task-watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep-sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    default: return "unknown";
  }
}
}  // namespace SystemRuntime
