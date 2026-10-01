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
// hello also returns caps=[...] naming optional features.
//
// The app can never approve: signing always goes through
// WalletManager::sign_transaction() (parse → TFT → physical B1).
// ============================================================

#include "../wallet/WalletCore.hpp"
#include "../wallet_manager/WalletManager.hpp"
#include "../price/PriceService.hpp"
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
                PriceService& price, IsMainnetFn is_mainnet);

    // Returns true when `line` is a protocol frame (handled here, including
    // malformed frames); false for ordinary console commands.
    bool handle_line(const char* line);

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
    std::atomic<bool> m_busy{false};

    void cmd_hello(uint32_t id);
    void cmd_get_pubkey(uint32_t id);
    void cmd_sign_tx(uint32_t id, cJSON* req);
    void cmd_cancel(uint32_t id);
    void cmd_show_address(uint32_t id);

    static void sign_worker(void* arg);
    void run_sign(SignJob& job);

    // Output helpers (each writes exactly one frame).
    static void send(cJSON* obj);  // takes ownership
    static void send_error(uint32_t id, const char* code, const char* detail = nullptr);
    static cJSON* reply(uint32_t id, bool ok);

    static constexpr const char* TAG = "UsbProtocol";
};

} // namespace Fuchey
