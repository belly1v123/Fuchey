// ============================================================
// Fuchey — AIManager.cpp
// ============================================================

#include "AIManager.hpp"
#include "../config/Config.hpp"
#include "../storage/Storage.hpp"
#include "esp_log.h"
#include "cJSON.h"
#include <cstring>

namespace Fuchey {

static constexpr const char* TAG = "AIManager";

AIManager::AIManager(WiFiManager& wifi) : m_wifi(wifi) {}

bool AIManager::init() {
    Storage::Handle nvs(NVS::AI_NS, NVS_READONLY);
    if (nvs.is_open()) {
        auto key = nvs.get_str(NVS::KEY_LLM_API_KEY);
        auto ep  = nvs.get_str(NVS::KEY_LLM_ENDPOINT);
        if (key) m_api_key = *key;
        if (ep)  m_endpoint = *ep;
    }

    if (m_endpoint.empty()) {
        m_endpoint = API::LLM_DEFAULT_ENDPOINT;
    }

    ESP_LOGI(TAG, "AIManager initialized. Endpoint: %s", m_endpoint.c_str());
    return true;
}

bool AIManager::set_api_key(const char* api_key) {
    Storage::Handle nvs(NVS::AI_NS, NVS_READWRITE);
    if (!nvs.is_open()) return false;

    if (!nvs.set_str(NVS::KEY_LLM_API_KEY, api_key)) return false;
    if (!nvs.commit()) return false;

    m_api_key = api_key;
    return true;
}

bool AIManager::set_endpoint(const char* endpoint) {
    Storage::Handle nvs(NVS::AI_NS, NVS_READWRITE);
    if (!nvs.is_open()) return false;

    if (!nvs.set_str(NVS::KEY_LLM_ENDPOINT, endpoint)) return false;
    if (!nvs.commit()) return false;

    m_endpoint = endpoint;
    return true;
}

std::string AIManager::build_json_payload(const char* new_message) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", API::LLM_DEFAULT_MODEL);

    cJSON* messages = cJSON_AddArrayToObject(root, "messages");

    // System prompt defining AI boundaries
    cJSON* sys_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_msg, "role", "system");
    cJSON_AddStringToObject(sys_msg, "content",
        "You are Fuchey AI, a helpful desk assistant on a Solana hardware wallet. "
        "Keep responses brief (max 100 chars). If the user wants to make a micro-payment or transfer, "
        "output standard format: [PAYMENT:amount_cents].");
    cJSON_AddItemToArray(messages, sys_msg);

    // Append history
    for (const auto& msg : m_history) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "role", msg.role.c_str());
        cJSON_AddStringToObject(item, "content", msg.content.c_str());
        cJSON_AddItemToArray(messages, item);
    }

    // Append new message
    cJSON* user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", new_message);
    cJSON_AddItemToArray(messages, user_msg);

    char* json_out = cJSON_PrintUnformatted(root);
    std::string payload(json_out);
    cJSON_free(json_out);
    cJSON_Delete(root);

    return payload;
}

std::string AIManager::parse_llm_json_response(const std::string& json_str) {
    cJSON* root = cJSON_Parse(json_str.c_str());
    if (!root) return "Error parsing LLM response";

    std::string result = "No response";
    cJSON* choices = cJSON_GetObjectItem(root, "choices");
    if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
        cJSON* first = cJSON_GetArrayItem(choices, 0);
        cJSON* msg   = cJSON_GetObjectItem(first, "message");
        if (msg) {
            cJSON* content = cJSON_GetObjectItem(msg, "content");
            if (cJSON_IsString(content) && content->valuestring) {
                result = content->valuestring;
            }
        }
    }

    cJSON_Delete(root);
    return result;
}

bool AIManager::send_user_message(const char* message) {
    if (!m_wifi.has_ip()) {
        ESP_LOGW(TAG, "Cannot send AI message — No WiFi connection");
        return false;
    }

    std::string payload = build_json_payload(message);
    auto resp = m_wifi.post_json(m_endpoint.c_str(), payload.c_str(), m_api_key.c_str());

    if (!resp.success) {
        ESP_LOGE(TAG, "LLM HTTP POST failed code: %d", resp.status_code);
        return false;
    }

    std::string reply = parse_llm_json_response(resp.body);

    // Save conversation history
    m_history.push_back({"user", message});
    m_history.push_back({"assistant", reply});
    if (m_history.size() > 6) { // Keep last 3 turns
        m_history.erase(m_history.begin(), m_history.begin() + 2);
    }

    // Check for payment intents in response
    parse_response_intent(reply);

    // Post to UI queue
    Events::Event evt{};
    evt.type = Events::EventType::AI_RESPONSE_READY;
    std::strncpy(evt.data.chat.text, reply.c_str(), sizeof(evt.data.chat.text) - 1);
    Events::post(Events::g_ui_queue, evt);

    return true;
}

void AIManager::parse_response_intent(const std::string& response_text) {
    size_t pos = response_text.find("[PAYMENT:");
    if (pos != std::string::npos) {
        size_t end_pos = response_text.find("]", pos);
        if (end_pos != std::string::npos) {
            std::string amount_str = response_text.substr(pos + 9, end_pos - (pos + 9));
            uint64_t amount_cents = std::stoull(amount_str);

            ESP_LOGI(TAG, "Detected AI payment intent: %llu cents", amount_cents);

            // Post request to WalletManager (never sign directly)
            Events::Event evt{};
            evt.type = Events::EventType::TX_REQUEST;
            evt.data.tx.amount_cents = amount_cents;
            evt.data.tx.tx_len = 0; // Empty mock tx for intent demonstration
            Events::post(Events::g_wallet_queue, evt);
        }
    }
}

void AIManager::task_entry(void* arg) {
    static_cast<AIManager*>(arg)->run();
}

void AIManager::run() {
    ESP_LOGI(TAG, "AIManager task running");
    Events::Event evt{};

    while (true) {
        if (Events::g_ai_queue &&
            xQueueReceive(Events::g_ai_queue, &evt, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (evt.type == Events::EventType::AI_MESSAGE_RECV) {
                send_user_message(evt.data.chat.text);
            }
        }
    }
}

} // namespace Fuchey
