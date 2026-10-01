// Fuchey companion — minimal Solana helpers (no dependencies).
// Builds exactly the two legacy message shapes the device's TxParser accepts:
//   SOL : System Program Transfer
//   USDC: SPL Token TransferChecked (with the USDC mint, 6 decimals)

export const NETWORKS = {
  devnet: {
    rpc: "https://api.devnet.solana.com",
    usdcMint: "4zMMC9srt5Ri5X14GAgXhaHii3GnPAEERYPJgZJDncDU",
    explorerSuffix: "?cluster=devnet",
  },
  mainnet: {
    rpc: "https://api.mainnet-beta.solana.com",
    usdcMint: "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v",
    explorerSuffix: "",
  },
};

export const TOKEN_PROGRAM = "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA";
export const ATA_PROGRAM = "ATokenGPvbdGVxr1b2hvZbsiqW5xWH25efTNsLJA8knL";
// Rent-exempt minimum of a 165-byte token account (what opening one costs).
export const TOKEN_ACCOUNT_RENT_LAMPORTS = 2039280n;
export const SOL_DECIMALS = 9;
export const USDC_DECIMALS = 6;
export const BASE_FEE_LAMPORTS = 5000n;   // per signature; Fuchey txs have 1

// ── base58 ────────────────────────────────────────────────
const B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

export function b58decode(s) {
  let n = 0n;
  for (const ch of s) {
    const v = B58.indexOf(ch);
    if (v < 0) throw new Error("invalid base58");
    n = n * 58n + BigInt(v);
  }
  const bytes = [];
  while (n > 0n) {
    bytes.unshift(Number(n & 0xffn));
    n >>= 8n;
  }
  let pad = 0;
  while (pad < s.length && s[pad] === "1") pad++;
  return Uint8Array.from([...new Array(pad).fill(0), ...bytes]);
}

export function b58encode(bytes) {
  let n = 0n;
  for (const b of bytes) n = (n << 8n) | BigInt(b);
  let out = "";
  while (n > 0n) {
    out = B58[Number(n % 58n)] + out;
    n /= 58n;
  }
  let pad = 0;
  while (pad < bytes.length && bytes[pad] === 0) pad++;
  return "1".repeat(pad) + out;
}

export function isAddress(s) {
  try {
    return typeof s === "string" && s.length >= 32 && s.length <= 44 && b58decode(s).length === 32;
  } catch {
    return false;
  }
}

// ── exact decimal amounts (BigInt, no floats) ─────────────
export function parseUnits(text, decimals) {
  const s = String(text).trim();
  if (!/^\d*\.?\d*$/.test(s) || s === "" || s === ".") throw new Error("Enter a number");
  const [whole, frac = ""] = s.split(".");
  if (frac.length > decimals) throw new Error(`Max ${decimals} decimal places`);
  return BigInt(whole || "0") * 10n ** BigInt(decimals) + BigInt((frac + "0".repeat(decimals)).slice(0, decimals) || "0");
}

export function formatUnits(value, decimals) {
  const v = BigInt(value);
  const scale = 10n ** BigInt(decimals);
  const whole = v / scale;
  let frac = (v % scale).toString().padStart(decimals, "0").replace(/0+$/, "");
  return frac ? `${whole}.${frac}` : `${whole}`;
}

// ── message building ──────────────────────────────────────
function compactU16(n) {
  const out = [];
  for (;;) {
    let b = n & 0x7f;
    n >>= 7;
    if (n === 0) { out.push(b); return out; }
    out.push(b | 0x80);
  }
}

function u64le(v) {
  const out = new Uint8Array(8);
  let x = BigInt(v);
  for (let i = 0; i < 8; i++) { out[i] = Number(x & 0xffn); x >>= 8n; }
  return out;
}

function concat(parts) {
  const len = parts.reduce((a, p) => a + p.length, 0);
  const out = new Uint8Array(len);
  let o = 0;
  for (const p of parts) { out.set(p, o); o += p.length; }
  return out;
}

