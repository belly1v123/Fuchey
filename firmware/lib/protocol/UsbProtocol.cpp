// ============================================================
// Fuchey — UsbProtocol.cpp
// ============================================================

#include "UsbProtocol.hpp"
#include "../config/Config.hpp"
#include "../events/Events.hpp"
#include "../crypto/Base58.hpp"
#include "../crypto/Base64.hpp"
#include "../wallet_manager/TxParser.hpp"
#include "../crypto/BIP39.hpp"
#include "esp_random.h"
#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>

namespace Fuchey {

static constexpr const char* TAG = "UsbProtocol";
static constexpr uint32_t SIGN_WORKER_STACK = 20480;  // TLS price fetch + signing
static constexpr int      SIGN_WORKER_PRIO  = 4;

UsbProtocol::UsbProtocol(WalletCore& core, WalletManager& manager,
                         PriceService& price, IsMainnetFn is_mainnet,
                         DeviceSettings* settings)
    : m_core(core), m_manager(manager), m_price(price), m_is_mainnet(is_mainnet),
      m_settings(settings),
      m_recovery([](int i) { return Crypto::BIP39::get_word(i); },
                 Crypto::BIP39::WORDLIST_SIZE, [] { return esp_random(); }) {}

// ─── CRC32 (IEEE 802.3 / zlib) ────────────────────────────
uint32_t UsbProtocol::crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

// ─── Output ───────────────────────────────────────────────
cJSON* UsbProtocol::reply(uint32_t id, bool ok) {
    cJSON* obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "id", id);
    cJSON_AddBoolToObject(obj, "ok", ok);
    return obj;
}

void UsbProtocol::send(cJSON* obj) {
    if (!obj) return;
    char* json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!json) return;
    uint32_t crc = crc32(reinterpret_cast<const uint8_t*>(json), strlen(json));
    // One printf call → one locked write, so log lines cannot split a frame.
    printf("%s%s*%08lX\n", PREFIX, json, static_cast<unsigned long>(crc));
    fflush(stdout);
    // fflush only hands bytes to the console driver; the USB CDC console keeps
    // a partial packet until fsync, so without this a reply can sit unsent
    // until the next log line (seen: ~30 s, longer than a blockhash lives).
    fsync(fileno(stdout));
    cJSON_free(json);
}

void UsbProtocol::send_error(uint32_t id, const char* code, const char* detail) {
    cJSON* obj = reply(id, false);
    cJSON_AddStringToObject(obj, "err", code);
    if (detail) cJSON_AddStringToObject(obj, "detail", detail);
    send(obj);
}

