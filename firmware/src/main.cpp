// ============================================================
// Fuchey — main.cpp
// System entry point. Boot sequence, queue allocation,
// subsystem initialization, and FreeRTOS task pinning.
// ============================================================

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "cJSON.h"
#include <cstring>
#include <cctype>
#include <cmath>
#include <string>
#include <array>
#include <vector>
#include <cstdint>

#include "../lib/config/Config.hpp"
#include "../lib/events/Events.hpp"
#include "../lib/storage/Storage.hpp"
#include "../lib/display/Display.hpp"
#include "../lib/display/ui/UIManager.hpp"
#include "../lib/led_indicator/LedIndicator.hpp"
#include "../lib/buzzer/Buzzer.hpp"
#include "../lib/buttons/ButtonDriver.hpp"
#include "../lib/crypto/CryptoEngine.hpp"
#include "../lib/crypto/Base58.hpp"
#include "../lib/crypto/SHA256.hpp"
#include "../lib/crypto/Ed25519.hpp"
#include "../lib/wallet/WalletCore.hpp"
#include "../lib/wallet_manager/WalletManager.hpp"
#include "../lib/wifi/WiFiManager.hpp"
#include "../lib/weather/WeatherService.hpp"
#include "../lib/price/PriceService.hpp"
#include "../lib/balance/BalanceMonitor.hpp"
#include "../lib/protocol/UsbProtocol.hpp"

namespace Fuchey {
namespace Events {

// Define global handles declared as extern in Events.hpp
QueueHandle_t g_wallet_queue = nullptr;
QueueHandle_t g_ui_queue     = nullptr;
QueueHandle_t g_button_queue = nullptr;
EventGroupHandle_t g_event_group = nullptr;

} // namespace Events
} // namespace Fuchey

namespace Fuchey {
QueueHandle_t g_tx_confirm_queue = nullptr;
}
QueueHandle_t g_button_queue_ref = nullptr;

static constexpr const char* TAG = "FucheyMain";

// Core System Objects
static Fuchey::Display        s_display;
static Fuchey::UIManager      s_ui(s_display);
static Fuchey::LedIndicator   s_led_indicator;
static Fuchey::Buzzer         s_buzzer;
static Fuchey::ButtonDriver   s_buttons(Fuchey::Buttons::PIN_B1_TX_BACK,
                                        Fuchey::Buttons::PIN_B2_MENU_SELECT,
                                        Fuchey::Buttons::PIN_B3_PREV,
                                        Fuchey::Buttons::PIN_B4_NEXT,
                                        Fuchey::Buttons::DEBOUNCE_MS,
                                        Fuchey::Buttons::LONG_PRESS_MS);
Fuchey::WalletCore            s_wallet_core;
namespace Fuchey { WalletCore* g_wallet_core_ptr = nullptr; }
static Fuchey::WalletManager  s_wallet_manager(s_wallet_core);

static Fuchey::WiFiManager    s_wifi_manager;
static Fuchey::WeatherService s_weather_service(s_wifi_manager);
static Fuchey::PriceService   s_price_service(s_wifi_manager);

// ─── Network State ─────────────────────────────────────────
static bool s_is_devnet = true; // Default to devnet for prototyping

static const char* get_rpc_url() {
    return s_is_devnet ? Fuchey::API::SOLANA_DEVNET_RPC : Fuchey::API::SOLANA_MAINNET_RPC;
}

static const char* get_usdc_mint() {
    return s_is_devnet ? Fuchey::API::USDC_DEVNET_MINT : Fuchey::API::USDC_MAINNET_MINT;
}

static Fuchey::BalanceMonitor s_balance_monitor(s_wifi_manager, "", get_usdc_mint(), get_rpc_url());

static bool is_mainnet() { return !s_is_devnet; }

// Device settings the companion app may change (WiFi, weather location).
// Same services and side effects as the console `w` / `setloc` commands.
static void apply_network_to_services();

class AppDeviceSettings final : public Fuchey::DeviceSettings {
public:
    Fuchey::DeviceStatus status() override {
        Fuchey::DeviceStatus st;
        st.wifi_configured     = s_wifi_manager.has_credentials();
        st.wifi_connected      = s_wifi_manager.is_connected();
        st.online              = s_wifi_manager.has_ip();
        st.wifi_ssid           = s_wifi_manager.saved_ssid();
        st.location_configured = s_weather_service.has_configured_location();
        st.city                = s_weather_service.city_name();
        st.lat                 = s_weather_service.lat();
        st.lon                 = s_weather_service.lon();
        st.setup_done = st.wifi_configured && st.location_configured &&
                        s_wallet_core.get_address().has_value();
        return st;
    }

    bool scan_wifi(std::vector<Fuchey::WifiNetwork>& out) override {
        std::vector<Fuchey::WiFiManager::ScanResult> res;
        if (!s_wifi_manager.scan(res)) return false;
        out.clear();
        for (auto& r : res) out.push_back({r.ssid, r.rssi, r.secure});
        return true;
    }

    bool set_wifi(const char* ssid, const char* password) override {
        ESP_LOGI(TAG, "[App] WiFi: saving credentials and connecting to SSID: %s", ssid);
        if (!s_wifi_manager.save_credentials(ssid, password)) return false;
        s_wifi_manager.disconnect();                 // drop any old network first
        const bool ok = s_wifi_manager.connect_from_nvs();
        s_ui.mark_wifi_configured(ssid);
        return ok;
    }

    bool set_location(const char* city, float lat, float lon) override {
        ESP_LOGI(TAG, "[App] Weather location: %s (%.4f, %.4f)", city, lat, lon);
        if (!s_weather_service.set_manual_location(city, lat, lon)) return false;
        s_ui.mark_location_configured(city);
        // Refresh off the console task (blocking HTTP).
        xTaskCreate([](void*) {
            s_weather_service.update_now();
            vTaskDelete(nullptr);
        }, "wx_now", 8192, nullptr, 3, nullptr);
        return true;
    }

    void set_network(bool mainnet) override {
        s_is_devnet = !mainnet;
        Fuchey::Storage::Handle cfg(Fuchey::NVS::CONFIG_NS, NVS_READWRITE);
        if (cfg.is_open()) {
            cfg.set_str(Fuchey::NVS::KEY_NETWORK, mainnet ? "mainnet" : "devnet");
            cfg.commit();
        }
        ESP_LOGI(TAG, "[App] Network switched to %s (approved on device)",
                 mainnet ? "MAINNET-BETA" : "DEVNET");
        apply_network_to_services();
    }

    // Same follow-up as a successful console wallet_import.
    void on_wallet_restored(const std::string& address) override {
        ESP_LOGI(TAG, "[App] Wallet restored on device: %s", address.c_str());
        Fuchey::Events::Event evt{};
        evt.type = Fuchey::Events::EventType::WALLET_IMPORTED;
        Fuchey::Events::post(Fuchey::Events::g_wallet_queue, evt);
        s_ui.mark_wallet_configured(address.c_str());
        s_balance_monitor.set_address(address);
    }
};
static AppDeviceSettings s_app_settings;
static Fuchey::WalletCreateSession s_wallet_create;   // app "Create wallet" (RAM only)
static Fuchey::RecoveryController  s_recovery;        // app scrambled-grid phrase entry

// Companion-app protocol ("@@" framed lines on the USB console).
static Fuchey::UsbProtocol s_usb_protocol(s_wallet_core, s_wallet_manager,
                                          s_price_service, is_mainnet,
                                          &s_app_settings);

// Re-point network-dependent services at the current s_is_devnet selection.
// The monitor snapshots its URL/mint at construction (devnet default, before
// NVS loads), so it must be re-applied after the NVS load and on switches.
static void apply_network_to_services() {
    s_balance_monitor.set_network(get_rpc_url(), get_usdc_mint());
}

// ─── Helpers ──────────────────────────────────────────────

// Check if string is a Solana hex private key: 32-byte seed or 64-byte seed+pubkey.
static bool is_hex_private_key(const char* s) {
    size_t len = strlen(s);
    if (len != 64 && len != 128) return false;
    for (size_t i = 0; i < len; ++i) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

static bool is_single_base58_token(const char* s) {
    if (!s || !*s) return false;
    for (const char* p = s; *p; ++p) {
        if (*p == ' ' || *p == '\t') return false;
        if (!std::strchr(Fuchey::Crypto::Base58::ALPHABET, *p)) return false;
    }
    return true;
}

// ─── Balance pre-check helpers ─────────────────────────────

// Map a Solana RPC error message to a short, human-friendly string.
// Insufficient-funds errors (payment for SOL, SPL Token "custom program error: 0x1")
// are rewritten to a clear message; everything else passes through unchanged.
static void friendly_tx_error(const char* raw, char* out, size_t out_len) {
    if (!raw || !*raw) {
        snprintf(out, out_len, "%s", "Unknown RPC error");
        return;
    }
    std::string lower = raw;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower.find("insufficient") != std::string::npos ||
        lower.find("custom program error: 0x1") != std::string::npos) {
        snprintf(out, out_len, "%s", "Insufficient balance");
        return;
    }
    snprintf(out, out_len, "%.*s", static_cast<int>(out_len - 1), raw);
}

// Fetch SOL balance in lamports via getBalance. Returns -1.0 on RPC/parse failure.
static double fetch_sol_balance_lamports(const std::string& address) {
    if (!s_wifi_manager.has_ip()) return -1.0;

    char req[256];
    snprintf(req, sizeof(req),
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getBalance\",\"params\":[\"%s\"]}",
             address.c_str());

    auto resp = s_wifi_manager.post_json(get_rpc_url(), req);
    if (!resp.success) {
        ESP_LOGE(TAG, "[PreCheck] SOL balance RPC failed (HTTP %d)", resp.status_code);
        return -1.0;
    }

    cJSON* root = cJSON_Parse(resp.body.c_str());
    if (!root) return -1.0;

    double lamports = -1.0;
    cJSON* res = cJSON_GetObjectItem(root, "result");
    cJSON* val = res ? cJSON_GetObjectItem(res, "value") : nullptr;
    if (val && cJSON_IsNumber(val)) lamports = val->valuedouble;
    cJSON_Delete(root);
    return lamports;
}

