#pragma once
// ============================================================
// Fuchey — Config.hpp
// Central configuration: pin assignments, task parameters,
// timing constants. No magic numbers anywhere else.
// ============================================================

#include <cstdint>
#include <driver/spi_common.h>

namespace Fuchey {

// ─── Firmware Version ─────────────────────────────────────
inline constexpr const char* FW_VERSION = "1.0.0-dev";

// ─── TFT Display (ST7789 240x240 SPI) ─────────────────────
namespace DisplayConfig {
    // 1.54" 240x240 ST7789V2 IPS over 4-wire SPI. BL tied to 3V3 on the
    // module (PIN_BL < 0 means firmware never drives the backlight).
    inline constexpr int  PIN_SCLK     = 9;
    inline constexpr int  PIN_MOSI     = 8;
    inline constexpr int  PIN_CS       = 5;                // GPIO5 (J1-5) — plain I/O
    inline constexpr int  PIN_DC       = 16;               // GPIO16 (J1-9) — plain I/O, no conflict
    inline constexpr int  PIN_RST      = 6;                // GPIO6 (J1-6) — plain I/O
    inline constexpr int  PIN_BL       = -1;               // no backlight control
    inline constexpr int  SPI_FREQ_HZ  = 8000000;          // 8 MHz — matches working Adafruit test; >40MHz can black-screen on dupont wiring
    inline constexpr spi_host_device_t SPI_HOST = SPI2_HOST;
    inline constexpr int  WIDTH        = 240;
    inline constexpr int  HEIGHT       = 240;
}

// ─── Buttons ──────────────────────────────────────────────
namespace Buttons {
    // B1 (GPIO4): TX confirm only during TX_CONFIRM (single=accept,
    // double/long=reject); elsewhere acts as hierarchical Back.
    // B2 (GPIO10): open menu / select highlighted item.
    // B3 (GPIO17): previous item in menu / sub-screen carousel.
    // B4 (GPIO13): next item in menu / sub-screen carousel.
    inline constexpr int     PIN_B1_TX_BACK    = 4;   // GPIO4 (TX confirm / Back)
    inline constexpr int     PIN_B2_MENU_SELECT = 10; // GPIO10 (Menu open / select)
    inline constexpr int     PIN_B3_PREV       = 17;  // GPIO17 (Prev)
    inline constexpr int     PIN_B4_NEXT       = 13;  // GPIO13 (Next)
    // Back-compat aliases (deprecated, prefer PIN_Bx_* above)
    inline constexpr int     PIN_CONFIRM = PIN_B1_TX_BACK;
    inline constexpr int     PIN_MENU    = PIN_B2_MENU_SELECT;
    inline constexpr int     PIN_SELECT  = PIN_B3_PREV;
    inline constexpr int     PIN_BACK    = PIN_B4_NEXT;
    inline constexpr uint32_t DEBOUNCE_MS  = 50;
    inline constexpr uint32_t LONG_PRESS_MS = 1000;  // hold = decline transaction
}

// ─── Buzzer (active 5 V electromagnetic buzzer, e.g. PS-HT1205 class) ─
// Driven low-side via NPN transistor (GPIO -> 1k -> base, emitter -> GND,
// collector -> buzzer(-), buzzer(+) -> 5V). Active-high: GPIO HIGH = sound.
// NOTE: pin reserved here; beep driver + UI hooks are a follow-up step.
namespace BuzzerConfig {
    inline constexpr int PIN = 40;   // GPIO40 (J3-8, MTDO) — no boot-strapping role
}

// ─── Onboard RGB LED (WS2812, ESP32-S3-DevKitC-1 GPIO48) ──
namespace LedConfig {    inline constexpr int        PIN                = 48;  // DevKitC-1 onboard WS2812
    inline constexpr uint32_t   RMT_RESOLUTION_HZ  = 10000000;  // 10 MHz → 0.1 us/tick
    inline constexpr int        SUCCESS_BLINKS     = 5;   // green blink count
    inline constexpr int        FAILURE_BLINKS     = 5;   // red blink count
    inline constexpr uint32_t   BLINK_ON_MS        = 200;
    inline constexpr uint32_t   BLINK_OFF_MS       = 200;
    inline constexpr uint8_t    SUCCESS_BRIGHTNESS = 180; // green 0–255
    inline constexpr uint8_t    FAILURE_BRIGHTNESS = 255; // red 0–255
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
    inline constexpr uint32_t BALANCE_STACK     = 8192;
    inline constexpr uint32_t UI_STACK          = 8192;
    inline constexpr uint32_t LED_STACK         = 4096;

