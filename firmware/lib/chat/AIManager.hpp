#pragma once
// ============================================================
// Fuchey — AIManager.hpp
// Untrusted AI Manager for handling text-based LLM chat and intent parsing.
// Communicates with cloud LLM endpoint via WiFiManager.
// Can build transactions or request signatures via WalletManager events,
// but NEVER has access to private keys or direct signing.
// ============================================================

#include "../wifi/WiFiManager.hpp"
#include "../events/Events.hpp"
#include <string>
#include <vector>
#include <cstdint>

namespace Fuchey {

struct ChatMessage {
    std::string role;    // "user" or "assistant" or "system"
    std::string content;
};

class AIManager {
public:
    explicit AIManager(WiFiManager& wifi);
    ~AIManager() = default;

    // Non-copyable
    AIManager(const AIManager&) = delete;
    AIManager& operator=(const AIManager&) = delete;

    bool init();

    // Configure API credentials
    bool set_api_key(const char* api_key);
    bool set_endpoint(const char* endpoint);

    // Send prompt to cloud LLM
    bool send_user_message(const char* message);

    // Process AI response and check for payment/transaction intents
    void parse_response_intent(const std::string& response_text);

    // FreeRTOS task entry
    static void task_entry(void* arg);
    void run();

private:
    WiFiManager& m_wifi;
    std::string  m_api_key;
    std::string  m_endpoint;
    std::vector<ChatMessage> m_history;

    std::string build_json_payload(const char* new_message);
    std::string parse_llm_json_response(const std::string& json_str);

    static constexpr const char* TAG = "AIManager";
};

} // namespace Fuchey
