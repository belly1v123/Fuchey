# Fuchey

Solana hardware wallet + AI desk companion on ESP32-S3 (16MB Flash, 8MB PSRAM).

## Features

- **Wallet**: Create (BIP39 12-word), import (mnemonic/hex/base58 private key), stored encrypted in NVS
- **Transactions**: Send SOL and USDC with hardware button confirmation
- **QR Display**: Receive address as QR code on 128×64 OLED
- **Balance**: On-demand SOL + USDC balance fetch via RPC (menu → View Balance)
- **Display**: Idle cycle between clock, weather, SOL price, and message
- **AI Assistant**: Chat with LLM via serial console
- **Network**: Auto-connect WiFi (saved in NVS), switch between devnet/mainnet

## Hardware Wiring

| Component | GPIO |
|-----------|------|
| OLED SCL  | 9    |
| OLED SDA  | 8    |
| Button    | 4    |

- CONFIRM = single press (CONFIRM/SELECT)
- BACK = double press (NEXT in menu)

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

# Switch to devnet/mainnet
network devnet
network mainnet

# AI Assistant
ai_key <your_openai_key>
ai <message>

# Help
h
```
