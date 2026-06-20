#pragma once

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "0.1.14"
#endif

#ifndef RECOVERY_VERSION
#define RECOVERY_VERSION "0.1.4"
#endif

namespace BuildInfo {
constexpr const char *version = FIRMWARE_VERSION;
constexpr const char *recoveryVersion = RECOVERY_VERSION;
constexpr const char *product = "NORVI-GSM-AE08-R-L";
constexpr const char *firmwarePath = "/firmware/update.bin";
constexpr const char *recoveryPath = "/firmware/recovery.bin";
constexpr const char *webPackagePath = "/firmware/www-update.tar";
constexpr const char *manifestPath = "/firmware/update.json";
constexpr const char *webVersionPath = "/www/version.json";
}  // namespace BuildInfo
