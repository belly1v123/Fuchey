# Fuchey Firmware Setup Guide

## System Requirements
- PlatformIO Core or Visual Studio Code with PlatformIO extension
- ESP-IDF v5.x toolchain
- ESP32-S3 Board (16MB Flash, 8MB OPI PSRAM)

## Building the Project
```bash
cd firmware
~/.platformio/penv/bin/pio run -e esp32s3_dev
```

## Flashing to Hardware
```bash
cd firmware
~/.platformio/penv/bin/pio run -e esp32s3_dev --target upload
```

## Serial Monitor
```bash
cd firmware
~/.platformio/penv/bin/pio device monitor -b 115200
```

