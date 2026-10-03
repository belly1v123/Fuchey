# Fuchey Companion (USB web wallet)

A single static web page that talks to Fuchey over USB (Web Serial).
The page builds Solana transactions and broadcasts them; **Fuchey parses
each one, shows it on its screen and signs only after you tap B1 on the
device.** The private key never leaves Fuchey.

No build step and no third-party JavaScript: `index.html`, `css/`, `js/`.

## Run

Web Serial needs desktop **Chrome or Edge** and a secure origin
(`http://localhost` counts).

```sh
cd companion
python -m http.server 8765 --bind 127.0.0.1
```

Open <http://localhost:8765>, plug Fuchey in, press **Connect Fuchey** and
pick the Espressif USB device (COM5 on the dev PC). Close `pio device
monitor` first — only one program can hold the serial port. Connecting can
restart Fuchey; the page waits ~12 s for it to boot. Check the badge shows
**DEVNET** before sending. Hard-reload (Ctrl+Shift+R) after updating files.

## What it does

- Reads the device's network (devnet/mainnet), firmware and address (`hello`).
- Shows SOL and USDC balances (via the RPC in Settings).
- Sends SOL (System `Transfer`) or USDC (SPL `TransferChecked`):
  1. checks the address, amount (exact decimals, no floats), balance and,
     for SOL to an empty address, the rent-exempt minimum;
     asks "are you sure?" above 0.5 SOL / 50 USDC (`LARGE_AMOUNT` in `app.js`);
  2. builds a legacy message with a fresh `confirmed` blockhash;
  3. `sign_tx` → Fuchey shows it; you tap B1 (or reject);
  4. verifies the returned signature with WebCrypto Ed25519 — a bad
     signature is never broadcast;
  5. broadcasts (retries briefly on "Blockhash not found"), waits for
     confirmation, links to the explorer. If confirmation fails after the
     broadcast, the explorer link is kept.
- **Cancel request** withdraws a pending confirmation from the device.
- **Receive (QR)** shows the address as a QR code; **Show on Fuchey** opens
  the device's own Receive QR screen (firmware cap `show_address`).
- **Device setup** (firmware cap `settings_v1`) replaces the serial-console
  setup: shows WiFi + weather-location status, scans WiFi networks, sends
  SSID + password (WPA2, 8–63 chars; the password is write-only — never
  stored by the page or returned by the device), and sets the weather
  location by town search (Open-Meteo geocoding, from the browser) or this
  computer's location (browser permission). Names are converted to ASCII
  for the device's font.

USDC to a wallet that already has a USDC account: the device screen shows
the destination **token account**; the page shows both it and the wallet.

USDC to a wallet **without** a USDC account (firmware cap
`usdc_create_ata`): the page derives the recipient's associated token
account and adds an ATA `CreateIdempotent` instruction; you pay the rent
(0.00203928 SOL) once. Fuchey checks the account really is that wallet's
ATA, then shows **TO WALLET** + the wallet address and a yellow
"+ new USDC acct" line.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| "The USB port is busy" | Another program has COM5 (pio monitor, `fuchey_usb.py`, another tab). Close it. |
| "Fuchey did not answer" | Still booting after connect — wait for the home screen, Connect again. |
| "Blockhash not found" | Tx expired before broadcast (devnet blockhash ≈ 35 s). Nothing was sent; send again and tap B1 promptly. |
| "Rejected" / "expired" | B1 double/long press, or no answer in 30 s. Nothing was signed. |
| Script hears nothing | Raise DTR after opening the port (the device only transmits with DTR set). |

## Protocol v1 (see `firmware/lib/protocol/UsbProtocol.hpp`)

One line per message on the USB console, mixed with log lines:

```
@@<json>*<CRC32 of json, 8 upper-case hex>\n
```

| Request | Reply |
|---|---|
| `{"id":1,"cmd":"hello"}` | `ok, proto, fw, network, max_tx, has_wallet, pubkey, busy, caps` |
| `{"id":2,"cmd":"get_pubkey"}` | `ok, pubkey` |
| `{"id":3,"cmd":"sign_tx","msg":"<base64 message>","network":"devnet"}` | event `awaiting_confirmation {asset, amount, fee, to, timeout_ms[, creates_account, rent]}`, then `ok, sig` (base64, 64 bytes) |
| `{"id":4,"cmd":"cancel"}` | `ok, cancelled` |
| `{"id":5,"cmd":"show_address"}` | `ok` (device opens its Receive QR screen) |
| `{"id":6,"cmd":"get_status"}` | `ok, wifi {configured, connected, online, ssid}, location {configured, city, lat, lon}, setup_done, network` |
| `{"id":7,"cmd":"wifi_scan"}` | `ok, networks [{ssid, rssi, secure}]` (strongest first, ≤ 20) or `scan_failed` |
| `{"id":8,"cmd":"set_wifi","ssid":"…","password":"…"}` | `ok` once the connection attempt started — poll `get_status` |
| `{"id":9,"cmd":"set_location","city":"Chitwan","lat":27.68,"lon":84.43}` | `ok` (saved; weather refreshes) |

### Command tiers

| Tier | Commands | Rule |
|---|---|---|
| Read-only | `hello`, `get_pubkey`, `get_status`, `wifi_scan`, `show_address` | Always allowed. Never returns secrets (no WiFi password, no keys). |
| Device settings | `set_wifi`, `set_location` (later: pet / wearable settings) | Allowed without a button press: they cannot move funds or touch keys. Refused while a signature is pending. Inputs are length/charset-checked. Secrets are write-only and never logged. |
| Signing | `sign_tx`, `cancel` | Parsed by TxParser, shown on the TFT, approved only by a hardware B1 tap. |
| Device-confirmed (future) | e.g. `network devnet/mainnet` | Would need a physical B1 confirmation on a dedicated screen. Not exposed today. |
| Never over USB | approve, wallet create / import / export / reset, reading keys | Device or console only (and export/reset are slated for removal). |


Errors: `{"id":n,"ok":false,"err":"rejected|cancelled|timeout|busy|network_mismatch|unsupported_tx|no_wallet|bad_request|bad_crc|bad_frame|bad_json|unknown_cmd|sign_failed","detail":"..."}`.

The page can never approve, change the network, export or reset — those
are device/console only.

## Test without the browser

`firmware/scripts/fuchey_usb.py` speaks the same protocol from Python:

```sh
python firmware/scripts/fuchey_usb.py COM5 hello
python firmware/scripts/fuchey_usb.py COM5 sign-sol <recipient> 0.001 --send
```
