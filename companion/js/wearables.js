// Fuchey companion — marketplace wearables → Fuchey item files (no DOM).
//
// The website (fuchey.xyz) keeps the catalogue in Supabase (`wearables`,
// `loadouts`) and sells each wearable as Metaplex Core NFTs in its own
// collection. Here we:
//   1. find which wearables a set of wallets owns (Core assets on-chain),
//   2. resolve each one's `visual` for the Yeti (perCharacter + x/y),
//   3. pack it into the device's "FWR1" item file (see firmware
//      lib/wearables/WearItem.hpp) so Fuchey can draw it.
// Nothing here signs or moves anything.

import { b58encode } from "./solana.js";

// Back to front (the website's SLOT_ORDER; also the device's slot index).
export const SLOTS = ["outfit", "backpack", "accessory", "headwear", "hat", "held", "special"];
export const CHARACTER = "yeti";       // the device's character (phase 1)

export const CORE_PROGRAM = "CoREENxT6tW1HoK8ypY1SxRMZTcVPm7R94rH4PZNhX7d";

// Device network names → the website's.
export const siteNetwork = (net) => (net === "mainnet" ? "mainnet-beta" : "devnet");

// ── Supabase (public, read-only via RLS) ──────────────────
export class Catalog {
  constructor({ SUPABASE_URL, SUPABASE_ANON_KEY }) {
    this.url = String(SUPABASE_URL || "").replace(/\/$/, "");
    this.key = SUPABASE_ANON_KEY || "";
  }

  get configured() {
    return Boolean(this.url && this.key && !this.url.includes("<") && !this.key.includes("<"));
  }

  async select(table, query) {
    const params = new URLSearchParams(query);
    const res = await fetch(`${this.url}/rest/v1/${table}?${params}`, {
      headers: { apikey: this.key, Authorization: `Bearer ${this.key}` },
    });
    if (!res.ok) throw new Error(`Couldn't load ${table} (HTTP ${res.status})`);
    return res.json();
  }

  /** Published wearables (RLS only returns published rows). */
  wearables() {
    return this.select("wearables", {
      select: "id,name,type,rarity,compatible_characters,nft,visual,image_url",
      published: "eq.true",
      order: "sort_order.asc",
    });
  }

  /** Published characters (Yeti, …) with their per-network NFT info. */
  characters() {
    return this.select("characters", { select: "id,name,title,nft", published: "eq.true" });
  }

  /** { slot: wearableId } a wallet saved for the Yeti on the website, or null. */
  async loadout(wallet, character = CHARACTER) {
    const rows = await this.select("loadouts", {
      select: "slots",
      wallet: `eq.${wallet}`,
      character_id: `eq.${character}`,
    });
    return rows[0]?.slots ?? null;
  }
}

// ── Ownership (Metaplex Core, read straight from the chain) ──
// AssetV1 layout: key u8 (=1) | owner [32] | updateAuthority kind u8
// (2 = Collection) | collection [32] | name, uri, …
// Only the collection counts as proof of what an asset is: names and
// images are never trusted (same rule as the website's backend).
export function decodeAssetHead(bytes) {
  if (bytes.length < 66 || bytes[0] !== 1) return null;
  return {
    owner: b58encode(bytes.subarray(1, 33)),
    collection: bytes[33] === 2 ? b58encode(bytes.subarray(34, 66)) : null,
  };
}

const b64bytes = (s) => Uint8Array.from(atob(s), (c) => c.charCodeAt(0));

/** Every Core asset `owner` holds → [{ address, collection }]. */
export async function coreAssetsOwnedBy(rpc, owner) {
  const accounts = await rpc.call("getProgramAccounts", [
    CORE_PROGRAM,
    {
      encoding: "base64",
      commitment: "confirmed",
      dataSlice: { offset: 0, length: 66 },
      filters: [
        { memcmp: { offset: 0, bytes: "2" } },   // base58 of byte 0x01: key = AssetV1
        { memcmp: { offset: 1, bytes: owner } },
      ],
    },
  ]);
  const out = [];
  for (const a of accounts ?? []) {
    const head = decodeAssetHead(b64bytes(a.account.data[0]));
    if (head && head.owner === owner) out.push({ address: a.pubkey, collection: head.collection });
  }
  return out;
}

/** collection address → wearable, for minted wearables on `network` (site name). */
export function collectionIndex(wearables, network) {
  const index = new Map();
  for (const w of wearables) {
    const e = w.nft?.[network];
    if (e?.collection && (e.status === undefined || e.status === "minted")) index.set(e.collection, w);
  }
  return index;
}

/** Every Core asset the wallets hold → [{ address, collection, wallet }] (one chain read per wallet). */
export async function walletAssets(rpc, wallets) {
  const out = [];
  for (const wallet of wallets) {
    for (const asset of await coreAssetsOwnedBy(rpc, wallet)) out.push({ ...asset, wallet });
  }
  return out;
}

/** Owned, Yeti-compatible wearables → Map(id → { wearable, assets[] }). */
export function ownedWearablesFrom(assets, wearables, network) {
  const index = collectionIndex(wearables, network);
  const owned = new Map();
  for (const asset of assets) {
    const w = asset.collection && index.get(asset.collection);
    if (!w || !fitsCharacter(w)) continue;
    if (!owned.has(w.id)) owned.set(w.id, { wearable: w, assets: [] });
    owned.get(w.id).assets.push(asset);
  }
  return owned;
}

export async function ownedWearables(rpc, wallets, wearables, network) {
  return ownedWearablesFrom(await walletAssets(rpc, wallets), wearables, network);
}

/**
 * Everything Fuchey the wallets own: characters and wearables of every
 * slot (also ones that don't fit the Yeti) →
 * [{ kind: "character"|"wearable", id, name, type, fits, assets[] }],
 * characters first, then wearables in slot order.
 */