// One USDC token account owned by an address, resolved from the chain.
struct UsdcAccount {
    std::string  pubkey;        // base58 token account address (empty if owner has none)
    double       balance = 0.0; // balance in whole USDC units (for display only)
    uint64_t     balance_micro = 0; // exact balance in 6-decimal micro-units (authoritative)
    bool         ok = false;    // true when the RPC call itself succeeded
};

// Resolve an owner's real USDC token account (address + balance) via
// getTokenAccountsByOwner. This is the same command used for balance display,
// and always targets the actual funded account instead of guessing a PDA.
static UsdcAccount fetch_usdc_account(const std::string& address) {
    UsdcAccount out;

    if (!s_wifi_manager.has_ip()) return out;

    char req[512];
    snprintf(req, sizeof(req),
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getTokenAccountsByOwner\","
             "\"params\":[\"%s\",{\"mint\":\"%s\"},{\"encoding\":\"jsonParsed\"}]}",
             address.c_str(), get_usdc_mint());

    auto resp = s_wifi_manager.post_json(get_rpc_url(), req);
    if (!resp.success) {
        ESP_LOGE(TAG, "[Pre-check] USDC account RPC failed (HTTP %d)", resp.status_code);
        return out;
    }

    cJSON* root = cJSON_Parse(resp.body.c_str());
    if (!root) return out;

    cJSON* res = cJSON_GetObjectItem(root, "result");
    cJSON* val = res ? cJSON_GetObjectItem(res, "value") : nullptr;
    if (cJSON_IsArray(val) && cJSON_GetArraySize(val) > 0) {
        int n = cJSON_GetArraySize(val);
        bool found = false;
        // A wallet can own several USDC accounts (e.g. an empty one plus a funded
        // one). Pick the account with the largest exact balance so we always send
        // from (and report) the account that can actually fund the transfer.
        for (int i = 0; i < n; ++i) {
            cJSON* item0   = cJSON_GetArrayItem(val, i);
            cJSON* pk      = cJSON_GetObjectItem(item0, "pubkey");
            cJSON* account = cJSON_GetObjectItem(item0, "account");
            cJSON* data    = account ? cJSON_GetObjectItem(account, "data")   : nullptr;
            cJSON* parsed  = data    ? cJSON_GetObjectItem(data,    "parsed") : nullptr;
            cJSON* info    = parsed  ? cJSON_GetObjectItem(parsed,  "info")   : nullptr;
            cJSON* t_amt   = info    ? cJSON_GetObjectItem(info,    "tokenAmount") : nullptr;
            // "amount" is the exact integer in micro-units (string); uiAmount is a
            // rounded float used only for human display.
            uint64_t micro = 0;
            cJSON* ui_amt = nullptr;
            if (t_amt) {
                cJSON* amt = cJSON_GetObjectItem(t_amt, "amount");
                ui_amt      = cJSON_GetObjectItem(t_amt, "uiAmount");
                if (amt && amt->valuestring) micro = strtoull(amt->valuestring, nullptr, 10);
            }
            if (!found || micro > out.balance_micro) {
                found = true;
                out.balance_micro = micro;
                if (pk && pk->valuestring) out.pubkey = pk->valuestring;
                if (ui_amt && cJSON_IsNumber(ui_amt)) out.balance = ui_amt->valuedouble;
            }
        }
    }
    cJSON_Delete(root);

    out.ok = true;
    return out;
}

static int count_bip39_words(const char* s) {
    int count = 0;
    while (s && *s) {
        while (*s && !std::isalpha(static_cast<unsigned char>(*s))) ++s;
        if (*s) {
            ++count;
            while (*s && std::isalpha(static_cast<unsigned char>(*s))) ++s;
        }
    }
    return count;
}

static std::string normalize_bip39_payload(const char* s) {
    std::string normalized;
    while (s && *s) {
        while (*s && !std::isalpha(static_cast<unsigned char>(*s))) ++s;
        if (!*s) break;

        if (!normalized.empty()) normalized += ' ';
        while (*s && std::isalpha(static_cast<unsigned char>(*s))) {
            normalized += static_cast<char>(std::tolower(static_cast<unsigned char>(*s)));
            ++s;
        }
    }
    return normalized;
}

// ─── Transfer helpers (console send sol / send usdc) ───────

struct SendArgs {
    bool usdc = false;
    char amount[32] = {};     // decimal string as typed, e.g. "0.25"
    char recipient[64] = {};  // base58 wallet address
};

// Exact decimal → integer base units ("0.25", 9 → 250000000). No floats:
// rejects signs, exponents, more fractional digits than `decimals`, overflow.
static bool parse_amount_units(const char* s, uint8_t decimals, uint64_t& out) {
    if (!s || !*s) return false;
    uint64_t whole = 0, frac = 0;
    int frac_digits = 0;
    bool seen_dot = false, seen_digit = false;
    for (const char* p = s; *p; ++p) {
        if (*p == '.') {
            if (seen_dot) return false;
            seen_dot = true;
            continue;
        }
        if (*p < '0' || *p > '9') return false;
        seen_digit = true;
        uint64_t d = static_cast<uint64_t>(*p - '0');
        if (seen_dot) {
            if (++frac_digits > decimals) return false;
            frac = frac * 10 + d;
        } else {
            if (whole > (UINT64_MAX - d) / 10) return false;
            whole = whole * 10 + d;
        }
    }
    if (!seen_digit) return false;
    uint64_t scale = 1;
    for (uint8_t i = 0; i < decimals; ++i) scale *= 10;
    for (int i = frac_digits; i < decimals; ++i) frac *= 10;
    if (whole > (UINT64_MAX - frac) / scale) return false;
    out = whole * scale + frac;
    return true;
}

static void post_tx_fail(const char* reason, const std::string& recipient) {
    Fuchey::Events::Event fail_evt{};
    fail_evt.type = Fuchey::Events::EventType::TX_BROADCAST_FAIL;
    snprintf(reinterpret_cast<char*>(fail_evt.data.tx.tx_data),
             sizeof(fail_evt.data.tx.tx_data), "%s|%s", reason, recipient.c_str());
    Fuchey::Events::post(Fuchey::Events::g_ui_queue, fail_evt);
}

// "confirmed", not "finalized": a finalized blockhash is already ~13 s old,
// and on devnet a blockhash can expire ~35 s after it is produced — too tight
// once the price fetch and the 30 s B1 window are added.
static bool fetch_latest_blockhash(std::array<uint8_t, 32>& out, const char* tag) {
    auto resp = s_wifi_manager.post_json(
        get_rpc_url(),
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getLatestBlockhash\",\"params\":[{\"commitment\":\"confirmed\"}]}");
    if (!resp.success) {
        ESP_LOGE(TAG, "[%s] getLatestBlockhash failed (HTTP %d)", tag, resp.status_code);
        return false;
    }

    std::string blockhash_str;
    cJSON* root = cJSON_Parse(resp.body.c_str());
    if (root) {
        cJSON* res = cJSON_GetObjectItem(root, "result");
        cJSON* val = res ? cJSON_GetObjectItem(res, "value") : nullptr;
        cJSON* bh  = val ? cJSON_GetObjectItem(val, "blockhash") : nullptr;
        if (bh && bh->valuestring) blockhash_str = bh->valuestring;
        cJSON_Delete(root);
    }

    auto bytes = Fuchey::Crypto::Base58::decode(blockhash_str);
    if (bytes.size() != 32) {
        ESP_LOGE(TAG, "[%s] Bad blockhash response: %.100s", tag, resp.body.c_str());
        return false;
    }
    std::copy(bytes.begin(), bytes.end(), out.begin());
    return true;
}

static void push_compact_u16(std::vector<uint8_t>& v, uint16_t n) {
    while (true) {
        uint8_t b = n & 0x7F;
        n >>= 7;
        if (n == 0) { v.push_back(b); return; }
        v.push_back(b | 0x80);
    }
}

static void push_u64_le(std::vector<uint8_t>& v, uint64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back(static_cast<uint8_t>((x >> (i * 8)) & 0xFF));
}

