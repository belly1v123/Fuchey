// Fuchey companion — Wardrobe card: the character on Fuchey and what it wears.
//
// On connect: read the device's wallet + linked wallets, find the characters
// and wearables they own on-chain, install the missing wearables (resolved for
// the character on Fuchey) as item files, and delete items that are no longer
// owned. The user picks the character (the built-in Yeti, or any owned
// character Fuchey has art for: its "character pack" is sent first) and what
// it wears; each character keeps its own look on the device.
// Everything here is cosmetic: no signing on Fuchey, no transactions.

import {
  SLOTS, CHARACTER, Catalog, siteNetwork, walletAssets, ownedWearablesFrom, ownedCharacters,
  fucheyCollection, resolveVisual, slotOf,
  runsSide, pixelsToRuns, encodeItem,
} from "./wearables.js";
import { verifySignature, isAddress } from "./solana.js";
import { crc32 } from "./serial.js";

const $ = (id) => document.getElementById(id);

// The Yeti's base art (same file as the website); other characters' art
// comes from the marketplace (characters.art.image).
const YETI_URL = "img/yeti.png";
// Character packs (Fuchey's home-screen art for characters other than the
// Yeti), built by scripts/make_character_pack.py: packs/index.json lists them.
const PACKS_URL = "packs/";
// Preview box on the 96×96 grid, with headroom for hats (the website's CHARACTER_VIEW),
// drawn at K canvas px per grid px so 120 px character art stays crisp.
const VIEW = { x: -14, y: -19, w: 116, h: 116 };
const K = 5;
// Developer switch (Settings): offer every published character, owned or not.
const DEV_ALL_KEY = "fuchey.dev.allCharacters";

let ctx = null;          // { device, rpc, log, describe }
let catalog = null;      // Catalog, once the config is loaded
const wr = {
  info: null,            // hello reply
  network: null,
  canSwitch: false,      // firmware has cap "characters_v1"
  syncing: false,
  busyText: "",          // e.g. "Sending Rhino to Fuchey… 40%"
  wallets: [],           // linked wallets (device setting)
  character: CHARACTER,  // the character on Fuchey (device setting)
  worn: {},              // slot → id, for wr.character
  owned: new Map(),      // id → { wearable, assets, file, item }, for wr.character
  collection: null,      // every owned Fuchey NFT (fucheyCollection), null = not read yet
  installed: new Map(),  // id → { slot, crc }, wr.character's items on Fuchey
  filter: "all",         // item grid: "all" or a slot
  chain: null,           // { wearables, characters, assets } from the last chain read
  ownedChars: new Set([CHARACTER]),
  packs: {},             // id → { file } that this page can send (packs/index.json)
  devPacks: new Map(),   // id → { crc } installed on Fuchey (char_list)
  art: new Map(),        // character id → loaded base Image (or null while loading / failed)
};

// ── config: config.local.js (gitignored override), else config.js ──
async function loadConfig() {
  if (catalog) return catalog;
  for (const file of ["../config.local.js", "../config.js"]) {
    try {
      const mod = await import(file);
      catalog = new Catalog(mod.default ?? mod);
      if (catalog.configured) return catalog;
    } catch {}
  }
  catalog = new Catalog({});
  return catalog;
}

async function loadPackIndex() {
  try {
    const res = await fetch(`${PACKS_URL}index.json`, { cache: "no-cache" });
    if (res.ok) wr.packs = await res.json();
  } catch {
    wr.packs = {};
  }
}

const devAll = () => { try { return localStorage.getItem(DEV_ALL_KEY) === "1"; } catch { return false; } };

function setMsg(text, ok = false) {
  const el = $("wr-msg");
  el.textContent = text;
  el.className = `small ${ok ? "note" : "error"}`;
  el.classList.toggle("hidden", !text);
}

function setStatus(text) {
  $("wr-status").textContent = text;
}

