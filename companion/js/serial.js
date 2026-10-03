// Fuchey companion — USB transport (Web Serial) for protocol v1.
//
// Frames are single lines shared with the device's log output:
//   @@<json>*<CRC32 hex, 8 upper-case chars>\n
// Everything else on the line stream is a log line.

const PREFIX = "@@";
const CHUNK = 256;          // bytes per write; gentle on the device RX buffer
const CHUNK_DELAY_MS = 8;

// ── CRC32 (IEEE / zlib), identical to UsbProtocol::crc32 ──
const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

export function crc32(bytes) {
  let crc = 0xffffffff;
  for (const b of bytes) crc = CRC_TABLE[(crc ^ b) & 0xff] ^ (crc >>> 8);
  return (crc ^ 0xffffffff) >>> 0;
}

const hex8 = (n) => n.toString(16).toUpperCase().padStart(8, "0");
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

export class DeviceError extends Error {
  constructor(code, detail) {
    super(detail ? `${code}: ${detail}` : code);
    this.code = code;
    this.detail = detail;
  }
}

export class FucheyDevice extends EventTarget {
  constructor() {
    super();
    this.port = null;
    this.reader = null;
    this.nextId = 1;
    this.pending = new Map();   // id → {resolve, reject, onEvent, timer}
    this.encoder = new TextEncoder();
  }

  static supported() {
    return "serial" in navigator;
  }

  get connected() {
    return this.port !== null;
  }

  async connect() {
    // Espressif USB VID (native USB CDC). The picker still lists others if absent.
    this.port = await navigator.serial.requestPort({ filters: [{ usbVendorId: 0x303a }] });
    await this.port.open({ baudRate: 115200, bufferSize: 16384 });
    try {
      // "Run" line state: never the DTR/RTS pattern that resets into the bootloader.
      await this.port.setSignals({ dataTerminalReady: true, requestToSend: false });
    } catch { /* not supported on every platform */ }
    this.port.addEventListener?.("disconnect", () => this._closed("disconnected"));
    this._readLoop();
  }

  async disconnect() {
    const port = this.port;
    if (!port) return;
    this.port = null;
    try { await this.reader?.cancel(); } catch {}
    try { await port.close(); } catch {}
    this._closed("closed");
  }

  _closed(reason) {
    for (const [, p] of this.pending) {
      clearTimeout(p.timer);
      p.reject(new DeviceError("disconnected", reason));
    }
    this.pending.clear();
    this.port = null;
    this.dispatchEvent(new CustomEvent("disconnect", { detail: reason }));
  }

  async _readLoop() {
    const decoder = new TextDecoderStream();
    this.port.readable.pipeTo(decoder.writable).catch(() => {});
    this.reader = decoder.readable.getReader();
    let buf = "";
    try {
      while (true) {
        const { value, done } = await this.reader.read();
        if (done) break;
        buf += value;
        let nl;
        while ((nl = buf.indexOf("\n")) >= 0) {
          const line = buf.slice(0, nl).replace(/\r$/, "");
          buf = buf.slice(nl + 1);
          this._onLine(line);
        }
        if (buf.length > 65536) buf = "";   // runaway line without newline
      }
    } catch {
      /* port closed */
    } finally {
      try { this.reader.releaseLock(); } catch {}
      if (this.port) this._closed("read ended");
    }
  }

  _onLine(line) {
    const at = line.indexOf(PREFIX);
    if (at < 0) {
      if (line.trim()) this.dispatchEvent(new CustomEvent("log", { detail: line }));
      return;
    }
    const frame = line.slice(at + PREFIX.length);
    const star = frame.lastIndexOf("*");
    if (star < 0) return;
    const body = frame.slice(0, star);
    if (hex8(crc32(this.encoder.encode(body))) !== frame.slice(star + 1)) {
      this.dispatchEvent(new CustomEvent("log", { detail: `[bad CRC frame] ${line}` }));
      return;
    }
    let msg;
    try { msg = JSON.parse(body); } catch { return; }

    const p = this.pending.get(msg.id);
    if (!p) {
      this.dispatchEvent(new CustomEvent("frame", { detail: msg }));
      return;
    }
    if (msg.event) {
      p.onEvent?.(msg);
      return;
    }
    clearTimeout(p.timer);
    this.pending.delete(msg.id);
    if (msg.ok) p.resolve(msg);
    else p.reject(new DeviceError(msg.err || "error", msg.detail));
  }

