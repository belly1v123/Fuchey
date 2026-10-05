#pragma once
// ============================================================
// Fuchey — UsbProtocol.hpp
// Companion-app protocol over the USB serial console (Web Serial).
//
// Framing: one line per message, sharing the port with ESP_LOG output.
//   @@<json>*<CRC32 as 8 uppercase hex>\n
// CRC32 = IEEE 802.3 (zlib) over the JSON bytes. Lines without the "@@"
// prefix are normal console commands / logs and are ignored by the app.
//
// Requests  (app → device): {"id":<u32>,"cmd":"<name>", ...}
// Responses (device → app): {"id":<u32>,"ok":true, ...}
//                           {"id":<u32>,"ok":false,"err":"<code>","detail":"..."}
// Events    (device → app): {"id":<u32>,"event":"<name>", ...}
//
// Commands (protocol v1):
//   hello                         → proto, fw, network, has_wallet, pubkey, max_tx
//   get_pubkey                    → pubkey
//   sign_tx  msg=<base64 message>, network="devnet"|"mainnet"
//            → event "awaiting_confirmation" {asset, amount, fee, to}
//            → {"ok":true,"sig":"<base64 64-byte signature>"}
//   cancel                        → withdraws the pending sign_tx
//   show_address                  → opens the Receive QR screen (read-only)
//   get_status                    → wifi {configured, connected, online, ssid},
//                                   location {configured, city, lat, lon}, setup_done
//   wifi_scan                     → networks [{ssid, rssi, secure}]
//   set_wifi  ssid, password      → saves + connects (password write-only)
//   set_location city, lat, lon   → saves the weather location
//   recovery_start purpose ("check"|"restore"), words (12|24)
//   recovery_tap pos (0..8)       → {word, total, mode} or {done, result[, address]}
//   recovery_cancel
//       Restore: after the last word Fuchey shows "SAVE WALLET?" with the
//       derived address (event awaiting_confirmation {address}) and stores
//       the wallet only after a hardware B1 tap.
//       Scrambled-grid phrase entry: the grid is drawn on the device only;
//       the host sends positions and learns progress, never letters/words.
//   wallet_create_start words (12|24) → device generates + shows the words
//   wallet_create_state           → {stage, page, pages, verify_n, wrong[, address]}
//   wallet_create_tap pos (0..8)  → confirm a word on the device's grid
//   wallet_create_cancel
//   set_network network ("devnet"|"mainnet")
//       → event awaiting_confirmation {network}; applied only after a
//         hardware B1 tap on Fuchey's "SWITCH NETWORK?" screen
//       → {ok, network, changed} or error rejected|timeout|busy
//       Only when no wallet exists. The words are shown on the device
//       screen only; nothing is stored until 3 words are confirmed.
// hello also returns caps=[...] naming optional features.
//
// Command tiers (see docs/ThreatModel.md):
//   read-only   hello, get_pubkey, get_status, wifi_scan, show_address
//   settings    set_wifi, set_location — device config only; refused while
//               a signature is pending; secrets never echoed or logged
//   signing     sign_tx — always parsed, shown, and approved by hardware B1
//   never here  approve, network switch, wallet create/import/export/reset
//
// The app can never approve: signing always goes through
// WalletManager::sign_transaction() (parse → TFT → physical B1).
// ============================================================

#include "../wallet/WalletCore.hpp"
#include "../wallet_manager/WalletManager.hpp"
#include "../price/PriceService.hpp"
#include "DeviceSettings.hpp"
#include "../wallet/RecoveryController.hpp"
#include "../wallet/WalletCreateSession.hpp"
#include <atomic>
#include <cstdint>
#include <vector>

struct cJSON;

namespace Fuchey {

class UsbProtocol {
public:
    static constexpr int      VERSION        = 1;
    static constexpr size_t   MAX_TX_BYTES   = 1232;  // Solana packet limit
    static constexpr size_t   MAX_LINE_CHARS = 2400;  // @@ + JSON(base64 1232 B) + CRC
    static constexpr const char* PREFIX      = "@@";

    using IsMainnetFn = bool (*)();

    UsbProtocol(WalletCore& core, WalletManager& manager,
                PriceService& price, IsMainnetFn is_mainnet,
                DeviceSettings* settings = nullptr);

    // Returns true when `line` is a protocol frame (handled here, including
    // malformed frames); false for ordinary console commands.
    bool handle_line(const char* line);

    // Shared with the UI (which draws the words and handles B1/B3/B4).
    void set_create_session(WalletCreateSession* s) { m_create = s; }
    void set_recovery(RecoveryController* r) { m_rc = r; }

    static uint32_t crc32(const uint8_t* data, size_t len);

private:
    struct SignJob {
        UsbProtocol*         self;
        uint32_t             id;
        std::vector<uint8_t> message;
    };

    WalletCore&       m_core;
    WalletManager&    m_manager;
    PriceService&     m_price;
    IsMainnetFn       m_is_mainnet;
    DeviceSettings*   m_settings;
    RecoveryController* m_rc{nullptr};
    bool              m_recovery_active{false};
    std::atomic<bool> m_recovery_confirming{false};   // restore: waiting for B1
    WalletCreateSession* m_create{nullptr};
    bool              m_create_active{false};
    std::atomic<bool> m_busy{false};

    void cmd_hello(uint32_t id);
    void cmd_get_pubkey(uint32_t id);
    void cmd_sign_tx(uint32_t id, cJSON* req);
    void cmd_cancel(uint32_t id);
    void cmd_show_address(uint32_t id);
    void cmd_get_status(uint32_t id);
    void cmd_wifi_scan(uint32_t id);
    void cmd_set_wifi(uint32_t id, cJSON* req);
    void cmd_set_location(uint32_t id, cJSON* req);
    void cmd_recovery_start(uint32_t id, cJSON* req);
    void cmd_recovery_tap(uint32_t id, cJSON* req);
    void cmd_recovery_cancel(uint32_t id);
    void recovery_post_view();
    void recovery_reply_progress(uint32_t id);
    void recovery_end();
    static void restore_worker(void* arg);
    void finish_recovery(uint32_t id, RecoveryController::Result result, const std::string& address);
    void cleanup_finished_sessions();
    void cmd_set_network(uint32_t id, cJSON* req);
    static void network_worker(void* arg);
    void cmd_wallet_create_start(uint32_t id, cJSON* req);
    void cmd_wallet_create_state(uint32_t id);
    void cmd_wallet_create_tap(uint32_t id, cJSON* req);
    void cmd_wallet_create_cancel(uint32_t id);
    void create_reply_state(uint32_t id);

    static void sign_worker(void* arg);
    void run_sign(SignJob& job);

    // Output helpers (each writes exactly one frame).
    static void send(cJSON* obj);  // takes ownership
    static void send_error(uint32_t id, const char* code, const char* detail = nullptr);
    static cJSON* reply(uint32_t id, bool ok);

    static constexpr const char* TAG = "UsbProtocol";
};

} // namespace Fuchey
