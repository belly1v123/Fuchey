# Fuchey Firmware Architecture

## Overview
Fuchey is an event-driven ESP32-S3 firmware combining a secure Solana hardware wallet with an ambient desk assistant and untrusted AI manager.

## Security Boundary
```
Internet -> WiFi -> AI Manager -> Wallet Manager -> Wallet Core -> Crypto Engine
```

- **AI & WiFi**: Untrusted, assigned to CPU Core 0 at lower priority.
- **Wallet Core**: Security kernel, assigned to CPU Core 1, strictly isolated from networking APIs.
- **Wallet Manager**: Intermediary enforcing spending policies and physical button confirmation.
