# Fuchey

**Fuchey** is an open-source, self-custodial **DIY Solana hardware wallet** built on the **ESP32-S3** with an integrated **text-based AI assistant**. Inspired by the idea that **"a wallet should not look like a wallet,"** Fuchey is designed to be an everyday desk device that displays useful information such as the time, weather, and live SOL price, while seamlessly transforming into a secure hardware wallet experience whenever blockchain interactions are required.

The primary goal of Fuchey is to make **self-custody secure, intuitive, and enjoyable**. All private keys are generated and stored on-device, transactions are signed locally, and sensitive operations require physical user confirmation. The integrated AI assistant helps users understand crypto, build transactions, and perform autonomous micropayments within user-defined spending limits, while remaining completely isolated from the wallet's cryptographic core.

Built with **ESP-IDF**, **PlatformIO**, and modern **C++**, Fuchey follows a modular, security-first architecture focused on reliability, maintainability, and an exceptional user experience.

## Tech Stack

- **MCU:** ESP32-S3-N16R8 — 16 MB flash, 8 MB Octal PSRAM (OPI, 80 MHz), 240 MHz CPU
- **Board target:** `esp32-s3-devkitc-1`
- **Framework:** ESP-IDF **v6.0.1** (resolved via the IDF Component Manager)
- **Build system:** PlatformIO (`platform = espressif32`, `framework = espidf`)
- **Language:** C++ (RTOS: FreeRTOS, bundled with ESP-IDF)
- **Display:** Custom ST7789 SPI TFT driver (`St7789.hpp`, `driver/spi_master.h`) — no third-party TFT/GFX library
- **QR codes:** Self-contained `qrcodegen` implementation for on-device receive-address QR
- **Crypto:** `esphome/libsodium ^1.10021.1` (keypair generation, signing, encryption)
- **JSON:** `espressif/cjson ^1.7.17` (IDF-managed component, locked to 1.7.19~2)
- **Storage:** NVS, encrypted private key at rest
- **Connectivity:** WiFi (STA), HTTPS via ESP-IDF HTTP client + mbedTLS (TLS 1.2/1.3)
- **License:** Intended fully open source — *no `LICENSE` file is currently present in the repo*

## Current Stage

- ✅ Create wallet (BIP39 12-word)
- ✅ Import wallet (mnemonic / hex / base58 private key)
- ✅ Store private key encrypted in NVS
- ✅ Send SOL and USDC with hardware button confirmation
- ✅ Display receive address as QR code on OLED
- ✅ On-demand SOL + USDC balance fetch via RPC
- ✅ Idle display cycle: clock → weather → SOL price → message
- ✅ AI Assistant chat via serial console
- ✅ Auto-connect WiFi (saved in NVS), switch between devnet/mainnet

## Hardware Wiring

| Component   | GPIO |
|-------------|------|
| OLED SCL    | 9    |
| OLED SDA    | 8    |
| TX button   | 4    |
| Menu button | 5    |
| Select      | 6    |
| Back        | 7    |

- GPIO4 (TX): single press = accept transaction · double press or hold = reject
- GPIO5 (Menu): single press = open menu / next option · double press = show wallet QR
- GPIO6 (Select): confirm the highlighted menu option
- GPIO7 (Back): go back

## Quick Start

```console
# Flash firmware
pio run -t upload

# Open serial monitor (115200 baud)

# Configure WiFi
w <SSID> <PASSWORD>

# Create wallet
wallet_create

# Show menu on OLED
menu

# Send SOL
send sol <amount> <recipient_address>

# Send USDC
send usdc <amount> <recipient_address>

# Fetch balance
balance

# Switch network
network devnet
network mainnet

# AI Assistant
ai_key <your_openai_key>
ai <message>

# Help
h
```

## Core Features

-  Self-custodial Solana hardware wallet
-  On-device transaction signing
-  Integrated text-based AI assistant
-  Clock, weather, and live SOL price display
-  ESP32-S3 powered with OLED interface
-  Security-first modular architecture
-  Fully open source

## Security Tooling

The `scripts/` folder contains developer tooling that guards against leaking secret material to the serial console:

- `scripts/check-no-secret-logging.ps1` — scans the firmware source and fails if any log call could dump private-key / seed / chain-code bytes (e.g. `ESP_LOG_BUFFER_HEX`).
  ```
  pwsh -NoProfile -File scripts/check-no-secret-logging.ps1
  ```
- `scripts/install-hooks.ps1` — installs a git `pre-commit` hook that runs the check above automatically on every commit.
  ```
  pwsh -NoProfile -File scripts/install-hooks.ps1
  ```

## Designed by Pranjal Kharel
## https://pranjalkharel.com.np