export function fucheyCollection(assets, { wearables, characters }, network) {
  const index = new Map();
  for (const c of characters) {
    const e = c.nft?.[network];
    if (e?.collection && (e.status === undefined || e.status === "minted")) {
      index.set(e.collection, { kind: "character", id: c.id, name: c.name, type: "character", fits: c.id === CHARACTER });
    }
  }
  for (const [col, w] of collectionIndex(wearables, network)) {
    index.set(col, { kind: "wearable", id: w.id, name: w.name, type: w.type, fits: fitsCharacter(w) });
  }
  const byId = new Map();
  for (const a of assets) {
    const meta = a.collection && index.get(a.collection);
    if (!meta) continue;
    const key = `${meta.kind}:${meta.id}`;
    if (!byId.has(key)) byId.set(key, { ...meta, assets: [] });
    byId.get(key).assets.push(a);
  }
  const rank = (e) => (e.kind === "character" ? -1 : Math.max(0, SLOTS.indexOf(e.type)));
  return [...byId.values()].sort((a, b) => rank(a) - rank(b) || a.name.localeCompare(b.name));
}

export const fitsCharacter = (w, character = CHARACTER) =>
  Array.isArray(w.compatible_characters) && w.compatible_characters.includes(character);

// ── Visual → pixels on the 96×96 grid ─────────────────────
export function resolveVisual(wearable, character = CHARACTER) {
  const v = wearable?.visual;
  if (!v) return null;
  const o = v.perCharacter?.[character];
  return o ? { ...v, ...o } : v;
}

export const slotOf = (wearable, visual) => {
  const i = SLOTS.indexOf(visual?.layer ?? wearable.type);
  return i < 0 ? SLOTS.indexOf(wearable.type) : i;
};

export function hexToRgb565(hex) {
  const m = /^#?([0-9a-f]{6})$/i.exec(String(hex).trim());
  if (!m) return null;
  const n = parseInt(m[1], 16);
  const r = (n >> 16) & 0xff, g = (n >> 8) & 0xff, b = n & 0xff;
  return ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3);
}

const rgb565 = (r, g, b) => ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3);

/** Pixel-runs side of a visual → [{x, y, w, c565}] with the layer's x/y shift applied. */
export function runsSide(visual, side) {
  const runs = visual[side];
  if (!Array.isArray(runs) || !runs.length) return [];
  const dx = visual.x ?? 0, dy = visual.y ?? 0;
  const out = [];
  for (const r of runs) {
    const c = hexToRgb565(r.c);
    const w = Math.round(r.w ?? 1);
    if (c === null || w <= 0) continue;
    out.push({ x: Math.round(r.x + dx), y: Math.round(r.y + dy), w, c });
  }
  return out;
}

/**
 * RGBA pixels of an image placed on the grid → runs.
 * `pixels`: Uint8ClampedArray (w*h*4) already scaled to grid units,
 * drawn with its top-left at grid (ox, oy). Alpha < 128 is transparent.
 */
export function pixelsToRuns(pixels, w, h, ox, oy) {
  const out = [];
  for (let y = 0; y < h; y++) {
    let x = 0;
    while (x < w) {
      const i = (y * w + x) * 4;
      if (pixels[i + 3] < 128) { x++; continue; }
      const c = rgb565(pixels[i], pixels[i + 1], pixels[i + 2]);
      let n = 1;
      while (x + n < w) {
        const j = (y * w + x + n) * 4;
        if (pixels[j + 3] < 128 || rgb565(pixels[j], pixels[j + 1], pixels[j + 2]) !== c) break;
        n++;
      }
      out.push({ x: ox + x, y: oy + y, w: n, c });
      x += n;
    }
  }
  return out;
}

// Keep runs inside what the file can hold (i8 x/y, u8 w) and near the grid:
// x -16..111, y -64..111 covers hats, halos and held items.
export function clipRuns(runs) {
  const out = [];
  for (const r of runs) {
    if (r.y < -64 || r.y > 111) continue;
    let x0 = Math.max(r.x, -16), x1 = Math.min(r.x + r.w, 112);
    while (x1 - x0 > 0) {
      const w = Math.min(255, x1 - x0);
      out.push({ x: x0, y: r.y, w, c: r.c });
      x0 += w;
    }
  }
  return out;
}

/** The device's FWR1 item file (see firmware WearItem.hpp). */
export function encodeItem({ id, slot, z = 0, front, back }) {
  const idBytes = new TextEncoder().encode(id);
  if (idBytes.length < 2 || idBytes.length > 48) throw new Error(`bad item id ${id}`);
  front = clipRuns(front);
  back = clipRuns(back);
  if (front.length + back.length > 6000) throw new Error(`${id}: too detailed for Fuchey`);
  const size = 4 + 1 + idBytes.length + 1 + 2 + 2 + 2 + (front.length + back.length) * 5;
  const buf = new Uint8Array(size);
  const dv = new DataView(buf.buffer);
  let o = 0;
  buf.set([0x46, 0x57, 0x52, 0x31], o); o += 4;            // "FWR1"
  buf[o++] = idBytes.length;
  buf.set(idBytes, o); o += idBytes.length;
  buf[o++] = slot;
  dv.setInt16(o, Math.max(-32768, Math.min(32767, Math.round(z))), true); o += 2;
  dv.setUint16(o, front.length, true); o += 2;
  dv.setUint16(o, back.length, true); o += 2;
  for (const r of [...front, ...back]) {
    dv.setInt8(o++, r.x);
    dv.setInt8(o++, r.y);
    buf[o++] = r.w;
    dv.setUint16(o, r.c, true); o += 2;
  }
  return buf;
}
