// Fuchey companion — UI controller.
import { FucheyDevice, DeviceError } from "./serial.js";
import {
  NETWORKS, SOL_DECIMALS, USDC_DECIMALS, BASE_FEE_LAMPORTS,
  Rpc, isAddress, parseUnits, formatUnits,
  buildSolTransfer, buildTokenTransferChecked, assembleTransaction, verifySignature,
  buildTokenTransferCheckedWithCreate, findAssociatedTokenAddress, TOKEN_ACCOUNT_RENT_LAMPORTS,
} from "./solana.js";
import { qrSvg } from "./qr.js";

const $ = (id) => document.getElementById(id);

// Ask "are you sure?" in the page before a send this large reaches the device
// (smallest units: lamports / USDC micro-units).
const LARGE_AMOUNT = { SOL: 500_000_000n, USDC: 50_000_000n };   // 0.5 SOL, 50 USDC
const device = new FucheyDevice();

const state = {
  info: null,        // hello reply
  network: null,     // "devnet" | "mainnet" (always from the device)
  balances: { sol: null, usdc: null },
  sending: false,
};

// ── settings (per-browser) ────────────────────────────────
const settings = {
  get(net) {
    try { return localStorage.getItem(`fuchey.rpc.${net}`) || NETWORKS[net].rpc; }
    catch { return NETWORKS[net].rpc; }
  },
  set(net, url) {
    try { url ? localStorage.setItem(`fuchey.rpc.${net}`, url) : localStorage.removeItem(`fuchey.rpc.${net}`); }
    catch { /* storage unavailable */ }
  },
};
const rpc = () => new Rpc(settings.get(state.network));

// ── error text ────────────────────────────────────────────
const DEVICE_ERRORS = {
  rejected: "You rejected it on Fuchey. Nothing was signed or sent.",
  cancelled: "Request cancelled. Nothing was signed or sent.",
  timeout: "No answer on Fuchey within 30 s, so the request expired. Nothing was signed — press Send to try again.",
  busy: "Fuchey is already showing another request. Approve or reject it on the device first.",
  network_mismatch: "This page and Fuchey disagree on the network (devnet/mainnet). Disconnect and connect again.",
  unsupported_tx: "Fuchey refused this transaction shape.",
  no_wallet: "Fuchey has no wallet yet. Create or import one on the device console.",
  disconnected: "Fuchey was disconnected (unplugged or reset). Nothing was signed. Plug it in, wait for the home screen, then Connect.",
  no_reply: "Fuchey did not answer. It may still be starting up — wait for its home screen and press Connect again. If it keeps happening, update the firmware.",
};

// Errors from the browser (Web Serial) and the RPC, in plain words.
function describeBrowserError(err) {
  const name = err?.name || "";
  const msg = err?.message || String(err);
  if (name === "NetworkError" || /failed to open serial port/i.test(msg)) {
    return "The USB port is busy. Close anything else using Fuchey (pio monitor, fuchey_usb.py, another tab with this page), then press Connect again.";
  }
  if (name === "InvalidStateError") return "The USB port is already open. Reload this page and connect again.";
  if (name === "SecurityError") return "The browser blocked USB access. Open this page on http://localhost (or HTTPS) in Chrome or Edge.";
  if (/failed to fetch|networkerror when attempting/i.test(msg)) {
    return "Can't reach the Solana RPC. Check your internet connection, or the RPC URL in Settings.";
  }
  if (/RPC HTTP 429/.test(msg)) return "The public RPC is rate-limiting this page. Wait a minute, or set your own RPC URL in Settings.";
  if (/insufficient (lamports|funds)|no record of a prior credit/i.test(msg)) {
    return "Not enough SOL to cover this transfer and the network fee.";
  }
  return msg;
}

function describe(err) {
  if (err instanceof DeviceError) {
    const base = DEVICE_ERRORS[err.code] || `Device error: ${err.code}`;
    return err.detail && err.code === "unsupported_tx" ? `${base} (${err.detail})` : base;
  }
  return describeBrowserError(err);
}

// ── log panel ─────────────────────────────────────────────
const logEl = $("log");
function log(line) {
  logEl.textContent += line + "\n";
  if (logEl.textContent.length > 40000) logEl.textContent = logEl.textContent.slice(-30000);
  logEl.scrollTop = logEl.scrollHeight;
}
device.addEventListener("log", (e) => log(e.detail));

// ── connection ────────────────────────────────────────────
async function connect() {
  $("btn-connect").disabled = true;
  $("plug-hint").classList.add("hidden");
  $("result").classList.add("hidden");
  try {
    await device.connect();
    log("— port opened, saying hello…");
    const info = await device.hello();
    if (info.proto !== 1) throw new Error(`Unsupported protocol v${info.proto}; update the page or firmware.`);
    state.info = info;
    state.network = info.network;
    renderConnected();
    await refreshBalances();
  } catch (e) {
    if (e?.name === "NotFoundError") { $("btn-connect").disabled = false; return; }  // picker closed
    showResult(false, `Could not connect: ${describe(e)}`);
    await device.disconnect();
  } finally {
    $("btn-connect").disabled = false;
  }
}

