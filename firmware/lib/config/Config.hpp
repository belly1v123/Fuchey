#pragma once
// ============================================================
// Fuchey — Config.hpp
// Central configuration: pin assignments, task parameters,
// timing constants. No magic numbers anywhere else.
// ============================================================

#include <cstdint>
#include <driver/i2c.h>

namespace Fuchey {

// ─── Firmware Version ─────────────────────────────────────
inline constexpr const char* FW_VERSION = "1.0.0-dev";

// ─── OLED Display (SSD1306 I2C) ───────────────────────────
namespace DisplayConfig {
    inline constexpr uint8_t    I2C_ADDRESS  = 0x3C;
    inline constexpr i2c_port_t I2C_PORT     = I2C_NUM_0;
    inline constexpr int        PIN_SDA      = 8;
    inline constexpr int        PIN_SCL      = 9;
    inline constexpr uint32_t   I2C_FREQ_HZ  = 400000;  // 400 kHz Fast Mode
    inline constexpr int        WIDTH        = 128;
    inline constexpr int        HEIGHT       = 64;
}

// ─── Buttons ──────────────────────────────────────────────
namespace Buttons {
    inline constexpr int     PIN_CONFIRM   = 20;  // GPIO20 (Physical confirm button)
    inline constexpr int     PIN_BACK      = -1;  // -1 = unused (single-button mode; long-press acts as BACK)
    inline constexpr uint32_t DEBOUNCE_MS  = 50;
    inline constexpr uint32_t LONG_PRESS_MS = 1000;
}

// ─── FreeRTOS Task Configuration ─────────────────────────
namespace Tasks {
    // Stack sizes in bytes (ESP-IDF xTaskCreate takes bytes)
    inline constexpr uint32_t DISPLAY_STACK     = 4096;
    inline constexpr uint32_t BUTTON_STACK      = 2048;
    inline constexpr uint32_t WIFI_STACK        = 8192;
    inline constexpr uint32_t WALLET_STACK      = 8192;
    inline constexpr uint32_t AI_STACK          = 8192;
    inline constexpr uint32_t WEATHER_STACK     = 8192;
    inline constexpr uint32_t PRICE_STACK       = 8192;
    inline constexpr uint32_t UI_STACK          = 8192;

    // Priorities (higher number = higher priority)
    inline constexpr int WALLET_PRIORITY        = 10;  // Highest — security critical
    inline constexpr int UI_PRIORITY            = 7;
    inline constexpr int WIFI_PRIORITY          = 6;
    inline constexpr int AI_PRIORITY            = 5;   // Intentionally lower than wallet
    inline constexpr int WEATHER_PRIORITY       = 3;
    inline constexpr int PRICE_PRIORITY         = 3;
    inline constexpr int BUTTON_PRIORITY        = 8;

    // CPU core assignment
    inline constexpr int WALLET_CORE            = 1;   // Core 1: security-critical only
    inline constexpr int UI_CORE                = 0;   // Core 0: UI, networking, services
    inline constexpr int WIFI_CORE              = 0;
    inline constexpr int AI_CORE               = 0;
    inline constexpr int BUTTON_CORE            = 1;   // Core 1: responsive input
}

// ─── Queue Depths ─────────────────────────────────────────
namespace Queues {
    inline constexpr int BUTTON_EVENTS          = 8;
    inline constexpr int WALLET_REQUESTS        = 4;
    inline constexpr int UI_COMMANDS            = 16;
    inline constexpr int AI_MESSAGES            = 8;
}

// ─── Timing ───────────────────────────────────────────────
namespace Timing {
    inline constexpr uint32_t IDLE_SCREEN_CYCLE_MS   = 10000;  // 10s per idle screen
    inline constexpr uint32_t WEATHER_UPDATE_MS       = 600000; // 10 minutes
    inline constexpr uint32_t PRICE_UPDATE_MS         = 300000; // 5 minutes
    inline constexpr uint32_t WIFI_RECONNECT_DELAY_MS = 5000;
    inline constexpr uint32_t SESSION_TIMEOUT_MS      = 300000; // 5 min auto-lock
    inline constexpr uint32_t DISPLAY_UPDATE_MS       = 100;    // 10 FPS UI refresh
}

// ─── NVS Namespaces ───────────────────────────────────────
namespace NVS {
    inline constexpr const char* WALLET_NS     = "fuchey_wallet";
    inline constexpr const char* CONFIG_NS     = "fuchey_cfg";
    inline constexpr const char* POLICY_NS     = "fuchey_policy";
    inline constexpr const char* WIFI_NS       = "fuchey_wifi";
    inline constexpr const char* AI_NS         = "fuchey_ai";