// Hand `msg` to WalletManager (parse → TFT → physical B1 → sign), then
// broadcast and report the result to the UI + log.
static void sign_and_broadcast(const char* tag, const std::vector<uint8_t>& msg,
                               const char* asset, const std::string& recipient) {
    // Fresh SOL/USD rate for the confirm screen (display only). On failure
    // fall back to the last cached rate, or show no USD at all — never the
    // PriceService placeholder price.
    const bool price_live = s_price_service.update_now();
    Fuchey::SignContext ctx{};
    ctx.mainnet    = !s_is_devnet;
    ctx.sol_usd    = s_price_service.has_data() ? s_price_service.get_sol_usd() : 0.0f;
    ctx.price_live = price_live;

    ESP_LOGI(TAG, "[%s] Check the device screen and tap B1 to sign (double/long press = reject)", tag);
    Fuchey::SignResult res = s_wallet_manager.sign_transaction(msg, ctx);
    if (res.status != Fuchey::SignStatus::SIGNED) {
        ESP_LOGW(TAG, "[%s] Not sent: %s%s%s", tag,
                 Fuchey::WalletManager::status_to_string(res.status),
                 res.status == Fuchey::SignStatus::UNSUPPORTED_TX ? " — " : "",
                 res.status == Fuchey::SignStatus::UNSUPPORTED_TX
                     ? Fuchey::TxParser::error_to_string(res.parse_error) : "");
        // User reject / timeout already left the confirm screen; report the rest.
        if (res.status != Fuchey::SignStatus::REJECTED &&
            res.status != Fuchey::SignStatus::TIMED_OUT &&
            res.status != Fuchey::SignStatus::CANCELLED) {
            post_tx_fail(Fuchey::WalletManager::status_to_string(res.status), recipient);
        }
        return;
    }

    // Wire format: [compact-u16 num_sigs][sig...][message]
    std::vector<uint8_t> wire_tx;
    wire_tx.push_back(1);
    wire_tx.insert(wire_tx.end(), res.signature.begin(), res.signature.end());
    wire_tx.insert(wire_tx.end(), msg.begin(), msg.end());

    std::string body = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"sendTransaction\",\"params\":[\"";
    body += Fuchey::Crypto::Base58::encode(wire_tx);
    body += "\",{\"encoding\":\"base58\"}]}";

    ESP_LOGI(TAG, "[%s] Broadcasting to Solana %s...", tag, s_is_devnet ? "Devnet" : "Mainnet-Beta");
    auto tx_resp = s_wifi_manager.post_json(get_rpc_url(), body.c_str());

    char amount_str[24];
    Fuchey::TxParser::format_units(res.parsed.amount, res.parsed.decimals,
                                   amount_str, sizeof(amount_str));

    Fuchey::Events::Event result_evt{};
    result_evt.type = Fuchey::Events::EventType::TX_BROADCAST_FAIL;
    snprintf(reinterpret_cast<char*>(result_evt.data.tx.tx_data),
             sizeof(result_evt.data.tx.tx_data), "%s:%s:%s", asset, amount_str, recipient.c_str());

    char err_buf[64] = {};
    if (!tx_resp.success) {
        ESP_LOGE(TAG, "  [TX ERROR] HTTP request failed (%d)", tx_resp.status_code);
        snprintf(err_buf, sizeof(err_buf), "HTTP %d", tx_resp.status_code);
    } else if (cJSON* root = cJSON_Parse(tx_resp.body.c_str())) {
        cJSON* tx_res = cJSON_GetObjectItem(root, "result");
        cJSON* tx_err = cJSON_GetObjectItem(root, "error");
        if (tx_res && tx_res->valuestring) {
            ESP_LOGI(TAG, "=================================================");
            ESP_LOGI(TAG, "  [SUCCESS] %s %s sent", amount_str, asset);
            ESP_LOGI(TAG, "  Signature: %s", tx_res->valuestring);
            ESP_LOGI(TAG, "  Explorer:  https://explorer.solana.com/tx/%s%s",
                     tx_res->valuestring, s_is_devnet ? "?cluster=devnet" : "");
            ESP_LOGI(TAG, "=================================================");
            result_evt.type = Fuchey::Events::EventType::TX_BROADCAST_OK;
        } else {
            cJSON* msg_item = tx_err ? cJSON_GetObjectItem(tx_err, "message") : nullptr;
            const char* raw = msg_item && msg_item->valuestring ? msg_item->valuestring : "RPC parse error";
            ESP_LOGE(TAG, "  [TX ERROR] %s", raw);
            friendly_tx_error(raw, err_buf, sizeof(err_buf));
        }
        cJSON_Delete(root);
    } else {
        ESP_LOGE(TAG, "  [TX ERROR] Parse failure. Raw: %.150s", tx_resp.body.c_str());
        snprintf(err_buf, sizeof(err_buf), "%s", "RPC parse failure");
    }

    if (result_evt.type == Fuchey::Events::EventType::TX_BROADCAST_FAIL) {
        snprintf(reinterpret_cast<char*>(result_evt.data.tx.tx_data),
                 sizeof(result_evt.data.tx.tx_data), "%s|%s", err_buf, recipient.c_str());
    }
    Fuchey::Events::post(Fuchey::Events::g_ui_queue, result_evt);
}

static void run_send_sol(const SendArgs& args) {
    static constexpr const char* T = "SEND SOL";
    const std::string recipient = args.recipient;

    uint64_t lamports = 0;
    if (!parse_amount_units(args.amount, 9, lamports) || lamports == 0) {
        ESP_LOGE(TAG, "[%s] Invalid amount '%s' (max 9 decimals, > 0)", T, args.amount);
        return;
    }
    auto recipient_bytes = Fuchey::Crypto::Base58::decode(recipient);
    if (recipient_bytes.size() != 32) {
        ESP_LOGE(TAG, "[%s] Invalid recipient address (%d bytes, expected 32)", T,
                 static_cast<int>(recipient_bytes.size()));
        return;
    }
    auto pubkey_opt = s_wallet_core.get_pubkey();
    auto addr_opt   = s_wallet_core.get_address();
    if (!pubkey_opt || !addr_opt) {
        ESP_LOGE(TAG, "[%s] No wallet configured", T);
        return;
    }

    // Balance pre-check (advisory; the chain is authoritative).
    double have = fetch_sol_balance_lamports(*addr_opt);
    constexpr uint64_t FEE_BUFFER_LAMPORTS = 10000;
    if (have >= 0.0 && static_cast<uint64_t>(have) < lamports + FEE_BUFFER_LAMPORTS) {
        ESP_LOGE(TAG, "[%s] Insufficient balance: have %.9f SOL", T, have / 1e9);
        post_tx_fail("Insufficient balance", recipient);
        return;
    } else if (have < 0.0) {
        ESP_LOGW(TAG, "[%s] Could not verify balance — proceeding anyway.", T);
    }

    std::array<uint8_t, 32> blockhash{};
    if (!fetch_latest_blockhash(blockhash, T)) {
        post_tx_fail("No blockhash", recipient);
        return;
    }

    // Legacy message: System Program Transfer
    std::vector<uint8_t> msg;
    msg.push_back(1);  // num_required_signatures
    msg.push_back(0);  // num_readonly_signed
    msg.push_back(1);  // num_readonly_unsigned (System Program)
    push_compact_u16(msg, 3);
    msg.insert(msg.end(), pubkey_opt->begin(), pubkey_opt->end());         // [0] from (signer, w)
    msg.insert(msg.end(), recipient_bytes.begin(), recipient_bytes.end()); // [1] to (w)
    msg.insert(msg.end(), 32, 0);                                          // [2] System Program
    msg.insert(msg.end(), blockhash.begin(), blockhash.end());
    push_compact_u16(msg, 1);  // 1 instruction
    msg.push_back(2);          // program index
    push_compact_u16(msg, 2);
    msg.push_back(0);
    msg.push_back(1);
    push_compact_u16(msg, 12);
    msg.push_back(2); msg.push_back(0); msg.push_back(0); msg.push_back(0);  // Transfer
    push_u64_le(msg, lamports);

    sign_and_broadcast(T, msg, "SOL", recipient);
}

static void run_send_usdc(const SendArgs& args) {
    static constexpr const char* T = "SEND USDC";
    const std::string recipient = args.recipient;

    uint64_t raw_amount = 0;
    if (!parse_amount_units(args.amount, 6, raw_amount) || raw_amount == 0) {
        ESP_LOGE(TAG, "[%s] Invalid amount '%s' (max 6 decimals, > 0)", T, args.amount);
        return;
    }
    if (Fuchey::Crypto::Base58::decode(recipient).size() != 32) {
        ESP_LOGE(TAG, "[%s] Invalid recipient address", T);
        return;
    }
    auto pubkey_opt = s_wallet_core.get_pubkey();
    auto addr_opt   = s_wallet_core.get_address();
    if (!pubkey_opt || !addr_opt) {
        ESP_LOGE(TAG, "[%s] No wallet configured", T);
        return;
    }

    auto token_prog = Fuchey::Crypto::Base58::decode("TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA");
    auto mint       = Fuchey::Crypto::Base58::decode(get_usdc_mint());
    if (token_prog.size() != 32 || mint.size() != 32) {
        ESP_LOGE(TAG, "[%s] Bad program/mint constant", T);
        return;
    }

    // Resolve the real token accounts (no PDA guessing).
    UsdcAccount sender_acct = fetch_usdc_account(*addr_opt);
    if (!sender_acct.ok) {
        ESP_LOGE(TAG, "[%s] Could not look up sender USDC account", T);
        post_tx_fail("RPC error", recipient);
        return;
    }
    if (sender_acct.pubkey.empty() || sender_acct.balance_micro < raw_amount) {
        ESP_LOGE(TAG, "[%s] Insufficient balance: have %llu.%06llu USDC", T,
                 static_cast<unsigned long long>(sender_acct.balance_micro / 1000000),
                 static_cast<unsigned long long>(sender_acct.balance_micro % 1000000));
        post_tx_fail("Insufficient balance", recipient);
        return;
    }
    UsdcAccount recipient_acct = fetch_usdc_account(recipient);
    if (!recipient_acct.ok || recipient_acct.pubkey.empty()) {
        ESP_LOGE(TAG, "[%s] Recipient has no USDC token account (must receive USDC once first)", T);
        post_tx_fail("No USDC acct", recipient);
        return;
    }
    auto source = Fuchey::Crypto::Base58::decode(sender_acct.pubkey);
    auto dest   = Fuchey::Crypto::Base58::decode(recipient_acct.pubkey);
    if (source.size() != 32 || dest.size() != 32) {
        ESP_LOGE(TAG, "[%s] Bad token account address from RPC", T);
        return;
    }
    ESP_LOGI(TAG, "[%s] From token acct %s -> %s (owner %s)", T,
             sender_acct.pubkey.c_str(), recipient_acct.pubkey.c_str(), recipient.c_str());

    std::array<uint8_t, 32> blockhash{};
    if (!fetch_latest_blockhash(blockhash, T)) {
        post_tx_fail("No blockhash", recipient);
        return;
    }

    // Legacy message: SPL Token TransferChecked (mint + decimals are verified
    // on-chain and let the device parser confirm the token is USDC).
    std::vector<uint8_t> msg;
    msg.push_back(1);  // num_required_signatures
    msg.push_back(0);  // num_readonly_signed
    msg.push_back(2);  // num_readonly_unsigned (mint, Token Program)
    push_compact_u16(msg, 5);
    msg.insert(msg.end(), pubkey_opt->begin(), pubkey_opt->end()); // [0] owner (signer, w)
    msg.insert(msg.end(), source.begin(), source.end());           // [1] source (w)
    msg.insert(msg.end(), dest.begin(), dest.end());               // [2] destination (w)
    msg.insert(msg.end(), mint.begin(), mint.end());               // [3] mint
    msg.insert(msg.end(), token_prog.begin(), token_prog.end());   // [4] Token Program
    msg.insert(msg.end(), blockhash.begin(), blockhash.end());
    push_compact_u16(msg, 1);
    msg.push_back(4);          // program index
    push_compact_u16(msg, 4);  // source, mint, destination, owner
    msg.push_back(1);
    msg.push_back(3);
    msg.push_back(2);
    msg.push_back(0);
    push_compact_u16(msg, 10);
    msg.push_back(12);         // TransferChecked
    push_u64_le(msg, raw_amount);
    msg.push_back(6);          // decimals

    sign_and_broadcast(T, msg, "USDC", recipient);
}