// ─── Input ────────────────────────────────────────────────
bool UsbProtocol::handle_line(const char* line) {
    if (!line || strncmp(line, PREFIX, 2) != 0) return false;

    const char* json = line + 2;
    const char* star = strrchr(json, '*');
    if (!star || strlen(star + 1) != 8) {
        send_error(0, "bad_frame", "missing *CRC32");
        return true;
    }
    char* end = nullptr;
    unsigned long want = strtoul(star + 1, &end, 16);
    if (!end || *end != '\0') {
        send_error(0, "bad_frame", "bad CRC hex");
        return true;
    }
    const size_t json_len = static_cast<size_t>(star - json);
    if (crc32(reinterpret_cast<const uint8_t*>(json), json_len) != static_cast<uint32_t>(want)) {
        send_error(0, "bad_crc");
        return true;
    }

    cJSON* req = cJSON_ParseWithLength(json, json_len);
    if (!req) {
        send_error(0, "bad_json");
        return true;
    }

    cJSON* id_item  = cJSON_GetObjectItem(req, "id");
    cJSON* cmd_item = cJSON_GetObjectItem(req, "cmd");
    const uint32_t id = (id_item && cJSON_IsNumber(id_item) && id_item->valuedouble >= 0)
                        ? static_cast<uint32_t>(id_item->valuedouble) : 0;
    const char* cmd = (cmd_item && cJSON_IsString(cmd_item)) ? cmd_item->valuestring : nullptr;

    if (!cmd) {
        send_error(id, "bad_request", "missing cmd");
    } else if (strcmp(cmd, "hello") == 0) {
        cmd_hello(id);
    } else if (strcmp(cmd, "get_pubkey") == 0) {
        cmd_get_pubkey(id);
    } else if (strcmp(cmd, "sign_tx") == 0) {
        cmd_sign_tx(id, req);
    } else if (strcmp(cmd, "cancel") == 0) {
        cmd_cancel(id);
    } else if (strcmp(cmd, "show_address") == 0) {
        cmd_show_address(id);
    } else if (m_settings && strcmp(cmd, "get_status") == 0) {
        cmd_get_status(id);
    } else if (m_settings && strcmp(cmd, "wifi_scan") == 0) {
        cmd_wifi_scan(id);
    } else if (m_settings && strcmp(cmd, "set_wifi") == 0) {
        cmd_set_wifi(id, req);
    } else if (m_settings && strcmp(cmd, "set_location") == 0) {
        cmd_set_location(id, req);
    } else if (m_settings && strcmp(cmd, "recovery_start") == 0) {
        cmd_recovery_start(id, req);
    } else if (m_settings && strcmp(cmd, "recovery_tap") == 0) {
        cmd_recovery_tap(id, req);
    } else if (m_settings && strcmp(cmd, "recovery_cancel") == 0) {
        cmd_recovery_cancel(id);
    } else {
        send_error(id, "unknown_cmd", cmd);
    }

    cJSON_Delete(req);
    return true;
}

// ─── Commands ─────────────────────────────────────────────
void UsbProtocol::cmd_hello(uint32_t id) {
    cJSON* obj = reply(id, true);
    cJSON_AddNumberToObject(obj, "proto", VERSION);
    cJSON_AddStringToObject(obj, "fw", FW_VERSION);
    cJSON_AddStringToObject(obj, "network", m_is_mainnet() ? "mainnet" : "devnet");
    cJSON_AddNumberToObject(obj, "max_tx", MAX_TX_BYTES);
    auto addr = m_core.get_address();
    cJSON_AddBoolToObject(obj, "has_wallet", addr.has_value());
    if (addr) cJSON_AddStringToObject(obj, "pubkey", addr->c_str());
    cJSON_AddBoolToObject(obj, "busy", m_busy.load());
    // Optional features, so the app can adapt to older firmware.
    cJSON* caps = cJSON_AddArrayToObject(obj, "caps");
    cJSON_AddItemToArray(caps, cJSON_CreateString("show_address"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("usdc_create_ata"));
    if (m_settings) {
        cJSON_AddItemToArray(caps, cJSON_CreateString("settings_v1"));
        cJSON_AddItemToArray(caps, cJSON_CreateString("recovery_grid"));
    }
    send(obj);
}

void UsbProtocol::cmd_get_pubkey(uint32_t id) {
    auto addr = m_core.get_address();
    if (!addr) {
        send_error(id, "no_wallet");
        return;
    }
    cJSON* obj = reply(id, true);
    cJSON_AddStringToObject(obj, "pubkey", addr->c_str());
    send(obj);
}

void UsbProtocol::cmd_show_address(uint32_t id) {
    if (!m_core.get_address()) {
        send_error(id, "no_wallet");
        return;
    }
    if (m_busy.load()) {
        send_error(id, "busy", "a signature request is pending");
        return;
    }
    Events::Event evt{};
    evt.type = Events::EventType::UI_SHOW_ADDRESS;
    if (!Events::post(Events::g_ui_queue, evt, pdMS_TO_TICKS(50))) {
        send_error(id, "busy", "UI queue full");
        return;
    }
    send(reply(id, true));
}

// ─── Device settings (tier "settings": never keys, never signing) ───
namespace {

// Printable ASCII only: it is drawn with the device's ASCII fonts.
bool is_printable_ascii(const char* s, size_t max_len) {
    size_t n = 0;
    for (; s[n]; ++n) {
        if (n >= max_len) return false;
        const unsigned char c = static_cast<unsigned char>(s[n]);
        if (c < 0x20 || c > 0x7E) return false;
    }
    return n > 0;
}

const char* str_field(cJSON* req, const char* name) {
    cJSON* it = cJSON_GetObjectItem(req, name);
    return (it && cJSON_IsString(it)) ? it->valuestring : nullptr;
}

} // namespace

void UsbProtocol::cmd_get_status(uint32_t id) {
    const DeviceStatus st = m_settings->status();
    cJSON* obj = reply(id, true);
    cJSON* wifi = cJSON_AddObjectToObject(obj, "wifi");
    cJSON_AddBoolToObject(wifi, "configured", st.wifi_configured);
    cJSON_AddBoolToObject(wifi, "connected", st.wifi_connected);
    cJSON_AddBoolToObject(wifi, "online", st.online);
    cJSON_AddStringToObject(wifi, "ssid", st.wifi_ssid.c_str());
    cJSON* loc = cJSON_AddObjectToObject(obj, "location");
    cJSON_AddBoolToObject(loc, "configured", st.location_configured);
    cJSON_AddStringToObject(loc, "city", st.city.c_str());
    cJSON_AddNumberToObject(loc, "lat", st.lat);
    cJSON_AddNumberToObject(loc, "lon", st.lon);
    cJSON_AddBoolToObject(obj, "setup_done", st.setup_done);
    cJSON_AddStringToObject(obj, "network", m_is_mainnet() ? "mainnet" : "devnet");
    send(obj);
}

void UsbProtocol::cmd_wifi_scan(uint32_t id) {
    std::vector<WifiNetwork> nets;
    if (!m_settings->scan_wifi(nets)) {
        send_error(id, "scan_failed", "WiFi radio busy — try again in a few seconds");
        return;
    }
    cJSON* obj = reply(id, true);
    cJSON* arr = cJSON_AddArrayToObject(obj, "networks");
    for (const auto& n : nets) {
        cJSON* e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "ssid", n.ssid.c_str());
        cJSON_AddNumberToObject(e, "rssi", n.rssi);
        cJSON_AddBoolToObject(e, "secure", n.secure);
        cJSON_AddItemToArray(arr, e);
    }
    send(obj);
}

