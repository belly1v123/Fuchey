#pragma once
// ============================================================
// Fuchey — DeviceSettings.hpp
// What the companion app may read and change about the device
// itself (not the wallet). Implemented in src/main.cpp on top of
// the same services the serial console uses.
//
// Security tier: "device settings". These never touch keys, never
// sign, never switch devnet/mainnet. Secrets are write-only (the
// WiFi password is never returned or logged).
// ============================================================

#include <cstdint>
#include <string>
#include <vector>

namespace Fuchey {

struct DeviceStatus {
    bool        wifi_configured{false};
    bool        wifi_connected{false};
    bool        online{false};          // has an IP address
    std::string wifi_ssid;              // saved SSID (never the password)
    bool        location_configured{false};
    std::string city;
    float       lat{0.0f};
    float       lon{0.0f};
    bool        setup_done{false};
};

struct WifiNetwork {
    std::string ssid;
    int8_t      rssi{0};
    bool        secure{true};
};

class DeviceSettings {
public:
    virtual ~DeviceSettings() = default;

    virtual DeviceStatus status() = 0;
    // Blocking scan (~2–3 s). False if the radio is busy (e.g. connecting).
    virtual bool scan_wifi(std::vector<WifiNetwork>& out) = 0;
    // Saves + connects; returns once the attempt has started.
    virtual bool set_wifi(const char* ssid, const char* password) = 0;
    // Saves the manual weather location and refreshes the weather.
    virtual bool set_location(const char* city, float lat, float lon) = 0;
};

} // namespace Fuchey
