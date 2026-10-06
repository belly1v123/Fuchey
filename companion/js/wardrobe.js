// Fuchey companion — Wardrobe card: marketplace wearables on the device's Yeti.
//
// On connect: read the device's wallet + linked wallets, find the wearables
// they own on-chain, install the missing ones on Fuchey as item files, and
// delete items that are no longer owned. The user picks what Yeti wears.
// Everything here is cosmetic: no signing on Fuchey, no transactions.

import {
  SLOTS, CHARACTER, Catalog, siteNetwork, ownedWearables, resolveVisual, slotOf,
  runsSide, pixelsToRuns, encodeItem,
} from "./wearables.js";
import { verifySignature, isAddress } from "./solana.js";

const $ = (id) => document.getElementById(id);

// The Yeti's base art (same file as the website), 4 screen px per grid px.
const YETI_URL = "img/yeti.png";
// Preview box on the 96×96 grid, with headroom for hats (the website's CHARACTER_VIEW).
const VIEW = { x: -14, y: -19, w: 116, h: 116 };

let ctx = null;          // { device, rpc, log, describe }
let catalog = null;      // Catalog, once config.local.js is loaded
let yetiImg = null;
const wr = {
  info: null,            // hello reply
  network: null,
  syncing: false,
  wallets: [],           // linked wallets (device setting)
  worn: {},              // slot → id
  owned: new Map(),      // id → { wearable, assets, file, item }
  installed: new Map(),  // id → { slot, crc }
};

// ── config (companion/config.local.js, gitignored) ────────
async function loadConfig() {
  if (catalog) return catalog;
  try {
    const mod = await import("../config.local.js");
    catalog = new Catalog(mod.default ?? mod);
  } catch {
    catalog = new Catalog({});
  }
  return catalog;
}

function setMsg(text, ok = false) {
  const el = $("wr-msg");
  el.textContent = text;
  el.className = `small ${ok ? "note" : "error"}`;
  el.classList.toggle("hidden", !text);
}

function setStatus(text) {
  $("wr-status").textContent = text;
}

// ── item files ────────────────────────────────────────────
function loadImage(url) {
  return new Promise((resolve, reject) => {
    const img = new Image();
    img.crossOrigin = "anonymous";
    img.onload = () => resolve(img);
    img.onerror = () => reject(new Error(`couldn't load ${url}`));
    img.src = url;
  });
}

// An uploaded PNG (kind "image") → runs on the grid, sampled like the website
// draws it: top-left at (x, y), `scale` grid px per source px, pixelated.
async function imageRuns(url, visual) {
  if (!url) return [];
  const img = await loadImage(url);
  const s = visual.scale ?? 1;
  const w = Math.max(1, Math.round((visual.width ?? img.naturalWidth) * s));
  const h = Math.max(1, Math.round((visual.height ?? img.naturalHeight) * s));
  const c = document.createElement("canvas");
  c.width = w;
  c.height = h;
  const g = c.getContext("2d", { willReadFrequently: true });
  g.imageSmoothingEnabled = false;
  g.drawImage(img, 0, 0, w, h);
  const px = g.getImageData(0, 0, w, h).data;
  return pixelsToRuns(px, w, h, Math.round(visual.x ?? 0), Math.round(visual.y ?? 0));
}

/** wearable → { item: {id, slot, z, front, back}, file: Uint8Array } for the Yeti. */
async function buildItem(wearable) {
  const visual = resolveVisual(wearable, CHARACTER);
  if (!visual) throw new Error(`${wearable.id} has no visual`);
  let front, back;
  if (visual.kind === "image") {
    front = await imageRuns(visual.image, visual);
    back = await imageRuns(visual.backImage, visual);
  } else {
    front = runsSide(visual, "front");
    back = runsSide(visual, "back");
  }
  const item = { id: wearable.id, slot: slotOf(wearable, visual), z: visual.z ?? 0, front, back };
  return { item, file: encodeItem(item) };
}