void UsbProtocol::cmd_set_wifi(uint32_t id, cJSON* req) {
    if (m_busy.load()) {
        send_error(id, "busy", "a signature request is pending");
        return;
    }
    const char* ssid = str_field(req, "ssid");
    const char* pass = str_field(req, "password");
    const size_t sl = ssid ? strlen(ssid) : 0;
    const size_t pl = pass ? strlen(pass) : 0;
    // 802.11: SSID 1..32 bytes; WPA2 passphrase 8..63 (the device requires WPA2).
    if (sl < 1 || sl > 32 || !pass || pl < 8 || pl > 63) {
        send_error(id, "bad_request", "ssid 1-32 bytes, password 8-63 characters");
        return;
    }
    if (!m_settings->set_wifi(ssid, pass)) {
        send_error(id, "failed", "could not save or start the connection");
        return;
    }
    send(reply(id, true));   // connection result: poll get_status
}

void UsbProtocol::cmd_set_location(uint32_t id, cJSON* req) {
    if (m_busy.load()) {
        send_error(id, "busy", "a signature request is pending");
        return;
    }
    const char* city = str_field(req, "city");
    cJSON* lat = cJSON_GetObjectItem(req, "lat");
    cJSON* lon = cJSON_GetObjectItem(req, "lon");
    if (!city || !is_printable_ascii(city, 31) || !cJSON_IsNumber(lat) || !cJSON_IsNumber(lon) ||
        lat->valuedouble < -90 || lat->valuedouble > 90 ||
        lon->valuedouble < -180 || lon->valuedouble > 180) {
        send_error(id, "bad_request", "city 1-31 printable ASCII, lat -90..90, lon -180..180");
        return;
    }
    if (!m_settings->set_location(city, static_cast<float>(lat->valuedouble),
                                  static_cast<float>(lon->valuedouble))) {
        send_error(id, "failed", "could not save the location");
        return;
    }
    send(reply(id, true));
}

