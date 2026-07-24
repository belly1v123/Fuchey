# Architecture

Version: 1.0
Project: Fuchey
Target: ESP32-S3
Framework: ESP-IDF
Build System: PlatformIO

---

# Overview

Fuchey is an open-source, self-custodial Solana hardware wallet with an integrated text-based AI assistant.

Unlike traditional hardware wallets, Fuchey is designed to be an everyday desk device. During normal operation, it displays useful ambient information such as the current time, weather, and SOL price. When cryptocurrency operations are required, it transitions into a secure hardware wallet while maintaining strict security boundaries.

The guiding philosophy is:

> A wallet should not look like a wallet.

Security always takes precedence over convenience or AI functionality.

---

# Goals

## Primary Goals

- Securely generate and store Solana wallet credentials.
- Sign Solana transactions on-device.
- Never expose private keys.
- Require physical confirmation for sensitive operations.
- Provide a text-based AI assistant.
- Support autonomous micropayments through configurable spending policies.
- Remain simple enough to run entirely on an ESP32-S3.

---

## Non Goals (MVP)

The following are intentionally excluded from the initial version.

- Desktop Companion
- BLE
- Mobile App
- Voice Assistant
- Speaker
- Microphone
- Secure Element
- OTA Updates
- Multi-chain Wallet Support

These may be introduced in future releases but should not influence the MVP architecture.

---

# Design Philosophy

The project follows several core principles.

## Security First

The wallet is always the most trusted subsystem.

No feature may reduce wallet security.

---

## AI Is Untrusted

The AI assistant is treated as an external service.

It may:

- Answer questions
- Explain transactions
- Build unsigned transactions
- Request wallet signatures

It may never:

- Access the seed phrase
- Access private keys
- Invoke signing directly
- Modify wallet policy

---

## Physical Confirmation

Any operation exceeding the configured spending policy must require explicit button confirmation.

Human presence is the final authorization.

---

## Simplicity

Avoid unnecessary complexity.

The MVP should solve one problem well before adding new functionality.

---

# High-Level Architecture

```
                    +----------------------+
                    |      OLED Display    |
                    +----------+-----------+
                               |
                               |
                     +---------v---------+
                     |        UI         |
                     +---------+---------+
                               |
                               |
         +---------------------+----------------------+
         |                                            |
         |                                            |
+--------v--------+                          +---------v---------+
| Wallet Manager  |                          |    AI Manager     |
+--------+--------+                          +---------+---------+
         |                                            |
         |                                            |
+--------v--------+                          +---------v---------+
|  Wallet Core    |                          |    WiFi Manager   |
+--------+--------+                          +---------+---------+
         |                                            |
         |                                            |
+--------v--------+                          +---------v---------+
| Crypto Engine   |                          | HTTP / APIs       |
+-----------------+                          +-------------------+
```

---

# Layered Architecture

The firmware is divided into independent layers.

```
Application Layer
│
├── UI
├── Wallet Manager
├── AI Manager
├── Weather
├── SOL Price
│
──────────────────────────────
System Layer
│
├── WiFi
├── Display Driver
├── Button Driver
├── USB
├── Storage
│
──────────────────────────────
Crypto Layer
│
├── BIP39
├── SLIP0010
├── Ed25519
├── SHA256
├── Base58
│
──────────────────────────────
Hardware Layer
│
ESP32-S3
OLED
Buttons
USB
```

Each layer only communicates with adjacent layers.

---

# Core Modules

## Wallet Core

Responsibilities

- Wallet creation
- Wallet import
- Key derivation
- Address generation
- Transaction signing

Must never depend on:

- AI
- WiFi
- Weather
- SOL price

---

## Wallet Manager

Responsibilities

- Wallet lifecycle
- Spending policy
- Transaction approval
- User confirmation
- Session management

This is the only module permitted to invoke signing.

---

## Crypto Engine

Contains all cryptographic operations.

Responsibilities

- BIP39
- SHA256
- HMAC
- SLIP0010
- Ed25519
- Base58

This module has no knowledge of networking or UI.

---

## AI Manager

Responsibilities

- Chat requests
- Intent parsing
- Payment requests
- Conversation management

Never directly signs anything.

---

## WiFi Manager

Responsibilities

- WiFi connection
- Reconnection
- HTTP requests
- TLS
- Network status

Other modules communicate through this interface.

No module should directly access ESP-IDF WiFi APIs.

---

## UI

Responsibilities

- Menu system
- Idle screens
- Transaction review
- Confirmation dialogs
- Notifications

---

# Idle Experience

The device cycles through:

Clock

↓

Weather

↓

SOL Price

↓

Friendly Messages

↓

Clock

Wallet functionality appears only when needed.

---

# Wallet Experience

Transaction request

↓

Wallet Manager

↓

Policy Evaluation

↓

If approval required

↓

Display Transaction

↓

Button Confirmation

↓

Wallet Core

↓

Signature

↓

Return

---

# AI Flow

User Prompt

↓

AI Manager

↓

HTTP Request

↓

Cloud LLM

↓

Response

↓

Display

If payment required

↓

Wallet Manager

↓

Policy

↓

Sign if permitted

---

# Spending Policy

The user may configure:

Auto Sign

- OFF
- $0.50
- $1
- $5

Example

```
Payment = $0.25

↓

Auto Sign Limit = $1

↓

Automatically Sign
```

Example

```
Payment = $12

↓

Display Confirmation

↓

Button

↓

Sign
```

The threshold must never be hardcoded.

---

# Event-Driven Design

Avoid giant polling loops.

Subsystems communicate through events.

Examples

- WalletCreated
- WalletImported
- TransactionReceived
- TransactionApproved
- TransactionRejected
- WiFiConnected
- WiFiDisconnected
- WeatherUpdated
- PriceUpdated

Future implementation should use FreeRTOS queues and event groups.

---

# Folder Structure

```
firmware/

src/
    main.cpp

lib/

wallet/

wallet_manager/

crypto/

wifi/

chat/

display/

buttons/

weather/

price/

policy/

protocol/

storage/

config/

docs/
```

Each module should have a single responsibility.

---

# Security Boundary

```
Internet

↓

WiFi

↓

AI Manager

↓

Wallet Manager

↓

Wallet Core

↓

Crypto Engine
```

The AI never communicates directly with Wallet Core.

Wallet Core never communicates directly with the Internet.

---

# Future Expansion

The architecture intentionally allows future support for:

- Desktop Companion
- BLE
- Secure Element
- Mobile App
- OTA Updates
- Multi-chain Wallets

These should integrate without modifying Wallet Core.

---

# Architecture Principles

When making implementation decisions, prioritize:

1. Security
2. Reliability
3. Simplicity
4. Maintainability
5. Performance
6. Features

If two approaches provide the same functionality, choose the simpler and more maintainable design.

---

# Summary

Fuchey is designed around one central idea:

The wallet is the trusted core.

Everything else—including AI, networking, and user interface—is built around that core without compromising its security.

The architecture intentionally isolates cryptographic operations from networking and AI so that future features can evolve independently while preserving the integrity of the wallet.