  async _write(text) {
    const bytes = this.encoder.encode(text);
    const writer = this.port.writable.getWriter();
    try {
      for (let i = 0; i < bytes.length; i += CHUNK) {
        await writer.write(bytes.subarray(i, i + CHUNK));
        if (i + CHUNK < bytes.length) await sleep(CHUNK_DELAY_MS);
      }
    } finally {
      writer.releaseLock();
    }
  }

  /**
   * Send a command and resolve with the device's final reply.
   * onEvent(frame) receives intermediate events (e.g. awaiting_confirmation).
   */
  request(cmd, params = {}, { timeoutMs = 5000, onEvent } = {}) {
    if (!this.port) return Promise.reject(new DeviceError("disconnected"));
    const id = this.nextId++;
    const body = JSON.stringify({ id, cmd, ...params });
    const line = `${PREFIX}${body}*${hex8(crc32(this.encoder.encode(body)))}\n`;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new DeviceError("no_reply", `${cmd} timed out`));
      }, timeoutMs);
      this.pending.set(id, { resolve, reject, onEvent, timer });
      this._write(line).catch((e) => {
        clearTimeout(timer);
        this.pending.delete(id);
        reject(e);
      });
    });
  }

  /** hello with retries (the device may still be booting after connect). */
  // Opening the port can restart Fuchey; it needs ~7 s to boot, so keep
  // asking for up to ~12 s before giving up.
  async hello(attempts = 8) {
    let last;
    for (let i = 0; i < attempts; i++) {
      try {
        return await this.request("hello", {}, { timeoutMs: 1500 });
      } catch (e) {
        last = e;
        if (e.code !== "no_reply") throw e;
      }
    }
    throw last;
  }

  /** Ask the device to sign a legacy message. Resolves with 64 signature bytes. */
  async signMessage(messageBytes, network, onEvent) {
    const res = await this.request(
      "sign_tx",
      { msg: bytesToBase64(messageBytes), network },
      { timeoutMs: 60000, onEvent },   // device times out at 30 s; RPC price fetch adds a little
    );
    return base64ToBytes(res.sig);
  }

  // ── Device settings (firmware cap "settings_v1") ──
  getStatus() {
    return this.request("get_status", {}, { timeoutMs: 3000 });
  }

  async wifiScan() {
    return (await this.request("wifi_scan", {}, { timeoutMs: 12000 })).networks || [];
  }

  /** The password goes to the device only; it is never returned or stored here. */
  setWifi(ssid, password) {
    return this.request("set_wifi", { ssid, password }, { timeoutMs: 8000 });
  }

  setLocation(city, lat, lon) {
    return this.request("set_location", { city, lat, lon }, { timeoutMs: 5000 });
  }

  // ── Scrambled-grid recovery (cap "recovery_grid") ──
  // Only cell positions travel over USB; the letters/words are on Fuchey's screen.
  recoveryStart(purpose, words) {
    return this.request("recovery_start", { purpose, words }, { timeoutMs: 4000 });
  }

  recoveryTap(pos) {
    // The last tap derives keys on the device (PBKDF2) — allow a few seconds.
    return this.request("recovery_tap", { pos }, { timeoutMs: 15000 });
  }

  // ── Create wallet (cap "wallet_create") — words shown on the device only ──
  walletCreateStart(words) {
    return this.request("wallet_create_start", { words }, { timeoutMs: 5000 });
  }

  walletCreateState() {
    return this.request("wallet_create_state", {}, { timeoutMs: 3000 });
  }

  walletCreateTap(pos) {
    // The final confirm stores the wallet (PBKDF2 + NVS) — allow a few seconds.
    return this.request("wallet_create_tap", { pos }, { timeoutMs: 15000 });
  }

  walletCreateCancel() {
    return this.request("wallet_create_cancel", {}, { timeoutMs: 3000 });
  }

  recoveryCancel() {
    return this.request("recovery_cancel", {}, { timeoutMs: 3000 });
  }

  /** Ask Fuchey to switch devnet/mainnet. Applied only after B1 on the device. */
  setNetwork(network, onEvent) {
    return this.request("set_network", { network }, { timeoutMs: 45000, onEvent });
  }

  /** Open the Receive QR screen on the device (firmware with caps "show_address"). */
  showAddress() {
    return this.request("show_address", {}, { timeoutMs: 3000 });
  }

  cancel() {
    return this.request("cancel", {}, { timeoutMs: 3000 });
  }
}

export function bytesToBase64(bytes) {
  let s = "";
  for (const b of bytes) s += String.fromCharCode(b);
  return btoa(s);
}

export function base64ToBytes(b64) {
  const s = atob(b64);
  const out = new Uint8Array(s.length);
  for (let i = 0; i < s.length; i++) out[i] = s.charCodeAt(i);
  return out;
}