// ─── Scrambled-grid recovery ──────────────────────────────
// The host only ever sends a cell position; the letters/words are drawn
// on the TFT. Replies carry progress (word n of N, mode) and the result.
void UsbProtocol::recovery_post_view(Events::RecoveryResult result) {
    Events::Event evt{};
    evt.type = Events::EventType::UI_RECOVERY_VIEW;
    auto& v = evt.data.recovery;
    v.active     = m_recovery.active() && result == Events::RecoveryResult::NONE;
    v.restore    = m_recovery_restore;
    v.words_mode = m_recovery.mode() == RecoverySession::Mode::WORDS;
    v.word       = static_cast<uint8_t>(m_recovery.word_number());
    v.total      = static_cast<uint8_t>(m_recovery.total());
    v.result     = result;
    snprintf(v.typed, sizeof(v.typed), "%s", m_recovery.typed_groups().c_str());
    for (int i = 0; i < RecoverySession::CELLS; ++i) {
        snprintf(v.cells[i], sizeof(v.cells[i]), "%s", m_recovery.cell_label(i).c_str());
    }
    Events::post(Events::g_ui_queue, evt, pdMS_TO_TICKS(50));
}

void UsbProtocol::recovery_reply_progress(uint32_t id) {
    cJSON* obj = reply(id, true);
    cJSON_AddBoolToObject(obj, "done", false);
    cJSON_AddNumberToObject(obj, "word", m_recovery.word_number());
    cJSON_AddNumberToObject(obj, "total", m_recovery.total());
    cJSON_AddStringToObject(obj, "mode",
        m_recovery.mode() == RecoverySession::Mode::WORDS ? "words" : "letters");
    send(obj);
}

void UsbProtocol::recovery_end() {
    m_recovery.wipe();
    m_recovery_active = false;
    m_busy.store(false);
}

void UsbProtocol::cmd_recovery_start(uint32_t id, cJSON* req) {
    const char* purpose = str_field(req, "purpose");
    cJSON* words = cJSON_GetObjectItem(req, "words");
    const bool restore = purpose && strcmp(purpose, "restore") == 0;
    const bool check   = purpose && strcmp(purpose, "check") == 0;
    const int  n = (words && cJSON_IsNumber(words)) ? words->valueint : 12;
    if ((!restore && !check) || (n != 12 && n != 24)) {
        send_error(id, "bad_request", "purpose check|restore, words 12|24");
        return;
    }
    // Restore never overwrites a wallet; check needs one to compare with.
    const bool has_wallet = m_core.get_address().has_value();
    if (restore && has_wallet) { send_error(id, "wallet_exists", "erase it on the device first"); return; }
    if (check && !has_wallet)  { send_error(id, "no_wallet"); return; }

    bool expected = false;
    if (!m_recovery_active && !m_busy.compare_exchange_strong(expected, true)) {
        send_error(id, "busy", "a signature request is pending");
        return;
    }
    Events::g_recovery_abort.store(false);
    m_recovery_restore = restore;
    m_recovery_active  = true;
    m_recovery.start(n);
    recovery_post_view(Events::RecoveryResult::NONE);
    recovery_reply_progress(id);
}