/** Legacy message: System Program Transfer {lamports}. */
export function buildSolTransfer({ from, to, lamports, blockhash }) {
  return concat([
    Uint8Array.from([1, 0, 1]),                     // header: 1 signer, 0 ro-signed, 1 ro-unsigned
    Uint8Array.from(compactU16(3)),
    b58decode(from), b58decode(to), new Uint8Array(32),  // [from, to, System Program]
    b58decode(blockhash),
    Uint8Array.from([...compactU16(1), 2, ...compactU16(2), 0, 1, ...compactU16(12)]),
    Uint8Array.from([2, 0, 0, 0]),                   // Transfer
    u64le(lamports),
  ]);
}

// ── associated token account (PDA) ───────────────────────
const ED_P = 2n ** 255n - 19n;
const ED_D = 37095705934669439343138083508754565189542113879843219016388785533085940283555n;

function modPow(b, e, m) {
  let r = 1n;
  b %= m;
  for (; e > 0n; e >>= 1n, b = (b * b) % m) if (e & 1n) r = (r * b) % m;
  return r;
}

// Same test as Solana's bytes_are_curve_point (and the device's TxParser).
function isOnCurve(bytes) {
  let y = 0n;
  for (let i = 31; i >= 0; i--) y = (y << 8n) | BigInt(i === 31 ? bytes[i] & 0x7f : bytes[i]);
  y %= ED_P;
  const y2 = (y * y) % ED_P;
  const u = (y2 - 1n + ED_P) % ED_P;
  const v = (ED_D * y2 + 1n) % ED_P;
  return u === 0n || modPow((u * v) % ED_P, (ED_P - 1n) / 2n, ED_P) === 1n;
}

/** Associated token account address of (owner, mint). */
export async function findAssociatedTokenAddress(owner, mint) {
  const marker = new TextEncoder().encode("ProgramDerivedAddress");
  for (let bump = 255; bump >= 0; bump--) {
    const seed = concat([
      b58decode(owner), b58decode(TOKEN_PROGRAM), b58decode(mint),
      Uint8Array.from([bump]), b58decode(ATA_PROGRAM), marker,
    ]);
    const h = new Uint8Array(await crypto.subtle.digest("SHA-256", seed));
    if (!isOnCurve(h)) return b58encode(h);
  }
  throw new Error("No associated token address");
}

/**
 * Legacy message: ATA CreateIdempotent for `recipient`, then SPL Token
 * TransferChecked into that new account. The device verifies the ATA
 * derivation and shows `recipient` (the owner wallet).
 */
export function buildTokenTransferCheckedWithCreate({ owner, source, recipient, ata, mint, amount, decimals, blockhash }) {
  // keys: 0 owner (signer, w) · 1 source (w) · 2 ata (w) · 3 mint · 4 token
  //       program · 5 ATA program · 6 System Program · 7 recipient wallet
  return concat([
    Uint8Array.from([1, 0, 5]),                     // 1 signer; last 5 keys read-only
    Uint8Array.from(compactU16(8)),
    b58decode(owner), b58decode(source), b58decode(ata),
    b58decode(mint), b58decode(TOKEN_PROGRAM), b58decode(ATA_PROGRAM),
    new Uint8Array(32), b58decode(recipient),
    b58decode(blockhash),
    Uint8Array.from(compactU16(2)),
    // CreateIdempotent: [payer, ata, owner, mint, system, token]
    Uint8Array.from([5, ...compactU16(6), 0, 2, 7, 3, 6, 4, ...compactU16(1), 1]),
    // TransferChecked: [source, mint, destination, owner]
    Uint8Array.from([4, ...compactU16(4), 1, 3, 2, 0, ...compactU16(10)]),
    Uint8Array.from([12]),
    u64le(amount),
    Uint8Array.from([decimals]),
  ]);
}