// ── preview (canvas over yeti.png) ────────────────────────
function drawRuns(g, runs) {
  for (const r of runs) {
    const c = r.c;
    const R = ((c >> 11) & 0x1f) * 255 / 31, G = ((c >> 5) & 0x3f) * 255 / 63, B = (c & 0x1f) * 255 / 31;
    g.fillStyle = `rgb(${R | 0},${G | 0},${B | 0})`;
    g.fillRect(r.x - VIEW.x, r.y - VIEW.y, r.w, 1);
  }
}

// Same order as Fuchey: every back layer, the Yeti, every front layer.
function drawLook(canvas, items) {
  const g = canvas.getContext("2d");
  canvas.width = VIEW.w;
  canvas.height = VIEW.h;
  g.imageSmoothingEnabled = false;
  g.clearRect(0, 0, VIEW.w, VIEW.h);
  const sorted = [...items].sort((a, b) => a.slot * 1000 + a.z - (b.slot * 1000 + b.z));
  for (const it of sorted) drawRuns(g, it.back);
  if (yetiImg) g.drawImage(yetiImg, -VIEW.x, -VIEW.y, 96, 96);
  for (const it of sorted) drawRuns(g, it.front);
}

function wornItems() {
  const out = [];
  for (const slot of SLOTS) {
    const id = wr.worn[slot];
    const o = id && wr.owned.get(id);
    if (o?.item) out.push(o.item);
  }
  return out;
}

// ── render ────────────────────────────────────────────────
function render() {
  drawLook($("wr-preview"), wornItems());
  const list = $("wr-items");
  list.innerHTML = "";
  if (!wr.owned.size) {
    list.innerHTML = '<p class="muted small">No wearables yet. Items you buy on fuchey.xyz with this ' +
      "Fuchey's wallet (or a linked wallet) show up here.</p>";
  }
  const sorted = [...wr.owned.values()].sort((a, b) => a.item?.slot - b.item?.slot);
  for (const o of sorted) {
    const w = o.wearable;
    const slot = SLOTS[o.item?.slot ?? 0];
    const wearing = wr.worn[slot] === w.id;
    const ready = wr.installed.has(w.id);
    const card = document.createElement("div");
    card.className = `wr-item${wearing ? " wearing" : ""}`;
    const cv = document.createElement("canvas");
    cv.className = "wr-thumb";
    if (o.item) drawLook(cv, [o.item]);
    const text = document.createElement("div");
    text.className = "wr-text";
    text.innerHTML = `<div class="wr-name"></div><div class="muted tiny"></div>`;
    text.firstChild.textContent = w.name;
    text.lastChild.textContent = ready ? slot : `${slot} · ${o.error ? "not installed" : "installing…"}`;
    const btn = document.createElement("button");
    btn.className = wearing ? "ghost small" : "primary small";
    btn.textContent = wearing ? "Take off" : "Wear";
    btn.disabled = !ready || wr.syncing;
    btn.addEventListener("click", () => wear(slot, wearing ? "none" : w.id));
    card.append(cv, text, btn);
    list.append(card);
  }

  const wl = $("wr-wallets");
  wl.innerHTML = "";
  for (const a of wr.wallets) {
    const row = document.createElement("div");
    row.className = "row between wr-wallet";
    const code = document.createElement("code");
    code.textContent = `${a.slice(0, 6)}…${a.slice(-6)}`;
    code.title = a;
    const rm = document.createElement("button");
    rm.className = "ghost small";
    rm.textContent = "Remove";
    rm.disabled = wr.syncing;
    rm.addEventListener("click", () => removeWallet(a));
    row.append(code, rm);
    wl.append(row);
  }
  if (!wr.wallets.length) wl.innerHTML = '<p class="muted tiny">None. Only this Fuchey\'s own wallet is checked.</p>';
  for (const id of ["btn-wr-sync", "btn-wr-website", "btn-wr-phantom"]) $(id).disabled = wr.syncing;
}

