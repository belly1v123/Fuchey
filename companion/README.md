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
pick the Espressif USB device. Close `pio device monitor` first — only one
program can hold the serial port.

## What it does

- Reads the device's network (devnet/mainnet), firmware and address (`hello`).
- Shows SOL and USDC balances (via the RPC in Settings).
- Sends SOL (System `Transfer`) or USDC (SPL `TransferChecked`):
  1. checks the address, amount (exact decimals, no floats), balance and,
     for SOL to an empty address, the rent-exempt minimum;
  2. builds a legacy message with a fresh blockhash;
  3. `sign_tx` → Fuchey shows it; you tap B1 (or reject);
  4. verifies the returned signature with WebCrypto Ed25519 — a bad
     signature is never broadcast;
  5. broadcasts, waits for confirmation, links to the explorer.
- **Cancel request** withdraws a pending confirmation from the device.

USDC can only be sent to wallets that already have a USDC token account
(same rule as the device parser). The device screen shows the destination
**token account**; the page shows both it and the recipient wallet.

## Protocol v1 (see `firmware/lib/protocol/UsbProtocol.hpp`)

One line per message on the USB console, mixed with log lines:

```
@@<json>*<CRC32 of json, 8 upper-case hex>\n
```

| Request | Reply |
|---|---|
| `{"id":1,"cmd":"hello"}` | `ok, proto, fw, network, max_tx, has_wallet, pubkey, busy` |
| `{"id":2,"cmd":"get_pubkey"}` | `ok, pubkey` |
| `{"id":3,"cmd":"sign_tx","msg":"<base64 message>","network":"devnet"}` | event `awaiting_confirmation {asset, amount, fee, to, timeout_ms}`, then `ok, sig` (base64, 64 bytes) |
| `{"id":4,"cmd":"cancel"}` | `ok, cancelled` |

Errors: `{"id":n,"ok":false,"err":"rejected|cancelled|timeout|busy|network_mismatch|unsupported_tx|no_wallet|bad_request|bad_crc|bad_frame|bad_json|unknown_cmd|sign_failed","detail":"..."}`.

The page can never approve, change the network, export or reset — those
are device/console only.

## Test without the browser

`firmware/scripts/fuchey_usb.py` speaks the same protocol from Python:

```sh
python firmware/scripts/fuchey_usb.py COM5 hello
python firmware/scripts/fuchey_usb.py COM5 sign-sol <recipient> 0.001 --send
```