extern "C" void app_main(void) {
    // Console = USB-Serial-JTAG. Install its driver so stdin reads block
    // (fgets in the console task) and long companion frames (~2.4 KB) fit.
    {
        usb_serial_jtag_driver_config_t usj = {};
        usj.tx_buffer_size = 4096;
        usj.rx_buffer_size = 4096;
        if (usb_serial_jtag_driver_install(&usj) == ESP_OK) {
            usb_serial_jtag_vfs_use_driver();
        }
    }
    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, "  Fuchey Firmware v%s", Fuchey::FW_VERSION);
    ESP_LOGI(TAG, "  Target: ESP32-S3 (16MB Flash, 8MB OPI PSRAM)");
    ESP_LOGI(TAG, "=================================================");

    // 1. Allocate Queues & Event Groups
    Fuchey::Events::g_wallet_queue = xQueueCreate(Fuchey::Queues::WALLET_REQUESTS, sizeof(Fuchey::Events::Event));
    Fuchey::Events::g_ui_queue     = xQueueCreate(Fuchey::Queues::UI_COMMANDS,     sizeof(Fuchey::Events::Event));
    Fuchey::Events::g_button_queue = xQueueCreate(Fuchey::Queues::BUTTON_EVENTS,   sizeof(Fuchey::ButtonState));
    Fuchey::Events::g_event_group  = xEventGroupCreate();

    g_button_queue_ref = Fuchey::Events::g_button_queue;
    Fuchey::g_tx_confirm_queue = xQueueCreate(4, sizeof(Fuchey::Events::Event));

    // 2. Initialize System Layer (NVS, Storage, Drivers)
    ESP_ERROR_CHECK(Fuchey::Storage::init());
    ESP_LOGI(TAG, "[OK] Storage (NVS) initialized");

    // Legacy cleanup: the AI assistant was removed. Wipe any LLM API key /
    // endpoint an older build left in NVS. Delete this block once every
    // device has been re-flashed once.
    {
        nvs_handle_t legacy;
        if (nvs_open("fuchey_ai", NVS_READONLY, &legacy) == ESP_OK) {
            nvs_close(legacy);
            if (nvs_open("fuchey_ai", NVS_READWRITE, &legacy) == ESP_OK) {
                nvs_erase_all(legacy);
                nvs_commit(legacy);
                nvs_close(legacy);
            }
        }
    }

    // Load network setting from NVS (default devnet)
    {
        Fuchey::Storage::Handle cfg(Fuchey::NVS::CONFIG_NS, NVS_READONLY);
        if (cfg.is_open()) {
            auto net = cfg.get_str(Fuchey::NVS::KEY_NETWORK);
            if (net && *net == "mainnet") {
                s_is_devnet = false;
            }
        }
    }
    ESP_LOGI(TAG, "[OK] Network configured: %s", s_is_devnet ? "Solana Devnet" : "Solana Mainnet-Beta");
    apply_network_to_services();

    // Buttons before display: harmless either way now that the display
    // (8/9/5/16/6) shares no pins with the buttons (4/10/17/13).
    if (!s_buttons.init(Fuchey::Events::g_button_queue)) {
        ESP_LOGE(TAG, "[!!] Button Driver initialization failed");
    } else {
        ESP_LOGI(TAG, "[OK] Button driver initialized "
                      "(B1_TX_BACK=GPIO%d, B2_MENU_SELECT=GPIO%d, B3_PREV=GPIO%d, B4_NEXT=GPIO%d)",
                 Fuchey::Buttons::PIN_B1_TX_BACK, Fuchey::Buttons::PIN_B2_MENU_SELECT,
                 Fuchey::Buttons::PIN_B3_PREV, Fuchey::Buttons::PIN_B4_NEXT);
    }

    if (!s_display.init()) {
        ESP_LOGE(TAG, "[!!] Display initialization failed — continuing without OLED");
    } else {
        ESP_LOGI(TAG, "[OK] Display initialized");
    }
    s_ui.init();

    if (!s_led_indicator.init()) {
        ESP_LOGE(TAG, "[!!] RGB LED initialization failed — continuing without indicator");
    } else {
        ESP_LOGI(TAG, "[OK] RGB LED indicator initialized");
    }

    // Buzzer: park GPIO40 LOW immediately so the NPN stays off (a floating
    // pin lets the transistor conduct and the buzzer sounds continuously).
    if (!s_buzzer.init()) {
        ESP_LOGE(TAG, "[!!] Buzzer initialization failed — continuing without buzzer");
    } else {
        ESP_LOGI(TAG, "[OK] Buzzer initialized (silent)");
    }

    Fuchey::g_wallet_core_ptr = &s_wallet_core;
    s_wallet_core.init();
    s_wallet_manager.init();
    ESP_LOGI(TAG, "[OK] Wallet core initialized — state: %s",
             s_wallet_core.has_wallet() ? "LOCKED (wallet found)" : "UNINITIALIZED (no wallet)");

    if (s_wallet_core.has_wallet()) {
        auto addr_opt = s_wallet_core.get_address();
        if (addr_opt) {
            s_balance_monitor.set_address(*addr_opt);
        }
    }

    // 4. Initialize Network & Services
    s_wifi_manager.init();
    s_weather_service.init();
    s_price_service.init();
    ESP_LOGI(TAG, "[OK] Network services initialized");

    // Attempt WiFi auto-connect from NVS
    s_wifi_manager.connect_from_nvs();

    // Initialize NTP (auto-syncs after WiFi gets IP)
    setenv("TZ", "NPT-5:45", 1);  // Nepal Time (UTC+5:45)
    tzset();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    // Boot animation — WiFi is already connecting in the background
    s_display.animate_boot(3000);

    // 5. Detect first-boot state for UIManager setup screen
    {
        bool wifi_missing    = !s_wifi_manager.has_credentials();
        bool wallet_missing  = !s_wallet_core.has_wallet();
        bool location_missing = !s_weather_service.has_configured_location();
        s_ui.set_setup_needed(wifi_missing, wallet_missing, location_missing);

        ESP_LOGI(TAG, "-------------------------------------------------");
        ESP_LOGI(TAG, "  Boot State:");
        ESP_LOGI(TAG, "    WiFi credentials : %s", wifi_missing     ? "MISSING" : "SAVED");
        ESP_LOGI(TAG, "    Wallet           : %s", wallet_missing   ? "MISSING" : "FOUND");
        ESP_LOGI(TAG, "    Weather location : %s", location_missing ? "MISSING" : "SAVED");
        ESP_LOGI(TAG, "-------------------------------------------------");
    }

    // 6. Spawn FreeRTOS Tasks with Strict CPU Core Pinning & Priorities
    ESP_LOGI(TAG, "Spawning FreeRTOS tasks...");

    // UI Task (Core 0)
    xTaskCreatePinnedToCore(Fuchey::UIManager::task_entry, "ui_task",
                            Fuchey::Tasks::UI_STACK, &s_ui,
                            Fuchey::Tasks::UI_PRIORITY, nullptr, Fuchey::Tasks::UI_CORE);

    // Wallet Manager Task (Core 1 — Security Critical)
    xTaskCreatePinnedToCore(Fuchey::WalletManager::task_entry, "wallet_mgr_task",
                            Fuchey::Tasks::WALLET_STACK, &s_wallet_manager,
                            Fuchey::Tasks::WALLET_PRIORITY, nullptr, Fuchey::Tasks::WALLET_CORE);

    // Weather Task (Core 1 — TLS won't starve IDLE0 on CPU 0)
    xTaskCreatePinnedToCore(Fuchey::WeatherService::task_entry, "weather_task",
                            Fuchey::Tasks::WEATHER_STACK, &s_weather_service,
                            Fuchey::Tasks::WEATHER_PRIORITY, nullptr, Fuchey::Tasks::WEATHER_CORE);

    // Price Task (Core 1 — TLS won't starve IDLE0 on CPU 0)
    xTaskCreatePinnedToCore(Fuchey::PriceService::task_entry, "price_task",
                             Fuchey::Tasks::PRICE_STACK, &s_price_service,
                             Fuchey::Tasks::PRICE_PRIORITY, nullptr, Fuchey::Tasks::PRICE_CORE);

    // RGB LED Indicator Task (Core 0 — idle until a TX result arrives)
    xTaskCreatePinnedToCore(Fuchey::LedIndicator::task_entry, "led_task",
                            Fuchey::Tasks::LED_STACK, &s_led_indicator,
                            Fuchey::Tasks::LED_PRIORITY, nullptr, Fuchey::Tasks::LED_CORE);

    // Wire BalanceMonitor to UIManager for on-demand balance fetch
    s_ui.set_balance_monitor(&s_balance_monitor);

    // Wire PriceService to UIManager for cached 24h market data (no HTTP on UI task)
    s_ui.set_price_service(&s_price_service);

    // Wire RGB LED indicator to UIManager for TX result feedback
    s_ui.set_led_indicator(&s_led_indicator);

    // Wire buzzer to UIManager for Pomodoro finish alerts (non-blocking pattern)
    s_ui.set_buzzer(&s_buzzer);

    // App-driven wallet creation: UI draws the words, protocol confirms.
    s_ui.set_create_session(&s_wallet_create);
    s_usb_protocol.set_create_session(&s_wallet_create);
    s_ui.set_recovery(&s_recovery);
    s_usb_protocol.set_recovery(&s_recovery);

    // Interactive Serial Console Task (Core 0)
    xTaskCreatePinnedToCore([](void*) {
        static constexpr const char* CTAG = "Console";

        ESP_LOGI(CTAG, "=================================================");
        ESP_LOGI(CTAG, "  Fuchey Interactive Serial Console Ready");
        ESP_LOGI(CTAG, "=================================================");
        ESP_LOGI(CTAG, "  Commands:");
        ESP_LOGI(CTAG, "    w <ssid> <pass>           WiFi connect & save");
        ESP_LOGI(CTAG, "    wallet_create              Generate new wallet");
        ESP_LOGI(CTAG, "    wallet_import <mnemonic>   Import BIP39 mnemonic");
        ESP_LOGI(CTAG, "    wallet_import <key>        Import hex/base58 private key");
        ESP_LOGI(CTAG, "    wallet_selftest            Verify Ed25519 math");
        ESP_LOGI(CTAG, "    wallet_info                Show current address");
        ESP_LOGI(CTAG, "    wallet_export              Export private key (DANGER)");
        ESP_LOGI(CTAG, "    p                          Force SOL price fetch");
        ESP_LOGI(CTAG, "    c / 1                      B1 press (Back; cannot approve TX)");
        ESP_LOGI(CTAG, "    x / 3                      B1 double-press (tx reject)");
        ESP_LOGI(CTAG, "    l                          B1 long-press (tx reject)");
        ESP_LOGI(CTAG, "    n / menu                   B2 press (open menu / select)");
        ESP_LOGI(CTAG, "    m / select                 B2 press (alias: choose option)");
        ESP_LOGI(CTAG, "    j / prev                   B3 press (previous item)");
        ESP_LOGI(CTAG, "    k / next                   B4 press (next item)");
        ESP_LOGI(CTAG, "    b / 2                      B1 press (alias: Back)");
        ESP_LOGI(CTAG, "    pass                       Worlds Fair banner screen");
        ESP_LOGI(CTAG, "    pomodoro                   Pomodoro timer screen");
        ESP_LOGI(CTAG, "    h / ?                      Show this help");
        ESP_LOGI(CTAG, "=================================================");

        // Sized for companion-app frames (base64 of a 1232-byte tx + JSON).
        char line[Fuchey::UsbProtocol::MAX_LINE_CHARS + 64];
        std::string pending_line;
        std::string pending_wallet_import;
        while (true) {
            if (fgets(line, sizeof(line), stdin)) {
                size_t chunk_len = strlen(line);
                bool line_complete = chunk_len > 0 &&
                                     (line[chunk_len - 1] == '\r' || line[chunk_len - 1] == '\n');

                pending_line.append(line, chunk_len);
                if (!line_complete && pending_line.size() < sizeof(line) - 1) {
                    continue;
                }
                if (pending_line.size() >= sizeof(line)) {
                    ESP_LOGW(CTAG, "Command too long; discarding input (%d bytes)",
                             static_cast<int>(pending_line.size()));
                    pending_line.clear();
                    continue;
                }

                std::strncpy(line, pending_line.c_str(), sizeof(line));
                line[sizeof(line) - 1] = '\0';
                pending_line.clear();

                size_t len = strlen(line);
                while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
                    line[--len] = '\0';
                }
                // Trim leading whitespace
                char* cmd = line;
                while (*cmd == ' ' || *cmd == '\t') ++cmd;
                len = strlen(cmd);
                while (len > 0 && (cmd[len - 1] == ' ' || cmd[len - 1] == '\t')) {
                    cmd[--len] = '\0';
                }
                if (len == 0) continue;

                // Companion-app frames ("@@...") never reach the command table.
                if (s_usb_protocol.handle_line(cmd)) continue;

                if (!pending_wallet_import.empty() &&
                    strcmp(cmd, "wallet_import_cancel") != 0) {
                    const char* continuation = cmd;
                    if (strncmp(cmd, "wallet_import ", 14) == 0 && len > 14) {
                        continuation = cmd + 14;
                        while (*continuation == ' ' || *continuation == '\t') ++continuation;
                    }
                    pending_wallet_import += ' ';
                    pending_wallet_import += continuation;
                    std::strncpy(line, pending_wallet_import.c_str(), sizeof(line));
                    line[sizeof(line) - 1] = '\0';
                    cmd = line;
                    len = strlen(cmd);
                }

                if (strcmp(cmd, "wallet_import_cancel") == 0) {
                    std::fill(pending_wallet_import.begin(), pending_wallet_import.end(), '\0');
                    pending_wallet_import.clear();
                    ESP_LOGW(CTAG, "[Wallet] Pending import cancelled");
                    continue;
                }

                // ── B1 (TX confirm / Back) ───────────────────────
                if ((cmd[0] == 'c' || cmd[0] == '1') && len == 1) {
                    ESP_LOGI(CTAG, "[INPUT] B1 press (tx accept / back)");
                    Fuchey::ButtonState state{
                        .id = Fuchey::ButtonId::B1_TX_BACK,
                        .event = Fuchey::ButtonEvent::PRESS,
                        .timestamp_ms = 0,
                        .from_console = true
                    };
                    xQueueSend(::g_button_queue_ref, &state, 0);

                // ── B1 double-press (tx reject) ────────────────
                } else if ((cmd[0] == 'x' || cmd[0] == '3') && len == 1) {
                    ESP_LOGI(CTAG, "[INPUT] B1 double-press (tx reject)");
                    Fuchey::ButtonState state{
                        .id = Fuchey::ButtonId::B1_TX_BACK,
                        .event = Fuchey::ButtonEvent::DOUBLE_PRESS,
                        .timestamp_ms = 0,
                        .from_console = true
                    };
                    xQueueSend(::g_button_queue_ref, &state, 0);

                // ── B1 long-press (tx reject) ──────────────────
                } else if (cmd[0] == 'l' && len == 1) {
                    ESP_LOGI(CTAG, "[INPUT] B1 long-press (tx reject)");
                    Fuchey::ButtonState state{
                        .id = Fuchey::ButtonId::B1_TX_BACK,
                        .event = Fuchey::ButtonEvent::LONG_PRESS,
                        .timestamp_ms = 0,
                        .from_console = true
                    };
                    xQueueSend(::g_button_queue_ref, &state, 0);

                // ── B2 (open menu / select) ────────────────────
                } else if (strcmp(cmd, "n") == 0 || strcmp(cmd, "menu") == 0 ||
                           (cmd[0] == 'm' && len == 1) || strcmp(cmd, "select") == 0) {
                    ESP_LOGI(CTAG, "[INPUT] B2 press (open menu / select)");
                    Fuchey::ButtonState state{
                        .id = Fuchey::ButtonId::B2_MENU_SELECT,
                        .event = Fuchey::ButtonEvent::PRESS,
                        .timestamp_ms = 0,
                        .from_console = true
                    };
                    xQueueSend(::g_button_queue_ref, &state, 0);

                // ── B3 (previous) ──────────────────────────────
                } else if (strcmp(cmd, "j") == 0 || strcmp(cmd, "prev") == 0) {
                    ESP_LOGI(CTAG, "[INPUT] B3 press (previous)");
                    Fuchey::ButtonState state{
                        .id = Fuchey::ButtonId::B3_PREV,
                        .event = Fuchey::ButtonEvent::PRESS,
                        .timestamp_ms = 0,
                        .from_console = true
                    };
                    xQueueSend(::g_button_queue_ref, &state, 0);

                // ── B4 (next) ──────────────────────────────────
                } else if (strcmp(cmd, "k") == 0 || strcmp(cmd, "next") == 0) {
                    ESP_LOGI(CTAG, "[INPUT] B4 press (next)");
                    Fuchey::ButtonState state{
                        .id = Fuchey::ButtonId::B4_NEXT,
                        .event = Fuchey::ButtonEvent::PRESS,
                        .timestamp_ms = 0,
                        .from_console = true
                    };
                    xQueueSend(::g_button_queue_ref, &state, 0);

                // ── B1 alias (Back) ────────────────────────────
                } else if ((cmd[0] == 'b' || cmd[0] == '2') && len == 1) {
                    ESP_LOGI(CTAG, "[INPUT] B1 (Back)");
                    Fuchey::ButtonState state{
                        .id = Fuchey::ButtonId::B1_TX_BACK,
                        .event = Fuchey::ButtonEvent::PRESS,
                        .timestamp_ms = 0,
                        .from_console = true
                    };
                    xQueueSend(::g_button_queue_ref, &state, 0);

                // ── menu command ──────────────────────────────
                } else if (strcmp(cmd, "menu") == 0) {
                    ESP_LOGI(CTAG, "[Console] Opening Main Menu on OLED screen");
                    s_ui.set_screen(Fuchey::UIScreen::MENU_MAIN);

                // ── WiFi connect ──────────────────────────────
                } else if (cmd[0] == 'w' && cmd[1] == ' ' && len > 2) {
                    char ssid[64] = {0}, pass[64] = {0};
                    int parsed = sscanf(cmd + 2, "%63s %63s", ssid, pass);
                    if (parsed >= 1) {
                        ESP_LOGI(CTAG, "-------------------------------------------------");
                        ESP_LOGI(CTAG, "[WiFi] Saving credentials and connecting...");
                        ESP_LOGI(CTAG, "       SSID: %s", ssid);
                        ESP_LOGI(CTAG, "-------------------------------------------------");
                        s_wifi_manager.save_credentials(ssid, pass);
                        s_wifi_manager.connect(ssid, pass);
                        s_ui.mark_wifi_configured(ssid);
                    } else {
                        ESP_LOGW(CTAG, "Usage: w <SSID> <PASSWORD>");
                    }

                // ── Force SOL price update ────────────────────
                } else if (cmd[0] == 'p' && len == 1) {
                    ESP_LOGI(CTAG, "[Input] Fetching live SOL price...");
                    s_price_service.update_now();

                // ── wallet_info ───────────────────────────────
                } else if (strcmp(cmd, "wallet_info") == 0) {
                    auto addr = s_wallet_core.get_address();
                    if (addr) {
                        ESP_LOGI(CTAG, "-------------------------------------------------");
                        ESP_LOGI(CTAG, "  Wallet Address: %s", addr->c_str());
                        ESP_LOGI(CTAG, "  State: %s",
                                 s_wallet_core.is_unlocked() ? "UNLOCKED" : "LOCKED");
                        ESP_LOGI(CTAG, "-------------------------------------------------");
                    } else {
                        ESP_LOGW(CTAG, "  No wallet configured yet.");
                    }

                // ── wallet_selftest ───────────────────────────
                } else if (strcmp(cmd, "wallet_selftest") == 0) {
                    ESP_LOGI(CTAG, "-------------------------------------------------");
                    ESP_LOGI(CTAG, "[Wallet] Running Ed25519 RFC8032 self-test...");
                    if (Fuchey::Crypto::Ed25519::self_test()) {
                        ESP_LOGI(CTAG, "[Wallet] Self-test PASSED");
                    } else {
                        ESP_LOGE(CTAG, "[Wallet] Self-test FAILED");
                    }
                    ESP_LOGI(CTAG, "-------------------------------------------------");

                // ── qr ────────────────────────────────────────
                } else if (strcmp(cmd, "qr") == 0) {
                    ESP_LOGI(CTAG, "[UI] Switching to Wallet QR screen");
                    s_ui.set_screen(Fuchey::UIScreen::WALLET_QR);

                // ── pass ──────────────────────────────────────
                } else if (strcmp(cmd, "pass") == 0) {
                    ESP_LOGI(CTAG, "[UI] Switching to FAIR_PASS screen");
                    s_ui.set_screen(Fuchey::UIScreen::FAIR_PASS);

                // ── pomodoro ──────────────────────────────────
                } else if (strcmp(cmd, "pomodoro") == 0) {
                    ESP_LOGI(CTAG, "[UI] Switching to POMODORO_VIEW screen");
                    s_ui.set_screen(Fuchey::UIScreen::POMODORO_VIEW);

                // ── balance ───────────────────────────────────
                } else if (strcmp(cmd, "balance") == 0) {
                    auto addr = s_wallet_core.get_address();
                    if (!addr) {
                        ESP_LOGW(CTAG, "No wallet configured yet.");
                    } else if (!s_wifi_manager.has_ip()) {
                        ESP_LOGW(CTAG, "WiFi not connected — cannot fetch balance.");
                    } else {
                        xTaskCreate([](void*) {
                            static constexpr const char* CTAG = "Console";
                            auto addr_opt = s_wallet_core.get_address();
                            if (!addr_opt) { vTaskDelete(nullptr); return; }
                            std::string address = *addr_opt;

                            ESP_LOGI(CTAG, "-------------------------------------------------");
                            ESP_LOGI(CTAG, "[Balance] Querying %s (%s) for %s...",
                                     s_is_devnet ? "Devnet" : "Mainnet-Beta",
                                     get_rpc_url(),
                                     address.c_str());

                            // ── Fetch SOL balance ──────────────────────────
                            char sol_req[256];
                            snprintf(sol_req, sizeof(sol_req),
                                     "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getBalance\",\"params\":[\"%s\"]}",
                                     address.c_str());

                            double sol_bal   = 0.0;
                            double lamports  = 0.0;
                            bool   sol_ok    = false;

                            auto resp = s_wifi_manager.post_json(get_rpc_url(), sol_req);
                            if (resp.success) {
                                cJSON* root = cJSON_Parse(resp.body.c_str());
                                if (root) {
                                    cJSON* res = cJSON_GetObjectItem(root, "result");
                                    cJSON* val = res ? cJSON_GetObjectItem(res, "value") : nullptr;
                                    if (val && cJSON_IsNumber(val)) {
                                        lamports = val->valuedouble;
                                        sol_bal  = lamports / 1000000000.0;
                                        sol_ok   = true;
                                    } else {
                                        cJSON* err_item = cJSON_GetObjectItem(root, "error");
                                        if (err_item) {
                                            cJSON* msg = cJSON_GetObjectItem(err_item, "message");
                                            ESP_LOGE(CTAG, "  SOL RPC Error: %s",
                                                     msg && msg->valuestring ? msg->valuestring : "Unknown error");
                                        }
                                    }
                                    cJSON_Delete(root);
                                } else {
                                    ESP_LOGE(CTAG, "  SOL RPC Parse Error. Raw: %.100s", resp.body.c_str());
                                }
                            } else {
                                ESP_LOGE(CTAG, "Failed to query SOL balance (HTTP %d)", resp.status_code);
                            }

                            // Small delay between RPC calls
                            vTaskDelay(pdMS_TO_TICKS(100));

                            // ── Fetch USDC balance ─────────────────────────
                            char usdc_req[512];
                            snprintf(usdc_req, sizeof(usdc_req),
                                     "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getTokenAccountsByOwner\","
                                     "\"params\":[\"%s\",{\"mint\":\"%s\"},{\"encoding\":\"jsonParsed\"}]}",
                                     address.c_str(), get_usdc_mint());

                            double usdc_bal = 0.0;
                            bool   usdc_ok  = false;

                            auto u_resp = s_wifi_manager.post_json(get_rpc_url(), usdc_req);
                            if (u_resp.success) {
                                cJSON* root = cJSON_Parse(u_resp.body.c_str());
                                if (root) {
                                    cJSON* res = cJSON_GetObjectItem(root, "result");
                                    cJSON* val = res ? cJSON_GetObjectItem(res, "value") : nullptr;
                                    if (cJSON_IsArray(val) && cJSON_GetArraySize(val) > 0) {
                                        cJSON* item0   = cJSON_GetArrayItem(val, 0);
                                        cJSON* account = cJSON_GetObjectItem(item0, "account");
                                        cJSON* data    = account ? cJSON_GetObjectItem(account, "data")   : nullptr;
                                        cJSON* parsed  = data    ? cJSON_GetObjectItem(data,    "parsed") : nullptr;
                                        cJSON* info    = parsed  ? cJSON_GetObjectItem(parsed,  "info")   : nullptr;
                                        cJSON* t_amt   = info    ? cJSON_GetObjectItem(info,    "tokenAmount") : nullptr;
                                        cJSON* ui_amt  = t_amt   ? cJSON_GetObjectItem(t_amt,   "uiAmount")   : nullptr;
                                        if (ui_amt && cJSON_IsNumber(ui_amt)) {
                                            usdc_bal = ui_amt->valuedouble;
                                            usdc_ok  = true;
                                        }
                                    } else {
                                        usdc_ok = true; // account exists, just 0 balance
                                    }
                                    cJSON_Delete(root);
                                }
                            } else {
                                ESP_LOGE(CTAG, "Failed to query USDC balance (HTTP %d)", u_resp.status_code);
                            }

                            // ── Print both on one line ──────────────────────
                            ESP_LOGI(CTAG, "  SOL: %.6f SOL  |  USDC: $%.2f",
                                     sol_ok ? sol_bal : 0.0,
                                     usdc_ok ? usdc_bal : 0.0);
                            ESP_LOGI(CTAG, "-------------------------------------------------");
                            vTaskDelete(nullptr);
                        }, "bal_task", 8192, nullptr, 4, nullptr);
                    }

                // ── network ───────────────────────────────────
                } else if (strcmp(cmd, "network") == 0) {
                    ESP_LOGI(CTAG, "=================================================");
                    ESP_LOGI(CTAG, "  Solana Network: %s", s_is_devnet ? "DEVNET" : "MAINNET-BETA");
                    ESP_LOGI(CTAG, "  RPC Endpoint:   %s", get_rpc_url());
                    ESP_LOGI(CTAG, "  USDC Mint:      %s", get_usdc_mint());
                    ESP_LOGI(CTAG, "  Commands:");
                    ESP_LOGI(CTAG, "    network devnet   - Switch to Solana Devnet");
                    ESP_LOGI(CTAG, "    network mainnet  - Switch to Solana Mainnet");
                    ESP_LOGI(CTAG, "=================================================");

                } else if (strcmp(cmd, "network devnet") == 0) {
                    s_is_devnet = true;
                    Fuchey::Storage::Handle cfg(Fuchey::NVS::CONFIG_NS, NVS_READWRITE);
                    if (cfg.is_open()) {
                        cfg.set_str(Fuchey::NVS::KEY_NETWORK, "devnet");
                        cfg.commit();
                    }
                    ESP_LOGI(CTAG, "-------------------------------------------------");
                    ESP_LOGI(CTAG, "[Network] Switched to Solana DEVNET");
                    ESP_LOGI(CTAG, "  RPC: %s", get_rpc_url());
                    ESP_LOGI(CTAG, "-------------------------------------------------");
                    apply_network_to_services();

                } else if (strcmp(cmd, "network mainnet") == 0) {
                    s_is_devnet = false;
                    Fuchey::Storage::Handle cfg(Fuchey::NVS::CONFIG_NS, NVS_READWRITE);
                    if (cfg.is_open()) {
                        cfg.set_str(Fuchey::NVS::KEY_NETWORK, "mainnet");
                        cfg.commit();
                    }
                    ESP_LOGI(CTAG, "-------------------------------------------------");
                    ESP_LOGI(CTAG, "[Network] Switched to Solana MAINNET-BETA");
                    ESP_LOGI(CTAG, "  RPC: %s", get_rpc_url());
                    ESP_LOGI(CTAG, "-------------------------------------------------");
                    apply_network_to_services();

                // ── Devnet Airdrop ────────────────────────────
                } else if (strncmp(cmd, "airdrop", 7) == 0) {
                    auto addr = s_wallet_core.get_address();
                    if (!addr) {
                        ESP_LOGW(CTAG, "No wallet configured yet.");
                    } else if (!s_is_devnet) {
                        ESP_LOGW(CTAG, "Airdrop is only available on Devnet! Type 'network devnet' first.");
                    } else if (!s_wifi_manager.has_ip()) {
                        ESP_LOGW(CTAG, "WiFi not connected.");
                    } else {
                        float sol_amt = 1.0f;
                        sscanf(cmd + 7, "%f", &sol_amt);
                        if (sol_amt <= 0.0f) sol_amt = 1.0f;

                        xTaskCreate([](void* arg) {
                            static constexpr const char* CTAG = "Console";
                            float amount = *static_cast<float*>(arg);
                            delete static_cast<float*>(arg);

                            auto addr_opt = s_wallet_core.get_address();
                            if (!addr_opt) { vTaskDelete(nullptr); return; }
                            std::string address = *addr_opt;

                            uint64_t lamports = static_cast<uint64_t>(amount * 1000000000.0f);
                            ESP_LOGI(CTAG, "-------------------------------------------------");
                            ESP_LOGI(CTAG, "[Airdrop] Requesting %.1f Devnet SOL for %s...", amount, address.c_str());

                            char req[384];
                            snprintf(req, sizeof(req),
                                     "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"requestAirdrop\",\"params\":[\"%s\",%llu]}",
                                     address.c_str(), lamports);

                            auto resp = s_wifi_manager.post_json(get_rpc_url(), req);
                            if (resp.success) {
                                cJSON* root = cJSON_Parse(resp.body.c_str());
                                cJSON* res = root ? cJSON_GetObjectItem(root, "result") : nullptr;
                                if (res && cJSON_IsString(res)) {
                                    ESP_LOGI(CTAG, "  Airdrop SUCCESS! Tx Signature:");
                                    ESP_LOGI(CTAG, "  %s", res->valuestring);
                                } else {
                                    ESP_LOGW(CTAG, "  Airdrop requested (response received)");
                                }
                                if (root) cJSON_Delete(root);
                            } else {
                                ESP_LOGE(CTAG, "Airdrop request failed");
                            }
                            ESP_LOGI(CTAG, "-------------------------------------------------");
                            vTaskDelete(nullptr);
                        }, "drop_task", 8192, new float(sol_amt), 4, nullptr);
                    }

                // ── wallet_create ─────────────────────────────
                } else if (strcmp(cmd, "wallet_create") == 0) {
                    ESP_LOGI(CTAG, "-------------------------------------------------");
                    ESP_LOGI(CTAG, "[Wallet] Generating new 12-word wallet...");
                    std::string mnemonic;
                    Fuchey::WalletResult r = s_wallet_core.create(12, mnemonic);
                    if (r == Fuchey::WalletResult::OK) {
                        auto addr = s_wallet_core.get_address();
                        ESP_LOGI(CTAG, "[Wallet] CREATED SUCCESSFULLY!");
                        ESP_LOGI(CTAG, "  Address: %s", addr ? addr->c_str() : "(error)");
                        ESP_LOGI(CTAG, "  ------- BACKUP YOUR SEED PHRASE -------");
                        ESP_LOGI(CTAG, "  %s", mnemonic.c_str());
                        ESP_LOGI(CTAG, "  ----------------------------------------");
                        ESP_LOGI(CTAG, "  WARNING: Save these 12 words securely!");
                        ESP_LOGI(CTAG, "  They will NOT be shown again.");
                        // Zero mnemonic from memory after logging
                        memset(&mnemonic[0], 0, mnemonic.size());
                        mnemonic.clear();

                        std::string addr_str = addr ? *addr : "";
                        Fuchey::Events::Event evt{};
                        evt.type = Fuchey::Events::EventType::WALLET_CREATED;
                        Fuchey::Events::post(Fuchey::Events::g_wallet_queue, evt);
                        s_ui.mark_wallet_configured(addr_str.c_str());
                        s_balance_monitor.set_address(addr_str);
                    } else if (r == Fuchey::WalletResult::ERR_ALREADY_EXISTS) {
                        ESP_LOGW(CTAG, "[Wallet] A wallet already exists!");
                        ESP_LOGW(CTAG, "  Use 'wallet_reset' first to erase it, then 'wallet_create'.");
                        auto addr = s_wallet_core.get_address();
                        ESP_LOGW(CTAG, "  Current address: %s", addr ? addr->c_str() : "(unknown)");
                    } else {
                        ESP_LOGE(CTAG, "[Wallet] Creation FAILED (err=%d)", static_cast<int>(r));
                    }
                    ESP_LOGI(CTAG, "-------------------------------------------------");

                // ── wallet_reset ──────────────────────────────
                } else if (strcmp(cmd, "wallet_reset") == 0) {
                    ESP_LOGW(CTAG, "-------------------------------------------------");
                    ESP_LOGW(CTAG, "[Wallet] FACTORY RESET — erasing wallet from NVS...");
                    Fuchey::WalletResult r = s_wallet_core.factory_reset();
                    if (r == Fuchey::WalletResult::OK) {
                        ESP_LOGW(CTAG, "[Wallet] Reset complete. You can now run 'wallet_create'.");
                    } else {
                        ESP_LOGE(CTAG, "[Wallet] Reset FAILED (err=%d)", static_cast<int>(r));
                    }
                    ESP_LOGW(CTAG, "-------------------------------------------------");

                // ── wallet_export (DEBUG) ──────────────────────
                // Kept for development. Needs a physical B1 tap on the
                // dedicated EXPORT KEY? screen (console c cannot approve).
                } else if (strcmp(cmd, "wallet_export") == 0) {
                    auto addr = s_wallet_core.get_address();
                    if (!addr || !s_wallet_core.is_unlocked()) {
                        ESP_LOGW(CTAG, "[Wallet] No unlocked wallet to export.");
                        continue;
                    }
                    ESP_LOGW(CTAG, "==================================================");
                    ESP_LOGW(CTAG, "[Wallet] EXPORT PRIVATE KEY (debug)");
                    ESP_LOGW(CTAG, "  DANGER: Anyone with this key can steal your funds.");
                    ESP_LOGW(CTAG, "  Tap B1 on the device within 10s to print it,");
                    ESP_LOGW(CTAG, "  or double/long press B1 to cancel.");
                    ESP_LOGW(CTAG, "==================================================");

                    Fuchey::Events::TxSummary summary{};
                    summary.kind = Fuchey::Events::ConfirmKind::EXPORT_KEY;
                    summary.mainnet = !s_is_devnet;
                    snprintf(summary.recipient, sizeof(summary.recipient), "%s", addr->c_str());

                    auto decision = s_wallet_manager.request_confirmation(summary, 10000);
                    if (decision != Fuchey::SignStatus::APPROVED) {
                        ESP_LOGW(CTAG, "[Wallet] Export not performed: %s",
                                 Fuchey::WalletManager::status_to_string(decision));
                        continue;
                    }

                    auto secret = s_wallet_core.export_secret();
                    if (!secret) {
                        ESP_LOGE(CTAG, "[Wallet] Failed to export secret key (wallet locked?)");
                        continue;
                    }
                    std::string encoded = Fuchey::Crypto::Base58::encode(
                        std::span<const uint8_t>(secret->data(), secret->size()));
                    ESP_LOGW(CTAG, "--------------------------------------------------");
                    ESP_LOGW(CTAG, "[Wallet] PRIVATE KEY (base58 — keep secret!):");
                    ESP_LOGW(CTAG, "  %s", encoded.c_str());
                    ESP_LOGW(CTAG, "--------------------------------------------------");
                    std::fill(encoded.begin(), encoded.end(), '\0');
                    secret->fill(0);


                // ── wallet_import ─────────────────────────────
                } else if (strncmp(cmd, "wallet_import ", 14) == 0 && len > 14) {
                    const char* payload = cmd + 14;
                    // Skip leading spaces
                    while (*payload == ' ' || *payload == '\t') ++payload;

                    ESP_LOGI(CTAG, "-------------------------------------------------");
                    Fuchey::WalletResult r;

                    // Auto-detect format: hex, Solana base58 secret key, or BIP39.
                    if (is_hex_private_key(payload)) {
                        ESP_LOGI(CTAG, "[Wallet] Detected: %d-char hex private key", static_cast<int>(strlen(payload)));
                        r = s_wallet_core.import_privkey_hex(payload);
                    } else if (is_single_base58_token(payload)) {
                        size_t payload_len = strlen(payload);
                        if (payload_len < 87) {
                            pending_wallet_import = "wallet_import ";
                            pending_wallet_import += payload;
                            ESP_LOGW(CTAG, "[Wallet] Private key input looks incomplete (%d chars). Paste/type the remaining characters, or wallet_import_cancel.",
                                     static_cast<int>(payload_len));
                            ESP_LOGI(CTAG, "-------------------------------------------------");
                            continue;
                        }
                        ESP_LOGI(CTAG, "[Wallet] Detected: base58 private key");
                        r = s_wallet_core.import_privkey_base58(payload);
                    } else {
                        std::string normalized = normalize_bip39_payload(payload);
                        int word_count = count_bip39_words(normalized.c_str());
                        ESP_LOGI(CTAG, "[Wallet] Detected: BIP39 mnemonic (%d words)", word_count);
                        ESP_LOGI(CTAG, "[Wallet] Mnemonic payload length: raw=%d normalized=%d",
                                 static_cast<int>(strlen(payload)),
                                 static_cast<int>(normalized.size()));
                        if (word_count > 0 && word_count < 12) {
                            pending_wallet_import = "wallet_import ";
                            pending_wallet_import += normalized;
                            ESP_LOGW(CTAG, "[Wallet] Mnemonic input is incomplete (%d/12 words). Paste/type the remaining word(s), or wallet_import_cancel.",
                                     word_count);
                            std::fill(normalized.begin(), normalized.end(), '\0');
                            ESP_LOGI(CTAG, "-------------------------------------------------");
                            continue;
                        }
                        if (word_count > 12 && word_count < 24) {
                            pending_wallet_import = "wallet_import ";
                            pending_wallet_import += normalized;
                            ESP_LOGW(CTAG, "[Wallet] Mnemonic input is incomplete (%d/24 words). Paste/type the remaining word(s), or wallet_import_cancel.",
                                     word_count);
                            std::fill(normalized.begin(), normalized.end(), '\0');
                            ESP_LOGI(CTAG, "-------------------------------------------------");
                            continue;
                        }
                        r = s_wallet_core.import(normalized);
                        std::fill(normalized.begin(), normalized.end(), '\0');
                    }

                    if (r == Fuchey::WalletResult::OK) {
                        std::fill(pending_wallet_import.begin(), pending_wallet_import.end(), '\0');
                        pending_wallet_import.clear();
                        auto addr = s_wallet_core.get_address();
                        ESP_LOGI(CTAG, "[Wallet] IMPORTED SUCCESSFULLY!");
                        ESP_LOGI(CTAG, "  Address: %s", addr ? addr->c_str() : "(error)");

                        std::string addr_str = addr ? *addr : "";
                        Fuchey::Events::Event evt{};
                        evt.type = Fuchey::Events::EventType::WALLET_IMPORTED;
                        Fuchey::Events::post(Fuchey::Events::g_wallet_queue, evt);
                        s_ui.mark_wallet_configured(addr_str.c_str());
                        s_balance_monitor.set_address(addr_str);
                    } else {
                        const char* reason =
                            (r == Fuchey::WalletResult::ERR_INVALID_MNEMONIC) ? "invalid mnemonic" :
                            (r == Fuchey::WalletResult::ERR_INVALID_PRIVKEY)  ? "invalid private key (bad hex/base58?)" :
                            (r == Fuchey::WalletResult::ERR_ALREADY_EXISTS)   ? "wallet already exists" :
                            "unknown error";
                        ESP_LOGE(CTAG, "[Wallet] Import FAILED: %s", reason);
                    }

                    // Zero the input line for security
                    memset(cmd + 14, 0, len - 14);
                    ESP_LOGI(CTAG, "-------------------------------------------------");

                // ── Send SOL / USDC ───────────────────────────
                // Parsing/checks run in a worker task; the message is built
                // first, then WalletManager parses it, shows it on the TFT and
                // signs only after a physical B1 tap.
                } else if ((strncmp(cmd, "send sol ", 9) == 0 && len > 9) ||
                           (strncmp(cmd, "send usdc ", 10) == 0 && len > 10)) {
                    auto* args = new SendArgs();
                    args->usdc = (strncmp(cmd, "send usdc ", 10) == 0);
                    const char* rest = cmd + (args->usdc ? 10 : 9);

                    if (sscanf(rest, "%31s %63s", args->amount, args->recipient) == 2) {
                        xTaskCreate([](void* p) {
                            auto* a = static_cast<SendArgs*>(p);
                            SendArgs args = *a;
                            delete a;
                            if (args.usdc) {
                                run_send_usdc(args);
                            } else {
                                run_send_sol(args);
                            }
                            vTaskDelete(nullptr);
                        }, args->usdc ? "send_usdc_task" : "send_sol_task", 20480, args, 4, nullptr);
                    } else {
                        delete args;
                        ESP_LOGW(CTAG, "Usage: send sol|usdc <amount> <recipient_address>");
                    }

                // ── Interactive Send prompt ───────────────────
                } else if (strcmp(cmd, "send") == 0) {
                    ESP_LOGI(CTAG, "=================================================");
                    ESP_LOGI(CTAG, "  SEND TOKEN SELECTION:");
                    ESP_LOGI(CTAG, "    send sol  <amount> <recipient>");
                    ESP_LOGI(CTAG, "    send usdc <amount> <recipient>");
                    ESP_LOGI(CTAG, "  Example:");
                    ESP_LOGI(CTAG, "    send sol 0.25 7xKX...3b9Z");
                    ESP_LOGI(CTAG, "    send usdc 5.00 7xKX...3b9Z");
                    ESP_LOGI(CTAG, "=================================================");

                // ── Manual Weather update ──────────────────────
                } else if (strcmp(cmd, "weather") == 0) {
                    ESP_LOGI(CTAG, "[Weather] Fetching: %s (source=%s)",
                             s_weather_service.city_name().c_str(),
                             s_weather_service.location_source().c_str());
                    s_weather_service.update_now();

                // ── Set manual weather location ────────────────
                } else if (strncmp(cmd, "setloc", 6) == 0) {
                    // Accept optional commas between coords (e.g. "27.56, 84.30")
                    for (char* p = cmd; *p; ++p) {
                        if (*p == ',') *p = ' ';
                    }
                    char city[64] = {0}, end[4] = {0};
                    float lat = 0.0f, lon = 0.0f;
                    int parsed = sscanf(cmd + 6, " %63[^ ]%f%f%3s", city, &lat, &lon, end);
                    if (parsed == 3 && city[0]) {
                        ESP_LOGI(CTAG, "-------------------------------------------------");
                        ESP_LOGI(CTAG, "[Weather] Setting manual location: %s (%.4f, %.4f)",
                                 city, lat, lon);
                        ESP_LOGI(CTAG, "-------------------------------------------------");
                        if (s_weather_service.set_manual_location(city, lat, lon)) {
                            ESP_LOGI(CTAG, "[Weather] Location saved to NVS (source=manual)");
                            s_ui.mark_location_configured(city);
                            s_weather_service.update_now();
                        } else {
                            ESP_LOGE(CTAG, "Usage: setloc <CITY> <LAT> <LON>  (lat -90..90, lon -180..180)");
                        }
                    } else {
                        ESP_LOGW(CTAG, "Usage: setloc <CITY> <LAT> <LON>");
                        ESP_LOGW(CTAG, "  Example: setloc Chitwan 27.68 84.43");
                    }

                // ── Help ──────────────────────────────────────
                } else if ((cmd[0] == 'h' || cmd[0] == '?') && len == 1) {
                    ESP_LOGI(CTAG, "  w <ssid> <pass>          WiFi connect & save");
                    ESP_LOGI(CTAG, "  wallet_create            Generate new wallet");
                    ESP_LOGI(CTAG, "  wallet_import <mnemonic> Import BIP39 mnemonic");
                    ESP_LOGI(CTAG, "  wallet_import <key>      Import hex/base58 private key");
                    ESP_LOGI(CTAG, "  wallet_selftest          Verify Ed25519 math");
                    ESP_LOGI(CTAG, "  wallet_info              Show current address");
                    ESP_LOGI(CTAG, "  wallet_export            Export private key (DANGER)");
                    ESP_LOGI(CTAG, "  balance                  Fetch live SOL & USDC balance");
                    ESP_LOGI(CTAG, "  send                     Token transfer menu (SOL / USDC)");
                    ESP_LOGI(CTAG, "  send sol <amt> <to>      Transfer SOL");
                    ESP_LOGI(CTAG, "  send usdc <amt> <to>     Transfer USDC");
                    ESP_LOGI(CTAG, "  network                  Show / switch network (devnet/mainnet)");
                    ESP_LOGI(CTAG, "  airdrop [amount]         Request Devnet SOL airdrop");
                    ESP_LOGI(CTAG, "  weather                  Fetch weather now");
                    ESP_LOGI(CTAG, "  setloc <city> <lat> <lon> Set weather location");
                    ESP_LOGI(CTAG, "  p                        Fetch SOL price");
                    ESP_LOGI(CTAG, "  c / 1                    B1 button (Back; TX needs real B1)");
                    ESP_LOGI(CTAG, "  n / menu                 B2 button (open / select)");
                    ESP_LOGI(CTAG, "  j / prev                 B3 button (previous)");
                    ESP_LOGI(CTAG, "  k / next                 B4 button (next)");
                    ESP_LOGI(CTAG, "  b / 2                    B1 button (alias: Back)");

                } else {
                    ESP_LOGW(CTAG, "Unknown command: '%s'  (type 'h' for help)", cmd);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }, "console_task", 12288, nullptr, 4, nullptr, 0);

    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, "  Boot complete. Fuchey is running.");
    ESP_LOGI(TAG, "=================================================");
}
