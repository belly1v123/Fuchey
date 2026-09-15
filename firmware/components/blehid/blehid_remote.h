#pragma once
// ============================================================
// Fuchey — blehid_remote.h
// BLE HID media remote (port of badge-hackgdl-2025 ble_hid).
// Media keys only: Vol Up / Vol Down / Play-Pause (consumer report).
// Lifecycle: begin() on entering HID screen, end() on exit (B1).
// Untrusted subsystem: must never touch WalletCore.
// ============================================================
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*blehid_event_cb_t)(bool connected);

// Start BLE HID advertising as "FUCHEY_HID". Idempotent.
// Safe to call after Storage::init() — never erases NVS.
// Returns true on success; on failure BT is left stopped, safe to retry.
bool blehid_begin(void);
// Stop advertising + disable/deinit the BT stack (radio off, tasks freed).
// The BLE controller's static memory is intentionally NOT released, so a
// later blehid_begin() can re-init without a reboot.
void blehid_end(void);

bool blehid_is_connected(void);
bool blehid_is_started(void);
void blehid_register_callback(blehid_event_cb_t cb);
void blehid_get_device_name(char *out, size_t out_len);
void blehid_get_device_mac(uint8_t out[6]);

// Press+release is done by caller as true then false (matches badge).
void blehid_volume_up(bool press);
void blehid_volume_down(bool press);
void blehid_play_pause(bool press);

#ifdef __cplusplus
}
#endif