device.addEventListener("disconnect", (e) => {
  const wasConnected = state.info !== null;
  state.info = null;
  state.network = null;
  closeModal();
  renderDisconnected();
  // A sign in progress reports its own error; otherwise explain the unplug.
  if (wasConnected && !state.sending && e.detail !== "closed") {
    showResult(false, DEVICE_ERRORS.disconnected);
  }
  log(`— disconnected (${e.detail})`);
});

// A previously allowed Fuchey was plugged back in. Don't auto-connect
// (opening the port can restart it) — just say it's there.
navigator.serial?.addEventListener("connect", () => {
  if (!state.info) $("plug-hint").classList.remove("hidden");
});

function renderConnected() {
  const { info, network } = state;
  $("intro").classList.add("hidden");
  $("btn-connect").classList.add("hidden");
  $("btn-disconnect").classList.remove("hidden");
  const badge = $("net-badge");
  badge.textContent = network.toUpperCase();
  badge.className = `badge ${network}`;
  $("mainnet-banner").classList.toggle("hidden", network !== "mainnet");
  $("wallet").classList.remove("hidden");
  $("device-info").textContent = `Firmware ${info.fw} · protocol v${info.proto} · max tx ${info.max_tx} bytes`;
  $("receive").classList.add("hidden");
  $("qr").innerHTML = "";
  const canShow = info.has_wallet && Array.isArray(info.caps) && info.caps.includes("show_address");
  $("btn-receive").classList.toggle("hidden", !info.has_wallet);
  $("btn-show-device").classList.toggle("hidden", !canShow);
  if (info.has_wallet) {
    $("address").textContent = info.pubkey;
    $("send").classList.remove("hidden");
  } else {
    $("address").textContent = "No wallet on this Fuchey yet (use wallet_create / wallet_import on its console).";
    $("send").classList.add("hidden");
  }
}

function renderDisconnected() {
  $("intro").classList.remove("hidden");
  $("btn-connect").classList.remove("hidden");
  $("btn-disconnect").classList.add("hidden");
  $("net-badge").className = "badge hidden";
  $("mainnet-banner").classList.add("hidden");
  $("wallet").classList.add("hidden");
  $("send").classList.add("hidden");
}

async function refreshBalances() {
  if (!state.info?.has_wallet) return;
  const r = rpc();
  const owner = state.info.pubkey;
  $("bal-sol").textContent = $("bal-usdc").textContent = "…";
  try {
    const [sol, usdc] = await Promise.all([
      r.getBalance(owner),
      r.getTokenAccount(owner, NETWORKS[state.network].usdcMint),
    ]);
    state.balances = { sol, usdc: usdc ? usdc.amount : 0n };
    $("bal-sol").textContent = formatUnits(sol, SOL_DECIMALS);
    $("bal-usdc").textContent = formatUnits(state.balances.usdc, USDC_DECIMALS);
  } catch (e) {
    $("bal-sol").textContent = $("bal-usdc").textContent = "error";
    log(`balance error: ${describe(e)}`);
  }
}

// ── send ──────────────────────────────────────────────────
function sendNote(msg) {
  const el = $("send-note");
  el.textContent = msg;
  el.classList.toggle("hidden", !msg);
}

function sendError(msg) {
  const el = $("send-error");
  el.textContent = msg;
  el.classList.toggle("hidden", !msg);
}

let txSig = null;   // set once broadcast — a later error must keep its link

