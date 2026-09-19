# Fuchey

**Fuchey** is an open-source, self-custodial **Solana crypto desk pet** built on the **ESP32-S3** inspired by the idea of bringing the personality and interaction of a Tamagotchi into the world of crypto.

It is a physical desk companion with its own display, buttons, and behavior engine. It lives beside your PC or laptop, reacts to events, displays useful information, and develops simple moods and behaviors based on what is happening around it.

The core experience is built around a **character-driven behavior engine.** 
The goal is to make crypto feel more tangible and alive rather than presenting everything through conventional dashboards and wallet interfaces.

The device uses a 240×240 ST7789 SPI TFT display to bring the character and pixel-art interface to life. Alongside the character, Fuchey can display information such as the time, weather, SOL price, 24-hour market change, wallet information, and other contextual data.

Crypto functionality remains an important part of the ecosystem, but it is treated as an interaction and event layer rather than Fuchey's entire identity. Wallet operations, blockchain activity, and companion software can communicate with the physical Fuchey and trigger behaviors or provide additional functionality. The project is built with ESP-IDF, PlatformIO, and modern C++, with a focus on modular firmware, responsive hardware interaction, pixel-art animation, and an extensible behavior engine.

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


## Current Stage

- ✅ Create wallet (BIP39 12-word)
- ✅ Import wallet (mnemonic / hex / base58 private key)
- ✅ Store private key encrypted in NVS
- ✅ Send SOL and USDC with hardware button confirmation
- ✅ Display receive address as QR code on TFT
- ✅ On-demand SOL + USDC balance fetch via RPC
- ✅ Idle display cycle: clock → weather → SOL price → message
- ✅ Auto-connect WiFi (saved in NVS), switch between devnet/mainnet

## Hardware Wiring (TFT_version — ST7789 240x240 SPI)

| Component   | GPIO | Notes |
|-------------|------|-------|
| TFT SCLK    | 9    | SPI2_HOST, 8 MHz |
| TFT MOSI    | 8    | SPI2_HOST, 8 MHz |
| TFT CS      | 5    | plain I/O |
| TFT DC      | 16   | plain I/O, no conflict |
| TFT RST     | 6    | plain I/O |
| TFT BL      | —    | tied to 3V3 on module, firmware does not drive it (`PIN_BL = -1`) |
| B1 TX/Back  | 4    | TX confirm only during TX_CONFIRM (1x=accept, 2x/hold=reject); elsewhere hierarchical Back |
| B2 Menu/Sel | 10   | open menu / select highlighted icon (Wallet hub → Balance/QR via B2) |
| B3 Prev     | 17   | previous icon (menu carousel + Wallet hub tab) |
| B4 Next     | 13   | next icon (menu carousel + Wallet hub tab) |
| Buzzer      | 40   | active 5V via NPN low-side, parked LOW at boot (silent) |
| RGB LED     | 48   | onboard WS2812 (DevKitC-1) |

- GPIO4 (B1): in TX_CONFIRM single press = accept · double press or hold = reject; elsewhere Back (QR/Balance → Wallet hub → Menu → Home, one level per press)
- GPIO10 (B2): open menu / select focused icon (Wallet hub → Balance/QR via B2)
- GPIO17 (B3): previous icon in menu carousel / Balance-vs-QR focus in Wallet hub (ignored elsewhere — B1 back first)
- GPIO13 (B4): next icon in menu carousel / Balance-vs-QR focus in Wallet hub (ignored elsewhere — B1 back first)
- Menu is an icon carousel (Wallet Info · Pomodoro · Badge → pass); View Balance + QR + SOL Price live under Wallet Info
- Idle cycle is Home ↔ Weather only (SOL price moved into Wallet Info)

## Quick Start

```console
# Flash firmware
pio run -t upload

# Open serial monitor (115200 baud)

# Configure WiFi
w <SSID> <PASSWORD>

# Create wallet
wallet_create

# Show menu on TFT
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

# Help
h
```

## Core Features

-  Self-custodial Solana hardware wallet
-  On-device transaction signing
-  Clock, weather, and live SOL price display
-  ESP32-S3 powered with TFT interface
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