    // Priorities (higher number = higher priority)
    inline constexpr int WALLET_PRIORITY        = 10;  // Highest — security critical
    inline constexpr int UI_PRIORITY            = 7;
    inline constexpr int WIFI_PRIORITY          = 6;
    inline constexpr int AI_PRIORITY            = 5;   // Intentionally lower than wallet
    inline constexpr int WEATHER_PRIORITY       = 3;
    inline constexpr int PRICE_PRIORITY         = 3;
    inline constexpr int BALANCE_PRIORITY       = 3;
    inline constexpr int BUTTON_PRIORITY        = 8;
    inline constexpr int LED_PRIORITY           = 2;   // Idle most of the time

    // CPU core assignment
    inline constexpr int WALLET_CORE            = 1;   // Core 1: security-critical only
    inline constexpr int UI_CORE                = 0;   // Core 0: UI + console
    inline constexpr int WIFI_CORE              = 0;
    inline constexpr int AI_CORE                = 0;
    inline constexpr int PRICE_CORE             = 1;   // Core 1: TLS won't starve IDLE0
    inline constexpr int WEATHER_CORE           = 1;
    inline constexpr int BALANCE_CORE           = 1;
    inline constexpr int BUTTON_CORE            = 1;
    inline constexpr int LED_CORE               = 0;
}

// ─── Queue Depths ─────────────────────────────────────────
namespace Queues {
    inline constexpr int BUTTON_EVENTS          = 8;
    inline constexpr int WALLET_REQUESTS        = 4;
    inline constexpr int UI_COMMANDS            = 16;
    inline constexpr int AI_MESSAGES            = 8;
    inline constexpr int LED_COMMANDS           = 4;
}

// ─── Timing ───────────────────────────────────────────────
namespace Timing {
    inline constexpr uint32_t IDLE_SCREEN_CYCLE_MS   = 10000;  // 10s per idle screen
    inline constexpr uint32_t WEATHER_UPDATE_MS       = 600000; // 10 minutes
    inline constexpr uint32_t PRICE_UPDATE_MS         = 300000; // 5 minutes
    inline constexpr uint32_t BALANCE_POLL_MS         = 30000;  // 30 seconds
    inline constexpr uint32_t WIFI_RECONNECT_DELAY_MS = 5000;
    inline constexpr uint32_t SESSION_TIMEOUT_MS      = 300000; // 5 min auto-lock
    inline constexpr uint32_t DISPLAY_UPDATE_MS       = 100;    // 10 FPS UI refresh
}

// ─── Pomodoro ─────────────────────────────────────────────
namespace Pomodoro {
    inline constexpr uint8_t  MAX_MIN              = 60;   // 60:00 cap per phase
    inline constexpr uint8_t  MAX_LOOPS            = 5;    // work+break pairs
    inline constexpr uint8_t  DEFAULT_WORK_MIN     = 25;
    inline constexpr uint8_t  DEFAULT_BREAK_MIN    = 5;
    inline constexpr uint32_t HOLD_REPEAT_START_MS = 500;  // hold B3/B4 before auto-repeat
    inline constexpr uint32_t HOLD_REPEAT_RATE_MS  = 120;  // repeat interval while held
    inline constexpr uint32_t SETUP_TIMEOUT_MS     = 30000;// setup screens -> menu after 30s idle
    // Finish alert: 4 x (ON 180ms / OFF 180ms) — active buzzer, timing only
    inline constexpr uint8_t  ALERT_BEEPS          = 4;
    inline constexpr uint32_t ALERT_ON_MS          = 180;
    inline constexpr uint32_t ALERT_OFF_MS         = 180;
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
    // Binance — no API key, cert always in ESP-IDF bundle (DigiCert)
    inline constexpr const char* SOL_PRICE_URL =
        "https://api.binance.com/api/v3/ticker/price?symbol=SOLUSDT";
    // 24h ticker: lastPrice + highPrice + lowPrice + priceChangePercent
    inline constexpr const char* SOL_PRICE_24H_URL =
        "https://api.binance.com/api/v3/ticker/24hr?symbol=SOLUSDT";

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