async function onSend(ev) {
  ev.preventDefault();
  if (state.sending) return;
  sendError("");
  sendNote("");
  $("result").classList.add("hidden");

  const asset = $("asset").value;
  const to = $("recipient").value.trim();
  const from = state.info.pubkey;
  const network = state.network;
  const decimals = asset === "SOL" ? SOL_DECIMALS : USDC_DECIMALS;

  let amount;
  try {
    if (!isAddress(to)) throw new Error("Recipient is not a valid Solana address.");
    if (to === from) throw new Error("Recipient is this Fuchey's own address.");
    amount = parseUnits($("amount").value, decimals);
    if (amount <= 0n) throw new Error("Amount must be greater than 0.");
  } catch (e) {
    sendError(e.message);
    return;
  }

  if (amount > LARGE_AMOUNT[asset]) {
    const ok = window.confirm(
      `Large amount: ${formatUnits(amount, decimals)} ${asset} on ${network.toUpperCase()}
` +
      `to ${to}

Is this amount correct? (Check for an extra zero.)`,
    );
    if (!ok) {
      sendError("Cancelled — nothing was sent to Fuchey.");
      return;
    }
  }

  state.sending = true;
  $("btn-send").disabled = true;
  try {
    const r = rpc();
    let message;
    let shownTo = to;
    txSig = null;
    let createsAccount = false;

    if (asset === "SOL") {
      const bal = await r.getBalance(from);
      if (bal < amount + BASE_FEE_LAMPORTS) throw new Error(`Insufficient SOL: have ${formatUnits(bal, 9)}.`);
      // A brand-new account must receive at least the rent-exempt minimum.
      const [toBal, rentMin] = await Promise.all([r.getBalance(to), r.getMinimumRentExempt(0)]);
      if (toBal === 0n && amount < rentMin) {
        throw new Error(`This address is empty; Solana requires at least ${formatUnits(rentMin, 9)} SOL for a new account.`);
      }
      const blockhash = await r.getLatestBlockhash();
      log(`— blockhash ${blockhash}`);
      message = buildSolTransfer({ from, to, lamports: amount, blockhash });
    } else {
      const mint = NETWORKS[network].usdcMint;
      const [src, dst, solBal] = await Promise.all([
        r.getTokenAccount(from, mint), r.getTokenAccount(to, mint), r.getBalance(from),
      ]);
      if (!src || src.amount < amount) {
        throw new Error(`Insufficient USDC: have ${formatUnits(src ? src.amount : 0n, 6)}.`);
      }
      const blockhash = await r.getLatestBlockhash();
      log(`— blockhash ${blockhash}`);
      if (dst) {
        if (solBal < BASE_FEE_LAMPORTS) throw new Error("Not enough SOL to pay the network fee.");
        shownTo = dst.address;   // the device shows the destination token account
        message = buildTokenTransferChecked({
          owner: from, source: src.address, destination: dst.address, mint,
          amount, decimals: USDC_DECIMALS, blockhash,
        });
      } else {
        // No USDC account yet: open the recipient's associated token account
        // in the same transaction (this wallet pays the rent).
        if (!(state.info.caps || []).includes("usdc_create_ata")) {
          throw new Error("Recipient has no USDC account yet, and this Fuchey firmware can't create one. Update the firmware.");
        }
        if (solBal < BASE_FEE_LAMPORTS + TOKEN_ACCOUNT_RENT_LAMPORTS) {
          throw new Error(`Opening the recipient's USDC account needs ${formatUnits(TOKEN_ACCOUNT_RENT_LAMPORTS + BASE_FEE_LAMPORTS, 9)} SOL; you have ${formatUnits(solBal, 9)}.`);
        }
        const ata = await findAssociatedTokenAddress(to, mint);
        log(`— recipient has no USDC account; creating ${ata}`);
        createsAccount = true;   // the device shows the recipient wallet
        sendNote(`This recipient has no USDC account yet. This send also opens one for them — ` +
                 `you pay ${formatUnits(TOKEN_ACCOUNT_RENT_LAMPORTS, 9)} SOL rent, once.`);
        message = buildTokenTransferCheckedWithCreate({
          owner: from, source: src.address, recipient: to, ata, mint,
          amount, decimals: USDC_DECIMALS, blockhash,
        });
      }
    }

    openModal({
      asset, amount: formatUnits(amount, decimals), to: shownTo,
      owner: asset === "USDC" && !createsAccount ? to : null, network, createsAccount,
    });

    log(`— sign_tx: ${formatUnits(amount, decimals)} ${asset} → ${shownTo} (${network}); tap B1 on Fuchey`);
    const signature = await device.signMessage(message, network, (evt) => {
      if (evt.event === "awaiting_confirmation") {
        startCountdown(evt.timeout_ms || 30000);
        $("modal-status").textContent = `Fuchey shows ${evt.amount} ${evt.asset} → ${evt.to.slice(0, 6)}…${evt.to.slice(-6)}, fee ${evt.fee} SOL`;
      }
    });
    log("— signed on Fuchey");
    $("modal-status").textContent = "Signed on Fuchey. Verifying…";

    const check = await verifySignature(from, signature, message);
    if (check === "invalid") throw new Error("Signature from the device did not verify — not broadcasting.");
    if (check === "unsupported") log("note: browser lacks Ed25519 WebCrypto; relying on RPC verification");

    log("— broadcasting…");
    $("modal-status").textContent = "Broadcasting…";
    txSig = await r.sendTransaction(assembleTransaction(signature, message));
    log(`— sent: ${txSig}`);
    closeModal();
    const opened = createsAccount
      ? ` and opened the recipient's USDC account (${formatUnits(TOKEN_ACCOUNT_RENT_LAMPORTS, 9)} SOL rent)`
      : "";
    showResult(true, `Sent ${formatUnits(amount, decimals)} ${asset}${opened}. Waiting for confirmation…`, txSig);
    await r.confirm(txSig);
    showResult(true, `Confirmed: ${formatUnits(amount, decimals)} ${asset} sent${opened}.`, txSig);
    $("amount").value = "";
    refreshBalances();
  } catch (e) {
    let msg = describe(e);
    if (/blockhash not found/i.test(msg)) {
      msg += " — the transaction expired before it reached the network (devnet blockhashes last ~35 s). Nothing was sent; press Send again and approve on Fuchey promptly.";
    }
    if (txSig && /^Transaction failed/.test(msg)) {
      msg = `It reached the network but failed on-chain (${msg.replace(/^Transaction failed: /, "")}). Funds did not move; only the fee was charged.`;
    } else if (txSig) {
      // Already broadcast: it may still land, so keep the Explorer link.
      msg = `Sent, but not confirmed yet: ${msg} Check the Explorer link before trying again.`;
    }
    log(`— send failed: ${msg}`);
    closeModal();
    showResult(false, msg, txSig);
  } finally {
    state.sending = false;
    $("btn-send").disabled = false;
  }
}