/** Legacy message: SPL Token TransferChecked {amount, decimals}. */
export function buildTokenTransferChecked({ owner, source, destination, mint, amount, decimals, blockhash }) {
  return concat([
    Uint8Array.from([1, 0, 2]),                     // 1 signer; mint + token program read-only
    Uint8Array.from(compactU16(5)),
    b58decode(owner), b58decode(source), b58decode(destination),
    b58decode(mint), b58decode(TOKEN_PROGRAM),
    b58decode(blockhash),
    Uint8Array.from([...compactU16(1), 4, ...compactU16(4), 1, 3, 2, 0, ...compactU16(10)]),
    Uint8Array.from([12]),                          // TransferChecked
    u64le(amount),
    Uint8Array.from([decimals]),
  ]);
}

/** Wire transaction: [compact-u16 1][signature 64][message]. */
export function assembleTransaction(signature, message) {
  return concat([Uint8Array.from([1]), signature, message]);
}

/** Verify the device's signature before broadcasting (WebCrypto Ed25519). */
export async function verifySignature(pubkeyB58, signature, message) {
  try {
    const key = await crypto.subtle.importKey("raw", b58decode(pubkeyB58), { name: "Ed25519" }, false, ["verify"]);
    return (await crypto.subtle.verify({ name: "Ed25519" }, key, signature, message)) ? "valid" : "invalid";
  } catch {
    return "unsupported";   // older browser without Ed25519 in WebCrypto
  }
}

// ── RPC ───────────────────────────────────────────────────
export class Rpc {
  constructor(url) {
    this.url = url;
  }

  async call(method, params = []) {
    const res = await fetch(this.url, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ jsonrpc: "2.0", id: 1, method, params }),
    });
    if (!res.ok) throw new Error(`RPC HTTP ${res.status}`);
    const body = await res.json();
    if (body.error) throw new Error(body.error.message || "RPC error");
    return body.result;
  }

  async getBalance(address) {
    return BigInt((await this.call("getBalance", [address, { commitment: "confirmed" }])).value);
  }

  async getLatestBlockhash() {
    // "confirmed" (not "finalized"): ~13 s fresher, which matters because the
    // blockhash has to survive the wait for B1 on the device.
    return (await this.call("getLatestBlockhash", [{ commitment: "confirmed" }])).value.blockhash;
  }

  async getMinimumRentExempt(size = 0) {
    return BigInt(await this.call("getMinimumBalanceForRentExemption", [size]));
  }

  /** Largest-balance token account of `owner` for `mint` (same rule as the device). */
  async getTokenAccount(owner, mint) {
    const r = await this.call("getTokenAccountsByOwner", [owner, { mint }, { encoding: "jsonParsed", commitment: "confirmed" }]);
    let best = null;
    for (const item of r.value) {
      const amount = BigInt(item.account.data.parsed.info.tokenAmount.amount);
      if (!best || amount > best.amount) best = { address: item.pubkey, amount };
    }
    return best;   // null → owner has no token account for this mint
  }

  async sendTransaction(wireBytes) {
    let s = "";
    for (const b of wireBytes) s += String.fromCharCode(b);
    const params = [btoa(s), { encoding: "base64", preflightCommitment: "confirmed", maxRetries: 5 }];
    // Public devnet RPC is load-balanced; a lagging node can briefly report a
    // fresh blockhash as unknown. Retry a few times before giving up.
    for (let attempt = 1; ; attempt++) {
      try {
        return await this.call("sendTransaction", params);
      } catch (e) {
        if (attempt >= 4 || !/blockhash not found/i.test(e.message)) throw e;
        await new Promise((r) => setTimeout(r, 1500));
      }
    }
  }

  /** Poll until confirmed/finalized, an on-chain error, or timeout. */
  async confirm(signature, timeoutMs = 60000) {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
      const r = await this.call("getSignatureStatuses", [[signature]]);
      const st = r.value[0];
      if (st?.err) throw new Error(`Transaction failed: ${JSON.stringify(st.err)}`);
      if (st && (st.confirmationStatus === "confirmed" || st.confirmationStatus === "finalized")) return st;
      await new Promise((r2) => setTimeout(r2, 1500));
    }
    throw new Error("Not confirmed within 60 s (it may still land — check the explorer)");
  }
}
