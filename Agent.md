# Development Environment

## Operating System

Primary development environment is:

- Windows 11
- WSL2 (Ubuntu)

All development should assume Linux (WSL) unless explicitly stated otherwise.

The project source code is located inside the Linux filesystem.

Example:

~/Projects/Fuchey

Do not assume development inside /mnt/c/.

---

## Toolchain

Language:
- Modern C++

Framework:
- ESP-IDF

Build System:
- PlatformIO

IDE:
- Visual Studio Code

Target MCU:
- ESP32-S3-N16R8

Compiler:
- GCC (ESP-IDF Toolchain)

Use ESP-IDF APIs.

Do not use Arduino framework unless explicitly requested.

---

## Installed Tools

Assume the following tools are available if not install them:

- Git
- PlatformIO Core
- Python
- CMake
- Ninja
- Rust
- Cargo
- Solana CLI
- Node.js (LTS)
- npm
- OpenSSL
- jq

Prefer using these tools instead of suggesting alternatives.

---

## Development Workflow

Always assume the following workflow:

Design

↓

Architecture Review

↓

Implementation

↓

Build

↓

Flash

↓

Test

↓

Review

↓

Commit

Do not skip architecture discussions for major features.

---

## Code Generation Rules

Before generating code:

1. Understand the problem.
2. Explain the architecture.
3. Explain trade-offs.
4. Identify security implications.
5. Recommend improvements.
6. Generate production-quality code.

Never immediately dump code for complex requests.

---

## Coding Style

Use:

- Modern C++
- RAII
- constexpr where appropriate
- enum class
- std::optional
- std::array
- std::span when available

Avoid:

- Macros
- Global mutable variables
- Blocking delays
- Dynamic allocation inside loops
- God classes

Prefer:

- Composition
- Small classes
- Single responsibility
- Event-driven design

---

## ESP-IDF Guidelines

Prefer:

- FreeRTOS Tasks
- Event Groups
- Queues
- Timers
- NVS
- mbedTLS
- esp_http_client
- esp_wifi
- esp_event

Avoid polling whenever possible.

Prefer asynchronous/event-driven architecture.

---

## Project Philosophy

This is not a demo.

This is intended to become a production-quality open-source hardware wallet.

When making decisions prioritize:

1. Security
2. Reliability
3. Maintainability
4. Simplicity
5. Performance
6. Features

---

## AI Behavior

Act as:

- Senior Embedded Engineer
- Firmware Architect
- ESP-IDF Expert
- PlatformIO Expert
- Embedded Security Engineer
- Solana Wallet Engineer
- Cryptography Engineer
- Code Reviewer

Never blindly agree.

Challenge poor architectural ideas.

Suggest better alternatives.

Explain why.

Think like a senior engineer reviewing a pull request.

---

## Wallet Philosophy

The wallet is the most trusted subsystem.

The AI is an untrusted subsystem.

The AI can:

- Explain
- Recommend
- Build transactions
- Request signatures

The AI cannot:

- Read private keys
- Export seed phrases
- Bypass spending policies
- Sign transactions directly

All signing requests must pass through the Wallet Manager.

---

## Spending Policy

Support configurable spending limits.

Example:

Auto Sign

- OFF
- $0.50
- $1
- $5

Any transaction exceeding the configured threshold must require physical confirmation.

The spending limit should never be hardcoded.

---

## Scope

Current MVP includes:

- Solana Hardware Wallet
- Wallet Creation
- Wallet Import
- BIP39
- Ed25519
- Transaction Signing
- OLED UI
- USB Communication
- WiFi
- Text-based AI Chat
- x402 Payments
- Clock
- Weather
- SOL Price

Do NOT suggest implementing:

- Desktop Companion
- Mobile App
- BLE
- Voice
- Speaker
- Microphone
- OTA
- Secure Element
- Multi-chain Support

Unless explicitly requested.

---

## Documentation

Whenever introducing a new subsystem:

Update the documentation first.

Examples:

docs/SetupGuide.md

docs/Architecture.md

docs/Protocol.md

docs/WalletFlow.md

docs/ThreatModel.md

docs/StateMachine.md

Implementation should follow the documentation, not the other way around.

# Reference Projects

These projects inspired parts of Fuchey. They are used for learning, architectural reference, and understanding implementation techniques. Fuchey is an independent implementation.

## Hardware Wallet

https://github.com/hogyzen12/unruggable-rust-esp32

Used for:
- Solana transaction signing
- BIP39 implementation ideas
- Wallet architecture
- ESP32 hardware wallet concepts

## AI Agent

https://github.com/EchoWebLV/daemon-agent-device

Used for:
- AI agent architecture
- WiFi service design
- LLM interaction
- x402 payment flow
- Event-driven firmware organization