// ── sync: chain → device ──────────────────────────────────
async function readDevice() {
  const s = await ctx.device.getSettings();
  wr.wallets = Array.isArray(s.linked_wallets) ? s.linked_wallets : [];
  wr.worn = {};
  for (const slot of SLOTS) {
    const v = s[`wear.${slot}`];
    if (v && v !== "none") wr.worn[slot] = v;
  }
  const list = await ctx.device.itemList();
  wr.installed = new Map((list.items || []).map((i) => [i.id, i]));
  return list;
}

export async function sync() {
  if (wr.syncing || !wr.info) return;
  wr.syncing = true;
  setMsg("");
  render();
  try {
    await loadConfig();
    setStatus("Reading Fuchey…");
    await readDevice();
    if (!catalog.configured) {
      setStatus("");
      setMsg("Marketplace not configured: copy companion/config.example.js to config.local.js and fill in the Supabase URL and anon key.");
      return;
    }
    const wallets = [...new Set([wr.info.pubkey, ...wr.wallets].filter(Boolean))];
    if (!wallets.length) {
      setStatus("This Fuchey has no wallet yet.");
      return;
    }
    setStatus("Loading the marketplace…");
    const wearables = await catalog.wearables();
    setStatus(`Checking what ${wallets.length > 1 ? `${wallets.length} wallets own` : "this wallet owns"} on ${wr.network}…`);
    // A failed chain read throws here, before anything is deleted.
    const owned = await ownedWearables(ctx.rpc(), wallets, wearables, siteNetwork(wr.network));

    for (const [id, o] of owned) {
      try {
        Object.assign(o, await buildItem(o.wearable));
      } catch (e) {
        o.error = e.message;
        ctx.log(`wardrobe: ${id}: ${e.message}`);
      }
    }
    wr.owned = owned;
    render();

    // Install new or changed items.
    const { crc32 } = await import("./serial.js");
    let installed = 0, removed = 0;
    for (const [id, o] of owned) {
      if (!o.file) continue;
      const have = wr.installed.get(id);
      if (have && have.crc === crc32(o.file)) continue;
      setStatus(`Installing ${o.wearable.name} on Fuchey…`);
      try {
        await ctx.device.itemInstall(id, o.file);
        installed++;
      } catch (e) {
        o.error = ctx.describe(e);
        ctx.log(`wardrobe: install ${id} failed: ${o.error}`);
      }
    }
    // Remove items no wallet owns any more (Fuchey also takes them off).
    for (const id of wr.installed.keys()) {
      if (owned.has(id)) continue;
      setStatus(`Removing ${id}…`);
      await ctx.device.itemDelete(id);
      removed++;
    }
    await readDevice();
    const n = owned.size;
    setStatus(`${n} wearable${n === 1 ? "" : "s"} owned` +
      (installed || removed ? ` · ${installed} installed, ${removed} removed` : " · Fuchey is up to date"));
  } catch (e) {
    setStatus("");
    setMsg(`Wardrobe sync failed: ${ctx.describe(e)}`);
  } finally {
    wr.syncing = false;
    render();
  }
}

// ── actions ───────────────────────────────────────────────
async function wear(slot, id) {
  try {
    await ctx.device.setSetting(`wear.${slot}`, id);
    if (id === "none") delete wr.worn[slot]; else wr.worn[slot] = id;
    setMsg("");
  } catch (e) {
    setMsg(ctx.describe(e));
  }
  render();
}