// ── characters ────────────────────────────────────────────
function charInfo(id) {
  return wr.chain?.characters.find((c) => c.id === id) ?? { id, name: id === CHARACTER ? "Yeti" : id };
}
const charName = (id) => charInfo(id).name || id;

// Characters to offer: the Yeti, every owned one (greyed out until Fuchey has
// art for it), whatever Fuchey shows now and, with the developer switch,
// every published one Fuchey has art for.
function pickable() {
  const all = wr.chain?.characters ?? [];
  const ids = new Set([CHARACTER, wr.character]);
  for (const c of all) if (wr.ownedChars.has(c.id) || (devAll() && hasArt(c.id))) ids.add(c.id);
  const order = (id) => { const i = all.findIndex((c) => c.id === id); return id === CHARACTER ? -1 : i < 0 ? 1e6 : i; };
  return [...ids].sort((a, b) => order(a) - order(b));
}

// Fuchey can show the Yeti, or a character this page has a pack for.
const hasArt = (id) => id === CHARACTER || Boolean(wr.packs[id]);

function artUrl(id) {
  if (id === CHARACTER) return YETI_URL;
  const c = charInfo(id);
  const img = c.art?.image;
  return img && img !== "yeti" ? img : c.image_url || null;
}

// Base art of a character, loaded once; render() runs again when it arrives.
function baseArt(id) {
  if (wr.art.has(id)) return wr.art.get(id);
  wr.art.set(id, null);
  const url = artUrl(id);
  if (url) loadImage(url).then((img) => { wr.art.set(id, img); if (wr.info) render(); }).catch(() => {});
  return null;
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

/** wearable → { item: {id, slot, z, front, back}, file: Uint8Array } for `character`. */
async function buildItem(wearable, character) {
  const visual = resolveVisual(wearable, character);
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

// ── preview (canvas: back layers, character art, front layers) ──
function drawRuns(g, runs) {
  for (const r of runs) {
    const c = r.c;
    const R = ((c >> 11) & 0x1f) * 255 / 31, G = ((c >> 5) & 0x3f) * 255 / 63, B = (c & 0x1f) * 255 / 31;
    g.fillStyle = `rgb(${R | 0},${G | 0},${B | 0})`;
    g.fillRect((r.x - VIEW.x) * K, (r.y - VIEW.y) * K, r.w * K, K);
  }
}

// Same order as Fuchey: every back layer, the character, every front layer.
// `base` false = the item alone (grid thumbnails).
function drawLook(canvas, items, base = true) {
  const g = canvas.getContext("2d");
  canvas.width = VIEW.w * K;
  canvas.height = VIEW.h * K;
  g.imageSmoothingEnabled = false;
  g.clearRect(0, 0, canvas.width, canvas.height);
  const sorted = [...items].sort((a, b) => a.slot * 1000 + a.z - (b.slot * 1000 + b.z));
  for (const it of sorted) drawRuns(g, it.back);
  const art = base && baseArt(wr.character);
  if (art) g.drawImage(art, -VIEW.x * K, -VIEW.y * K, 96 * K, 96 * K);
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
const slotName = (slot) => slot[0].toUpperCase() + slot.slice(1);

// One button per character Fuchey can show; the active one is pressed.
function renderCharacters() {
  const box = $("wr-chars");
  const ids = wr.canSwitch ? pickable() : [];
  box.classList.toggle("hidden", ids.length < 2);
  box.innerHTML = "";
  for (const id of ids) {
    const active = id === wr.character;
    const b = document.createElement("button");
    b.className = `wr-char${active ? " active" : ""}`;
    b.setAttribute("aria-pressed", String(active));
    b.disabled = wr.syncing || (!active && !hasArt(id));
    const owned = wr.ownedChars.has(id);
    b.title = active ? `${charName(id)} is on Fuchey`
      : !hasArt(id) ? `No Fuchey art for ${charName(id)} yet`
      : `Show ${charName(id)} on Fuchey${owned ? "" : " (developer: not owned)"}`;
    const img = document.createElement("img");
    img.alt = "";
    const url = artUrl(id);
    if (url) img.src = url;
    const name = document.createElement("span");
    name.textContent = charName(id);
    b.append(img, name);
    if (!owned && id !== wr.character) {
      const tag = document.createElement("span");
      tag.className = "wr-char-tag";
      tag.textContent = "dev";
      b.append(tag);
    }
    b.addEventListener("click", () => { if (!active) switchCharacter(id); });
    box.append(b);
  }
}

// What the character has on, one chip per slot; tapping a chip takes it off.
function renderWorn() {
  const box = $("wr-worn");
  box.innerHTML = "";
  for (const slot of SLOTS) {
    const id = wr.worn[slot];
    const o = id && wr.owned.get(id);
    if (!o) continue;
    const chip = document.createElement("button");
    chip.className = "wr-chip";
    chip.title = `Take off ${o.wearable.name}`;
    chip.disabled = wr.syncing;
    chip.innerHTML = '<span class="wr-chip-slot"></span><span></span><span aria-hidden="true">×</span>';
    chip.children[0].textContent = slotName(slot);
    chip.children[1].textContent = o.wearable.name;
    chip.addEventListener("click", () => wear(slot, "none"));
    box.append(chip);
  }
  if (!box.children.length) box.innerHTML = '<p class="muted small center">Nothing on. Tap an item below to wear it.</p>';
}

// "All" plus one filter per slot that has items.
function renderFilters(items) {
  const box = $("wr-filters");
  box.innerHTML = "";
  const slots = SLOTS.filter((s) => items.some((o) => SLOTS[o.item?.slot ?? 0] === s));
  if (wr.filter !== "all" && !slots.includes(wr.filter)) wr.filter = "all";
  box.classList.toggle("hidden", slots.length < 2);
  for (const s of ["all", ...slots]) {
    const b = document.createElement("button");
    b.className = `wr-filter${wr.filter === s ? " active" : ""}`;
    b.setAttribute("aria-pressed", String(wr.filter === s));
    b.textContent = s === "all" ? `All ${items.length}` : slotName(s);
    b.addEventListener("click", () => { wr.filter = s; render(); });
    box.append(b);
  }
}

function itemTile(o) {
  const w = o.wearable;
  const slot = SLOTS[o.item?.slot ?? 0];
  const wearing = wr.worn[slot] === w.id;
  const ready = wr.installed.has(w.id);
  const tile = document.createElement("button");
  tile.className = `wr-tile${wearing ? " wearing" : ""}`;
  tile.disabled = !ready || wr.syncing;
  tile.setAttribute("aria-pressed", String(wearing));
  tile.title = !ready ? (o.error ? `Not on Fuchey: ${o.error}` : "Installing on Fuchey…")
    : wearing ? `Take off ${w.name}` : `Wear ${w.name}`;
  const cv = document.createElement("canvas");
  cv.className = "wr-thumb";
  if (o.item) drawLook(cv, [o.item]);
  const name = document.createElement("div");
  name.className = "wr-name";
  name.textContent = w.name;
  const tag = document.createElement("div");
  tag.className = "muted tiny";
  tag.textContent = slotName(slot);
  tile.append(cv, name, tag);
  const badge = wearing ? "On" : !ready ? (o.error ? "Not on Fuchey" : "Installing…") : "";
  if (badge) {
    const b = document.createElement("span");
    b.className = `wr-badge${o.error ? " err" : ""}`;
    b.textContent = badge;
    tile.append(b);
  }
  tile.addEventListener("click", () => wear(slot, wearing ? "none" : w.id));
  return tile;
}

function render() {
  const name = charName(wr.character);
  $("wr-title").textContent = name;
  $("wr-preview").setAttribute("aria-label", `What ${name} is wearing`);
  renderCharacters();
  drawLook($("wr-preview"), wornItems());
  renderWorn();
  const items = [...wr.owned.values()].sort((a, b) => a.item?.slot - b.item?.slot);
  renderFilters(items);
  const list = $("wr-items");
  list.innerHTML = "";
  if (!items.length) {
    const p = document.createElement("p");
    p.className = "muted small wr-none";
    p.textContent = `No wearables for the ${name} yet. Items you buy on fuchey.xyz with this ` +
      "Fuchey's wallet (or a linked wallet) that fit it show up here.";
    list.append(p);
  }
  for (const o of items) {
    if (wr.filter === "all" || SLOTS[o.item?.slot ?? 0] === wr.filter) list.append(itemTile(o));
  }

  renderCollection();

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
  $("wr-wallet-count").textContent = wr.wallets.length || "";
  for (const id of ["btn-wr-sync", "btn-wr-website", "btn-wr-phantom"]) $(id).disabled = wr.syncing;
}

// ── My Fuchey collection (everything owned, not just what Fuchey can wear) ──
const walletLabel = (a) => (a === wr.info?.pubkey ? "this Fuchey" : `${a.slice(0, 4)}…${a.slice(-4)}`);

function collectionStatus(e) {
  if (e.kind === "character") {
    if (e.id === wr.character) return "Character · on Fuchey now";
    if (!hasArt(e.id)) return "Character · coming to Fuchey soon";
    return wr.canSwitch ? "Character · pick it above to show it on Fuchey" : "Character · needs a Fuchey firmware update";
  }
  const name = charName(wr.character);
  if (!e.fits) return `${e.type} · doesn't fit the ${name}`;
  if (wr.installed.has(e.id)) {
    return wr.worn[e.type] === e.id ? `${e.type} · on Fuchey, wearing` : `${e.type} · on Fuchey, tap Wear`;
  }
  const err = wr.owned.get(e.id)?.error;
  return `${e.type} · ${err ? `not installed: ${err}` : "not on Fuchey yet"}`;
}

function renderCollection() {
  const card = $("collection");
  card.classList.toggle("hidden", wr.collection === null);
  if (wr.collection === null) return;
  const list = $("col-list");
  list.innerHTML = "";
  const n = wr.collection.reduce((sum, e) => sum + e.assets.length, 0);
  $("col-count").textContent = n || "";
  $("col-summary").textContent = n
    ? `${n} Fuchey NFT${n === 1 ? "" : "s"} on ${wr.network}`
    : `No Fuchey NFTs on ${wr.network} in these wallets yet.`;
  for (const e of wr.collection) {
    const row = document.createElement("div");
    row.className = "col-row";
    const left = document.createElement("div");
    left.innerHTML = '<div class="wr-name"></div><div class="muted tiny"></div>';
    left.firstChild.textContent = e.assets.length > 1 ? `${e.name} ×${e.assets.length}` : e.name;
    left.lastChild.textContent = collectionStatus(e);
    const who = document.createElement("code");
    who.className = "tiny";
    who.textContent = [...new Set(e.assets.map((a) => walletLabel(a.wallet)))].join(", ");
    row.append(left, who);
    list.append(row);
  }
}

// ── sync: chain → device ──────────────────────────────────
async function readDevice() {
  const s = await ctx.device.getSettings();
  wr.wallets = Array.isArray(s.linked_wallets) ? s.linked_wallets : [];
  wr.character = (wr.canSwitch && s.character) || CHARACTER;
  wr.worn = {};
  for (const slot of SLOTS) {
    const v = s[`wear.${slot}`];
    if (v && v !== "none") wr.worn[slot] = v;
  }
  const list = await ctx.device.itemList(wr.character);
  wr.installed = new Map((list.items || []).map((i) => [i.id, i]));
  if (wr.canSwitch) {
    const packs = await ctx.device.charList();
    wr.devPacks = new Map((packs.chars || []).map((c) => [c.id, c]));
  }
  return list;
}

// Reads the chain again unless `chain` is false and an earlier read is cached
// (switching character only re-installs that character's items).
export async function sync({ chain = true } = {}) {
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
    const network = siteNetwork(wr.network);
    if (chain || !wr.chain) {
      setStatus("Loading the marketplace…");
      const wearables = await catalog.wearables();
      const characters = await catalog.characters().catch(() => []);
      setStatus(`Checking what ${wallets.length > 1 ? `${wallets.length} wallets own` : "this wallet owns"} on ${wr.network}…`);
      // A failed chain read throws here, before anything is deleted.
      const assets = await walletAssets(ctx.rpc(), wallets);
      wr.chain = { wearables, characters, assets };
      wr.ownedChars = ownedCharacters(assets, characters, network);
    }
    const { wearables, characters, assets } = wr.chain;
    const character = wr.character;
    const owned = ownedWearablesFrom(assets, wearables, network, character);
    wr.collection = fucheyCollection(assets, { wearables, characters }, network, character);

    for (const [id, o] of owned) {
      try {
        Object.assign(o, await buildItem(o.wearable, character));
      } catch (e) {
        o.error = e.message;
        ctx.log(`wardrobe: ${id}: ${e.message}`);
      }
    }
    wr.owned = owned;
    render();

    // Install new or changed items (for the character on Fuchey).
    let installed = 0, removed = 0, failed = 0;
    for (const [id, o] of owned) {
      if (!o.file) continue;
      const have = wr.installed.get(id);
      if (have && have.crc === crc32(o.file)) continue;
      setStatus(`Installing ${o.wearable.name} on Fuchey…`);
      try {
        await ctx.device.itemInstall(id, o.file, null, character);
        installed++;
        // A new item goes straight on when nothing is worn in its slot.
        const slot = SLOTS[o.item.slot];
        if (!have && !wr.worn[slot]) {
          await ctx.device.setSetting(`wear.${slot}`, id);
          wr.worn[slot] = id;
        }
      } catch (e) {
        o.error = ctx.describe(e);
        failed++;
        ctx.log(`wardrobe: install ${id} failed: ${o.error}`);
      }
    }
    // Remove items no wallet owns any more (Fuchey also takes them off).
    for (const id of wr.installed.keys()) {
      if (owned.has(id)) continue;
      setStatus(`Removing ${id}…`);
      await ctx.device.itemDelete(id, character);
      removed++;
    }
    await readDevice();
    const n = owned.size;
    const notOnFuchey = [...owned.keys()].filter((id) => !wr.installed.has(id)).length;
    setStatus(`${charName(character)} · ${n} wearable${n === 1 ? "" : "s"} owned` +
      (installed || removed ? ` · ${installed} installed, ${removed} removed` : "") +
      (notOnFuchey ? ` · ${notOnFuchey} not on Fuchey yet` : (installed || removed ? "" : " · Fuchey is up to date")));
    if (failed) setMsg(`${failed} item${failed === 1 ? "" : "s"} couldn't be installed — see the list below. Press Sync to retry.`);
  } catch (e) {
    setStatus("");
    setMsg(`Wardrobe sync failed: ${ctx.describe(e)}`);
  } finally {
    wr.syncing = false;
    render();
  }
}

// ── actions ───────────────────────────────────────────────
// Puts `id` on Fuchey's home screen: sends its pack first if Fuchey doesn't
// have this version, then installs the items that fit it.
async function switchCharacter(id) {
  if (wr.syncing || !wr.canSwitch || id === wr.character) return;
  const name = charName(id);
  wr.syncing = true;
  setMsg("");
  render();
  try {
    if (id !== CHARACTER) {
      const entry = wr.packs[id];
      if (!entry) throw new Error(`no Fuchey art for ${name} yet`);
      setStatus(`Loading ${name}…`);
      const res = await fetch(`${PACKS_URL}${entry.file}`, { cache: "no-cache" });
      if (!res.ok) throw new Error(`couldn't load ${name}'s pack (HTTP ${res.status})`);
      const pack = new Uint8Array(await res.arrayBuffer());
      if (wr.devPacks.get(id)?.crc !== crc32(pack)) {
        await ctx.device.charInstall(id, pack, (sent, total) =>
          setStatus(`Sending ${name} to Fuchey… ${Math.floor((sent * 100) / total)}%`));
      }
    }
    setStatus(`Switching to ${name}…`);
    await ctx.device.setSetting("character", id);
    wr.character = id;
    wr.owned = new Map();
    wr.installed = new Map();
    wr.worn = {};
  } catch (e) {
    setStatus("");
    setMsg(`Couldn't switch to ${name}: ${ctx.describe(e)}`);
    return;
  } finally {
    wr.syncing = false;
    render();
  }
  await sync({ chain: false });
}

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
  const name = charName(wr.character);
  try {
    await loadConfig();
    if (!catalog.configured) throw new Error("marketplace not configured (config.local.js)");
    const wallets = [...new Set([wr.info.pubkey, ...wr.wallets].filter(Boolean))];
    let slots = null;
    for (const w of wallets) {
      slots = await catalog.loadout(w, wr.character);
      if (slots) break;
    }
    if (!slots) { setMsg(`No saved ${name} look on fuchey.xyz for these wallets.`); return; }
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
    setMsg(skipped.length ? `Applied. Skipped (not owned here): ${skipped.join(", ")}` : `Your website look is on Fuchey's ${name}.`, !skipped.length);
  } catch (e) {
    setMsg(ctx.describe(e));
  }
  render();
}

// ── Browser wallets ───────────────────────────────────────
// Detected when "Add a wallet" is pressed: wallets that announce themselves
// through the Wallet Standard (Phantom, Solflare, Backpack, …), plus the
// older window.phantom / window.solflare objects as a fallback.
const standardWallets = new Set();
function listenForStandardWallets() {
  const api = { register: (...ws) => { ws.forEach((w) => standardWallets.add(w)); return () => {}; } };
  window.addEventListener("wallet-standard:register-wallet", (e) => { try { e.detail?.(api); } catch {} });
  try { window.dispatchEvent(new CustomEvent("wallet-standard:app-ready", { detail: api })); } catch {}
}

const bytesOf = (s) => (s instanceof Uint8Array ? s : new Uint8Array(s));

// Each wallet → { name, icon, connect() → address, sign(message) → signature bytes }.
function detectWallets() {
  const found = [];
  for (const w of standardWallets) {
    const connect = w.features?.["standard:connect"];
    const signer = w.features?.["solana:signMessage"];
    const solana = (w.chains ?? []).some((c) => String(c).startsWith("solana:"));
    if (!connect || !signer || !solana) continue;
    let account = null;
    found.push({
      name: w.name, icon: w.icon,
      async connect() {
        const { accounts } = await connect.connect();
        account = (accounts ?? w.accounts ?? [])[0];
        return account?.address;
      },
      async sign(message) {
        const [out] = await signer.signMessage({ account, message });
        return bytesOf(out.signature);
      },
    });
  }
  const legacy = [
    ["Phantom", window.phantom?.solana?.isPhantom ? window.phantom.solana : window.solana?.isPhantom ? window.solana : null],
    ["Solflare", window.solflare?.isSolflare ? window.solflare : window.solana?.isSolflare ? window.solana : null],
  ];
  for (const [name, p] of legacy) {
    if (!p || found.some((f) => f.name.toLowerCase() === name.toLowerCase())) continue;
    found.push({
      name, icon: null,
      async connect() { await p.connect(); return p.publicKey?.toString(); },
      // Phantom returns { signature }, older Solflare a bare Uint8Array.
      async sign(message) { const r = await p.signMessage(message, "utf8"); return bytesOf(r?.signature ?? r); },
    });
  }
  return found;
}

// "Add a wallet": one wallet → use it; several → show a picker; none → say so.
function addWallet() {
  setMsg("");
  const pick = $("wr-pick");
  pick.classList.add("hidden");
  pick.innerHTML = "";
  const wallets = detectWallets();
  if (!wallets.length) { setMsg("No Solana wallet found in this browser."); return; }
  if (wallets.length === 1) { linkWallet(wallets[0]); return; }
  const label = document.createElement("span");
  label.className = "muted small";
  label.textContent = "Choose a wallet:";
  pick.append(label);
  for (const w of wallets) {
    const b = document.createElement("button");
    b.className = "ghost small wr-wallet-btn";
    if (w.icon) {
      const img = document.createElement("img");
      img.src = w.icon;
      img.alt = "";
      b.append(img);
    }
    b.append(w.name);
    b.addEventListener("click", () => { pick.classList.add("hidden"); linkWallet(w); });
    pick.append(b);
  }
  pick.classList.remove("hidden");
}

// Prove control of a browser wallet (it signs a plain-text message; nothing
// on-chain), then add its address to Fuchey's linked wallets.
async function linkWallet(wallet) {
  setMsg("");
  if (!wr.info?.pubkey) { setMsg("This Fuchey has no wallet yet."); return; }
  try {
    const addr = await wallet.connect();
    if (!isAddress(addr)) throw new Error("the wallet returned no address");
    if (addr === wr.info.pubkey || wr.wallets.includes(addr)) { setMsg("That wallet is already linked.", true); return; }
    if (wr.wallets.length >= 8) throw new Error("Fuchey can link up to 8 wallets");
    const message = new TextEncoder().encode(`Link ${addr} to Fuchey ${wr.info.pubkey}`);
    const sig = await wallet.sign(message);
    if ((await verifySignature(addr, sig, message)) !== "valid") {
      throw new Error("the wallet's signature didn't verify (or this browser can't check Ed25519)");
    }
    await ctx.device.setSetting("linked_wallets", [...wr.wallets, addr]);
    wr.wallets = [...wr.wallets, addr];
    render();
    await sync();
    setMsg(`Linked ${addr.slice(0, 4)}…${addr.slice(-4)} (${wallet.name}).`, true);
  } catch (e) {
    setMsg(e?.code === 4001 || /reject|cancel/i.test(e?.message ?? "") ? "Cancelled in the wallet." : ctx.describe(e));
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
  baseArt(CHARACTER);
  loadPackIndex();
  $("btn-wr-sync").addEventListener("click", () => sync());
  $("btn-wr-website").addEventListener("click", useWebsiteLook);
  listenForStandardWallets();
  $("btn-wr-phantom").addEventListener("click", addWallet);
  const dev = $("dev-all-chars");
  dev.checked = devAll();
  dev.addEventListener("change", () => {
    try { dev.checked ? localStorage.setItem(DEV_ALL_KEY, "1") : localStorage.removeItem(DEV_ALL_KEY); } catch {}
    if (wr.info) render();
  });
}

export function onConnected(info, network) {
  const can = Array.isArray(info.caps) && info.caps.includes("wardrobe_v1");
  $("wardrobe").classList.toggle("hidden", !can);
  $("wardrobe-empty").classList.toggle("hidden", can);
  $("wardrobe-empty").textContent = "This Fuchey's firmware has no wardrobe yet.";
  if (!can) return;
  wr.info = info;
  wr.network = network;
  wr.canSwitch = info.caps.includes("characters_v1");
  wr.character = CHARACTER;
  wr.chain = null;
  wr.ownedChars = new Set([CHARACTER]);
  wr.devPacks = new Map();
  wr.owned = new Map();
  wr.collection = null;
  render();
  sync();
}

export function onDisconnected() {
  wr.info = null;
  wr.syncing = false;
  $("wardrobe").classList.add("hidden");
  $("collection").classList.add("hidden");
  $("wardrobe-empty").classList.remove("hidden");
  $("wardrobe-empty").textContent = "Connect your Fuchey to dress up its character.";
}