    // Keys
    inline constexpr const char* KEY_MNEMONIC_ENC  = "mnemonic_enc";
    inline constexpr const char* KEY_WALLET_CREATED = "wallet_ok";
    inline constexpr const char* KEY_SPEND_LIMIT   = "spend_limit";
    inline constexpr const char* KEY_WIFI_SSID     = "ssid";
    inline constexpr const char* KEY_WIFI_PASS     = "password";
    inline constexpr const char* KEY_LLM_API_KEY   = "llm_api_key";
    inline constexpr const char* KEY_LLM_ENDPOINT  = "llm_endpoint";
    inline constexpr const char* KEY_WEATHER_CITY  = "weather_city";
    inline constexpr const char* KEY_WEATHER_LAT   = "weather_lat";
    inline constexpr const char* KEY_WEATHER_LON   = "weather_lon";
    inline constexpr const char* KEY_NETWORK       = "network";
}

// ─── API Endpoints ────────────────────────────────────────
namespace API {
    // CoinGecko (no API key needed for basic)
    inline constexpr const char* SOL_PRICE_URL =
        "https://api.coingecko.com/api/v3/simple/price?ids=solana&vs_currencies=usd";

    // Open-Meteo (no API key needed)
    inline constexpr const char* WEATHER_URL_FMT =
        "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
        "&current_weather=true&temperature_unit=celsius";

    // Default LLM — configurable via NVS
    inline constexpr const char* LLM_DEFAULT_ENDPOINT =
        "https://api.openai.com/v1/chat/completions";
    inline constexpr const char* LLM_DEFAULT_MODEL = "gpt-4o-mini";

    // Solana Network Endpoints & Token Mints
    inline constexpr const char* SOLANA_MAINNET_RPC =
        "https://api.mainnet-beta.solana.com";
    inline constexpr const char* SOLANA_DEVNET_RPC =
        "https://api.devnet.solana.com";

    inline constexpr const char* USDC_MAINNET_MINT =
        "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v";
    inline constexpr const char* USDC_DEVNET_MINT =
        "4zMMC9srt5Ri5X14GAgXhaHii3GnPAEERYPJgZJDncDU"; // Devnet USDC SPL mint
}

// ─── Crypto Constants ─────────────────────────────────────
namespace Crypto {
    inline constexpr int  MNEMONIC_WORDS_12    = 12;
    inline constexpr int  MNEMONIC_WORDS_24    = 24;
    inline constexpr int  SEED_BYTES           = 64;
    inline constexpr int  PRIVKEY_BYTES        = 32;
    inline constexpr int  PUBKEY_BYTES         = 32;
    inline constexpr int  SIGNATURE_BYTES      = 64;
    inline constexpr int  ENTROPY_128_BITS     = 16; // 12-word mnemonic
    inline constexpr int  ENTROPY_256_BITS     = 32; // 24-word mnemonic

    // Solana HD derivation path: m/44'/501'/0'/0'
    inline constexpr const char* SOLANA_DERIVATION_PATH = "m/44'/501'/0'/0'";
}

// ─── Security ─────────────────────────────────────────────
namespace Security {
    // Auto-sign threshold sentinel for "OFF" state
    inline constexpr uint32_t SPEND_LIMIT_OFF  = 0;
    // Amounts stored as micro-USD (uint32_t cents * 100)
    // e.g. $0.50 = 50, $1.00 = 100, $5.00 = 500 (in cents)
    inline constexpr uint32_t SPEND_LIMIT_50c  = 50;
    inline constexpr uint32_t SPEND_LIMIT_1USD  = 100;
    inline constexpr uint32_t SPEND_LIMIT_5USD  = 500;
}

} // namespace Fuchey