async function useWebsiteLook() {
  setMsg("");
  try {
    await loadConfig();
    if (!catalog.configured) throw new Error("marketplace not configured (config.local.js)");
    const wallets = [...new Set([wr.info.pubkey, ...wr.wallets].filter(Boolean))];
    let slots = null;
    for (const w of wallets) {
      slots = await catalog.loadout(w, CHARACTER);
      if (slots) break;
    }
    if (!slots) { setMsg("No saved Yeti look on fuchey.xyz for these wallets."); return; }
    const skipped = [];
    for (const slot of SLOTS) {
      const id = slots[slot];
      const o = id && wr.owned.get(id);
      const ok = o?.item && SLOTS[o.item.slot] === slot && wr.installed.has(id);
      if (id && !ok) skipped.push(id);
      const want = ok ? id : "none";
      if ((wr.worn[slot] ?? "none") === want) continue;
      await ctx.device.setSetting(`wear.${slot}`, want);
      if (want === "none") delete wr.worn[slot]; else wr.worn[slot] = want;
    }
    setMsg(skipped.length ? `Applied. Skipped (not owned here): ${skipped.join(", ")}` : "Your website look is on Fuchey.", !skipped.length);
  } catch (e) {
    setMsg(ctx.describe(e));
  }
  render();
}

function walletProvider() {
  if (window.phantom?.solana?.isPhantom) return window.phantom.solana;
  if (window.solana?.isPhantom) return window.solana;
  if (window.solflare?.isSolflare) return window.solflare;
  return null;
}

// Prove control of a browser wallet (it signs a plain-text message; nothing
// on-chain), then add its address to Fuchey's linked wallets.
async function addPhantom() {
  setMsg("");
  const provider = walletProvider();
  if (!provider) { setMsg("No Phantom or Solflare wallet in this browser."); return; }
  if (!wr.info?.pubkey) { setMsg("This Fuchey has no wallet yet."); return; }
  try {
    await provider.connect();
    const addr = provider.publicKey?.toString();
    if (!isAddress(addr)) throw new Error("the wallet returned no address");
    if (addr === wr.info.pubkey || wr.wallets.includes(addr)) { setMsg("That wallet is already linked.", true); return; }
    if (wr.wallets.length >= 8) throw new Error("Fuchey can link up to 8 wallets");
    const message = new TextEncoder().encode(`Link ${addr} to Fuchey ${wr.info.pubkey}`);
    const res = await provider.signMessage(message, "utf8");
    const sig = res?.signature ?? res;
    if ((await verifySignature(addr, new Uint8Array(sig), message)) !== "valid") {
      throw new Error("the wallet's signature didn't verify (or this browser can't check Ed25519)");
    }
    await ctx.device.setSetting("linked_wallets", [...wr.wallets, addr]);
    wr.wallets = [...wr.wallets, addr];
    setMsg(`Linked ${addr.slice(0, 4)}…${addr.slice(-4)}.`, true);
    render();
    await sync();
  } catch (e) {
    setMsg(e?.code === 4001 ? "Cancelled in the wallet." : ctx.describe(e));
  }
}

async function removeWallet(addr) {
  try {
    const next = wr.wallets.filter((a) => a !== addr);
    await ctx.device.setSetting("linked_wallets", next);
    wr.wallets = next;
    render();
    await sync();
  } catch (e) {
    setMsg(ctx.describe(e));
  }
}

// ── lifecycle ─────────────────────────────────────────────
export function init(c) {
  ctx = c;
  loadImage(YETI_URL).then((img) => { yetiImg = img; if (wr.info) render(); }).catch(() => {});
  $("btn-wr-sync").addEventListener("click", sync);
  $("btn-wr-website").addEventListener("click", useWebsiteLook);
  $("btn-wr-phantom").addEventListener("click", addPhantom);
}

export function onConnected(info, network) {
  const can = Array.isArray(info.caps) && info.caps.includes("wardrobe_v1");
  $("wardrobe").classList.toggle("hidden", !can);
  if (!can) return;
  wr.info = info;
  wr.network = network;
  wr.owned = new Map();
  render();
  sync();
}

export function onDisconnected() {
  wr.info = null;
  wr.syncing = false;
  $("wardrobe").classList.add("hidden");
}