// ── modal / result ────────────────────────────────────────
let countdownTimer = null;

function openModal({ asset, amount, to, owner, network, createsAccount = false }) {
  const dl = $("confirm-details");
  dl.innerHTML = "";
  const rows = [
    ["Network", network.toUpperCase()],
    ["Amount", `${amount} ${asset}`],
    [owner ? "To token account" : "To", to],
    ...(owner ? [["Recipient wallet", owner]] : []),
    ["Fee", "0.000005 SOL"],
    ...(createsAccount ? [["New USDC account", `${formatUnits(TOKEN_ACCOUNT_RENT_LAMPORTS, 9)} SOL rent (paid once)`]] : []),
  ];
  for (const [k, v] of rows) {
    const dt = document.createElement("dt"); dt.textContent = k;
    const dd = document.createElement("dd"); dd.textContent = v;
    dl.append(dt, dd);
  }
  $("modal-status").textContent = "Sending to Fuchey…";
  $("countdown-bar").style.width = "100%";
  $("modal").classList.remove("hidden");
}

function startCountdown(ms) {
  clearInterval(countdownTimer);
  const end = Date.now() + ms;
  countdownTimer = setInterval(() => {
    const left = Math.max(0, end - Date.now());
    $("countdown-bar").style.width = `${(left / ms) * 100}%`;
    if (!left) clearInterval(countdownTimer);
  }, 250);
}

function closeModal() {
  clearInterval(countdownTimer);
  $("modal").classList.add("hidden");
}

function showResult(ok, text, txSig) {
  const el = $("result");
  el.className = `card ${ok ? "ok" : "fail"}`;
  el.textContent = text;
  if (txSig) {
    const a = document.createElement("a");
    a.href = `https://explorer.solana.com/tx/${txSig}${NETWORKS[state.network]?.explorerSuffix ?? ""}`;
    a.target = "_blank";
    a.rel = "noopener";
    a.textContent = "View on Solana Explorer";
    el.append(document.createElement("br"), a);
  }
}

// ── wiring ────────────────────────────────────────────────
function init() {
  if (!FucheyDevice.supported()) {
    $("unsupported").classList.remove("hidden");
    $("btn-connect").disabled = true;
  }
  $("btn-connect").addEventListener("click", connect);
  $("btn-disconnect").addEventListener("click", () => device.disconnect());
  $("btn-refresh").addEventListener("click", refreshBalances);
  $("btn-copy").addEventListener("click", () => navigator.clipboard?.writeText(state.info?.pubkey || ""));
  $("btn-receive").addEventListener("click", () => {
    const panel = $("receive");
    const open = panel.classList.toggle("hidden") === false;
    // Plain address (not a solana: URI) — every wallet app scans it.
    if (open && state.info?.pubkey) $("qr").innerHTML = qrSvg(state.info.pubkey);
  });
  $("btn-show-device").addEventListener("click", async () => {
    try {
      await device.showAddress();
      log("— Fuchey is showing its Receive QR");
    } catch (e) {
      showResult(false, `Could not show the address on Fuchey: ${describe(e)}`);
    }
  });
  $("send-form").addEventListener("submit", onSend);
  $("btn-cancel").addEventListener("click", async () => {
    $("modal-status").textContent = "Cancelling…";
    try { await device.cancel(); } catch (e) { log(`cancel: ${describe(e)}`); }
  });

  for (const net of ["devnet", "mainnet"]) $(`rpc-${net}`).value = settings.get(net);
  $("btn-save-settings").addEventListener("click", () => {
    for (const net of ["devnet", "mainnet"]) {
      const v = $(`rpc-${net}`).value.trim();
      settings.set(net, v === NETWORKS[net].rpc ? "" : v);
    }
    if (state.network) refreshBalances();
  });
}

init();