void UsbProtocol::cmd_recovery_tap(uint32_t id, cJSON* req) {
    if (!m_recovery_active) { send_error(id, "no_session"); return; }
    if (Events::g_recovery_abort.exchange(false)) {
        recovery_end();
        send_error(id, "cancelled", "cancelled on the device");
        return;
    }
    cJSON* pos = cJSON_GetObjectItem(req, "pos");
    if (!pos || !cJSON_IsNumber(pos) || pos->valueint < 0 || pos->valueint > 8) {
        send_error(id, "bad_request", "pos 0..8");
        return;
    }
    m_recovery.tap(pos->valueint);           // an empty cell is simply ignored
    if (!m_recovery.complete()) {
        recovery_post_view(Events::RecoveryResult::NONE);
        recovery_reply_progress(id);
        return;
    }

    // All words entered: validate, then compare or restore.
    std::string phrase = m_recovery.phrase();
    Events::RecoveryResult result = Events::RecoveryResult::FAILED;
    std::string address;
    if (!Crypto::BIP39::validate(phrase)) {
        result = Events::RecoveryResult::BAD_CHECKSUM;
    } else if (m_recovery_restore) {
        if (m_core.import(phrase) == WalletResult::OK) {
            result = Events::RecoveryResult::RESTORED;
            address = m_core.get_address().value_or("");
        }
    } else {
        bool match = false;
        if (m_core.matches_mnemonic(phrase, match) == WalletResult::OK) {
            result = match ? Events::RecoveryResult::MATCH : Events::RecoveryResult::MISMATCH;
        }
    }
    std::fill(phrase.begin(), phrase.end(), '\0');
    phrase.clear();
    recovery_post_view(result);
    recovery_end();
    if (result == Events::RecoveryResult::RESTORED && m_settings) {
        m_settings->on_wallet_restored(address);
    }

    static constexpr const char* NAMES[] = {
        "none", "match", "mismatch", "bad_checksum", "restored", "failed", "cancelled"};
    cJSON* obj = reply(id, true);
    cJSON_AddBoolToObject(obj, "done", true);
    cJSON_AddStringToObject(obj, "result", NAMES[static_cast<int>(result)]);
    if (!address.empty()) cJSON_AddStringToObject(obj, "address", address.c_str());
    send(obj);
}

void UsbProtocol::cmd_recovery_cancel(uint32_t id) {
    if (m_recovery_active) {
        recovery_post_view(Events::RecoveryResult::CANCELLED);
        recovery_end();
    }
    send(reply(id, true));
}

void UsbProtocol::cmd_cancel(uint32_t id) {
    cJSON* obj = reply(id, true);
    cJSON_AddBoolToObject(obj, "cancelled", m_manager.cancel_pending());
    send(obj);
}

void UsbProtocol::cmd_sign_tx(uint32_t id, cJSON* req) {
    cJSON* msg_item = cJSON_GetObjectItem(req, "msg");
    cJSON* net_item = cJSON_GetObjectItem(req, "network");
    if (!msg_item || !cJSON_IsString(msg_item) || !net_item || !cJSON_IsString(net_item)) {
        send_error(id, "bad_request", "need msg (base64) and network");
        return;
    }

    // The app must be talking about the same cluster the device shows.
    const bool want_mainnet = strcmp(net_item->valuestring, "mainnet") == 0;
    if ((!want_mainnet && strcmp(net_item->valuestring, "devnet") != 0) ||
        want_mainnet != m_is_mainnet()) {
        send_error(id, "network_mismatch", m_is_mainnet() ? "device is on mainnet"
                                                          : "device is on devnet");
        return;
    }

    auto* job = new SignJob{this, id, {}};
    if (!Crypto::Base64::decode(msg_item->valuestring, job->message) ||
        job->message.empty() || job->message.size() > MAX_TX_BYTES) {
        delete job;
        send_error(id, "bad_request", "msg must be base64, 1..1232 bytes");
        return;
    }

    bool expected = false;
    if (!m_busy.compare_exchange_strong(expected, true)) {
        delete job;
        send_error(id, "busy", "a signature request is already pending");
        return;
    }

    if (xTaskCreate(sign_worker, "usb_sign_task", SIGN_WORKER_STACK, job,
                    SIGN_WORKER_PRIO, nullptr) != pdPASS) {
        m_busy.store(false);
        delete job;
        send_error(id, "sign_failed", "out of memory");
    }
}

