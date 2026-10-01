#!/usr/bin/env python3
"""
Fuchey companion protocol test tool (USB serial, protocol v1).

Frames:  @@<json>*<CRC32 hex>\n   (CRC32 = zlib.crc32 of the JSON bytes)

Usage:
  python fuchey_usb.py COM5 hello
  python fuchey_usb.py COM5 pubkey
  python fuchey_usb.py COM5 sign-sol <recipient> <amount_sol> [--send] [--rpc URL]
  python fuchey_usb.py COM5 cancel

sign-sol builds a legacy System Transfer on the device's network, asks the
device to sign it (tap B1 on Fuchey), and with --send broadcasts it.
Needs: pip install pyserial   (already present if PlatformIO is installed)
"""

import argparse
import base64
import json
import sys
import time
import urllib.request
import zlib
from decimal import Decimal

import serial  # pyserial

B58_ALPHABET = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
RPC = {
    "devnet": "https://api.devnet.solana.com",
    "mainnet": "https://api.mainnet-beta.solana.com",
}


# ── base58 ────────────────────────────────────────────────
def b58decode(s: str) -> bytes:
    n = 0
    for ch in s:
        n = n * 58 + B58_ALPHABET.index(ch)
    raw = n.to_bytes((n.bit_length() + 7) // 8, "big") if n else b""
    pad = len(s) - len(s.lstrip("1"))
    return b"\x00" * pad + raw


def b58encode(b: bytes) -> str:
    n = int.from_bytes(b, "big")
    out = ""
    while n:
        n, r = divmod(n, 58)
        out = B58_ALPHABET[r] + out
    pad = len(b) - len(b.lstrip(b"\x00"))
    return "1" * pad + out


# ── framing ───────────────────────────────────────────────
class Device:
    def __init__(self, port: str):
        # Open with DTR/RTS de-asserted: toggling them on open resets the S3.
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = 115200
        self.ser.timeout = 0.2
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        # Raise DTR only after opening (RTS stays low): the "run" state, which
        # never resets the chip, and the USB CDC console only transmits to a
        # host that has DTR asserted.
        self.ser.dtr = True
        self.next_id = 1
        self.buf = b""

    def send(self, cmd: str, **params) -> int:
        rid = self.next_id
        self.next_id += 1
        body = json.dumps({"id": rid, "cmd": cmd, **params}, separators=(",", ":"))
        line = f"@@{body}*{zlib.crc32(body.encode()) & 0xFFFFFFFF:08X}\n".encode()
        # Chunked write: gentle on the device's USB RX buffer.
        for i in range(0, len(line), 256):
            self.ser.write(line[i:i + 256])
            time.sleep(0.01)
        return rid

    def frames(self, timeout: float):
        """Yield decoded protocol frames; print log lines as they arrive."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.buf += self.ser.read(4096)
            while b"\n" in self.buf:
                raw, self.buf = self.buf.split(b"\n", 1)
                text = raw.decode("utf-8", "replace").rstrip("\r")
                at = text.find("@@")   # a log fragment may precede the frame
                if at < 0:
                    if text.strip():
                        print(f"  [log] {text}")
                    continue
                if at > 0:
                    print(f"  [log] {text[:at]}")
                body, _, crc = text[at + 2:].rpartition("*")
                if f"{zlib.crc32(body.encode()) & 0xFFFFFFFF:08X}" != crc:
                    print(f"  [bad crc] {text}")
                    continue
                yield json.loads(body)

    def request(self, cmd: str, timeout: float = 5.0, **params) -> dict:
        rid = self.send(cmd, **params)
        for frame in self.frames(timeout):
            if frame.get("id") != rid:
                continue
            if "event" in frame:
                print(f"  [event] {frame}")
                continue
            return frame
        raise TimeoutError(f"no reply to {cmd}")


# ── Solana helpers ────────────────────────────────────────
def rpc(url: str, method: str, params: list):
    req = urllib.request.Request(
        url,
        data=json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params}).encode(),
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=15) as r:
        out = json.load(r)
    if "error" in out:
        raise RuntimeError(out["error"])
    return out["result"]


def build_sol_transfer(sender: bytes, recipient: bytes, lamports: int, blockhash: bytes) -> bytes:
    msg = bytes([1, 0, 1, 3]) + sender + recipient + bytes(32) + blockhash
    msg += bytes([1, 2, 2, 0, 1, 12]) + (2).to_bytes(4, "little") + lamports.to_bytes(8, "little")
    return msg


# ── main ──────────────────────────────────────────────────
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("cmd", choices=["hello", "pubkey", "sign-sol", "cancel"])
    ap.add_argument("args", nargs="*")
    ap.add_argument("--send", action="store_true", help="broadcast after signing")
    ap.add_argument("--rpc", help="override RPC URL")
    a = ap.parse_args()

    dev = Device(a.port)
    time.sleep(0.3)

    if a.cmd == "hello":
        print(dev.request("hello"))
    elif a.cmd == "pubkey":
        print(dev.request("get_pubkey"))
    elif a.cmd == "cancel":
        print(dev.request("cancel"))
    elif a.cmd == "sign-sol":
        if len(a.args) != 2:
            sys.exit("usage: sign-sol <recipient> <amount_sol>")
        info = dev.request("hello")
        if not info.get("ok") or not info.get("has_wallet"):
            sys.exit(f"device not ready: {info}")
        network = info["network"]
        url = a.rpc or RPC[network]
        lamports = int(Decimal(a.args[1]) * 10**9)
        bh = rpc(url, "getLatestBlockhash", [{"commitment": "finalized"}])["value"]["blockhash"]
        msg = build_sol_transfer(b58decode(info["pubkey"]), b58decode(a.args[0]), lamports, b58decode(bh))
        print(f"network={network} from={info['pubkey']} lamports={lamports}")
        print("Check the Fuchey screen and tap B1 (double/long press to reject)...")
        res = dev.request("sign_tx", timeout=45, msg=base64.b64encode(msg).decode(), network=network)
        print(res)
        if res.get("ok") and a.send:
            wire = bytes([1]) + base64.b64decode(res["sig"]) + msg
            sig = rpc(url, "sendTransaction", [base64.b64encode(wire).decode(), {"encoding": "base64"}])
            print(f"sent: {sig}")
            print(f"https://explorer.solana.com/tx/{sig}" + ("?cluster=devnet" if network == "devnet" else ""))


if __name__ == "__main__":
    main()