// ─── Signing worker ───────────────────────────────────────
void UsbProtocol::sign_worker(void* arg) {
    auto* job = static_cast<SignJob*>(arg);
    UsbProtocol* self = job->self;
    self->run_sign(*job);
    delete job;
    self->m_busy.store(false);
    vTaskDelete(nullptr);
}

void UsbProtocol::run_sign(SignJob& job) {
    auto pubkey = m_core.get_pubkey();
    if (!pubkey) {
        send_error(job.id, "no_wallet");
        return;
    }

    // Pre-parse so the app gets a precise error immediately and can mirror
    // the device screen. WalletManager parses again before signing.
    TxParser::ParsedTransfer parsed{};
    auto perr = TxParser::parse_transfer(job.message, *pubkey, parsed);
    if (perr != TxParser::ParseError::OK) {
        send_error(job.id, "unsupported_tx", TxParser::error_to_string(perr));
        return;
    }

    // Fresh SOL/USD rate for the on-device USD figures (display only).
    const bool price_live = m_price.update_now();
    SignContext ctx{};
    ctx.mainnet    = m_is_mainnet();
    ctx.sol_usd    = m_price.has_data() ? m_price.get_sol_usd() : 0.0f;
    ctx.price_live = price_live;

    {
        char amount[24], fee[16];
        TxParser::format_units(parsed.amount, parsed.decimals, amount, sizeof(amount));
        TxParser::format_units(parsed.fee_lamports, 9, fee, sizeof(fee));
        const Crypto::PubKey& shown = parsed.creates_token_account ? parsed.owner
                                                                   : parsed.destination;
        std::string to = Crypto::Base58::pubkey_to_address(
            std::span<const uint8_t, 32>(shown.data(), 32));

        cJSON* evt = cJSON_CreateObject();
        cJSON_AddNumberToObject(evt, "id", job.id);
        cJSON_AddStringToObject(evt, "event", "awaiting_confirmation");
        cJSON_AddStringToObject(evt, "asset", parsed.asset == TxParser::Asset::SOL ? "SOL" : "USDC");
        cJSON_AddStringToObject(evt, "amount", amount);
        cJSON_AddStringToObject(evt, "fee", fee);
        cJSON_AddStringToObject(evt, "to", to.c_str());
        cJSON_AddNumberToObject(evt, "timeout_ms", WalletManager::CONFIRM_TIMEOUT_MS);
        if (parsed.creates_token_account) {
            char rent[16];
            TxParser::format_units(parsed.rent_lamports, 9, rent, sizeof(rent));
            cJSON_AddBoolToObject(evt, "creates_account", true);
            cJSON_AddStringToObject(evt, "rent", rent);
        }
        send(evt);
    }

    ESP_LOGI(TAG, "sign_tx #%lu: waiting for B1 on the device", static_cast<unsigned long>(job.id));
    SignResult res = m_manager.sign_transaction(job.message, ctx);

    if (res.status == SignStatus::SIGNED) {
        std::string sig = Crypto::Base64::encode(
            std::span<const uint8_t>(res.signature.data(), res.signature.size()));
        cJSON* obj = reply(job.id, true);
        cJSON_AddStringToObject(obj, "sig", sig.c_str());
        send(obj);
        return;
    }

    const char* code = "sign_failed";
    switch (res.status) {
        case SignStatus::REJECTED:         code = "rejected";         break;
        case SignStatus::CANCELLED:        code = "cancelled";        break;
        case SignStatus::TIMED_OUT:        code = "timeout";          break;
        case SignStatus::UNSUPPORTED_TX:   code = "unsupported_tx";   break;
        case SignStatus::NETWORK_MISMATCH: code = "network_mismatch"; break;
        case SignStatus::BUSY:             code = "busy";             break;
        case SignStatus::NO_WALLET:        code = "no_wallet";        break;
        default:                                                      break;
    }
    send_error(job.id, code, res.status == SignStatus::UNSUPPORTED_TX
                                 ? TxParser::error_to_string(res.parse_error)
                                 : WalletManager::status_to_string(res.status));
}

} // namespace Fuchey
