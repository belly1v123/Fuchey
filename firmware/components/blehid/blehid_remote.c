/*
 * Fuchey BLE HID remote — adapted from
 * ElectronicCats/badge-hackgdl-2025 firmware/components/ble_hid/ble_hidd_main.c
 * (Espressif Bluedroid HIDD example, AGPL-3.0 / Unlicense).
 *
 * Changes vs badge:
 *  - Device name MININO_HID -> FUCHEY_HID
 *  - NEVER nvs_flash_erase() (badge did on NO_FREE_PAGES; that would wipe
 *    the Fuchey wallet in NVS). NVS is owned by Storage::init().
 *  - Added blehid_end() for stop-on-exit (frees BT RAM, avoids WiFi contention).
 *  - Idempotent begin/end via s_started flag.
 */
#include "blehid_remote.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_defs.h"
#include "esp_gatts_api.h"
#include "esp_hidd_prf_api.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "hid_dev.h"
#include "nvs_flash.h"
#include <string.h>

#define TAG "BleHid"

static uint16_t s_conn_id = 0;
static bool s_sec_conn = false;
static bool s_started = false;
static bool s_classic_released = false;
static blehid_event_cb_t s_cb = NULL;

static char *HID_DEVICE_NAME = "FUCHEY_HID";
static uint8_t hidd_service_uuid128[] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x12, 0x18, 0x00, 0x00,
};

static esp_ble_adv_data_t hidd_adv_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = true,
    .min_interval = 0x0006,
    .max_interval = 0x0010,
    .appearance = 0x03c0, // HID Generic
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = sizeof(hidd_service_uuid128),
    .p_service_uuid = hidd_service_uuid128,
    .flag = 0x6,
};

static esp_ble_adv_params_t hidd_adv_params = {
    .adv_int_min = 0x20,
    .adv_int_max = 0x30,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static void hidd_event_callback(esp_hidd_cb_event_t event,
                                esp_hidd_cb_param_t *param) {
  switch (event) {
  case ESP_HIDD_EVENT_REG_FINISH: {
    if (param->init_finish.state == ESP_HIDD_INIT_OK) {
      esp_ble_gap_set_device_name(HID_DEVICE_NAME);
      esp_ble_gap_config_adv_data(&hidd_adv_data);
    }
    break;
  }
  case ESP_BAT_EVENT_REG:
    break;
  case ESP_HIDD_EVENT_DEINIT_FINISH:
    break;
  case ESP_HIDD_EVENT_BLE_CONNECT: {
    ESP_LOGI(TAG, "HID connected");
    s_conn_id = param->connect.conn_id;
    if (s_cb != NULL) {
      s_cb(true);
    }
    break;
  }
  case ESP_HIDD_EVENT_BLE_DISCONNECT: {
    s_sec_conn = false;
    ESP_LOGI(TAG, "HID disconnected, re-advertising");
    esp_ble_gap_start_advertising(&hidd_adv_params);
    if (s_cb != NULL) {
      s_cb(false);
    }
    break;
  }
  case ESP_HIDD_EVENT_BLE_VENDOR_REPORT_WRITE_EVT: {
    // Inbound report bytes are intentionally not logged (secret-logging).
    break;
  }
  case ESP_HIDD_EVENT_BLE_LED_REPORT_WRITE_EVT: {
    break;
  }
  default:
    break;
  }
}

static void gap_event_handler(esp_gap_ble_cb_event_t event,
                              esp_ble_gap_cb_param_t *param) {
  switch (event) {
  case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
    esp_ble_gap_start_advertising(&hidd_adv_params);
    break;
  case ESP_GAP_BLE_SEC_REQ_EVT:
    esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
    break;
  case ESP_GAP_BLE_AUTH_CMPL_EVT:
    s_sec_conn = true;
    ESP_LOGI(TAG, "HID bonded: %s",
             param->ble_security.auth_cmpl.success ? "ok" : "fail");
    if (!param->ble_security.auth_cmpl.success) {
      ESP_LOGE(TAG, "bond fail reason=0x%x",
               param->ble_security.auth_cmpl.fail_reason);
    }
    break;
  default:
    break;
  }
}

void blehid_volume_up(bool press) {
  if (s_sec_conn) {
    esp_hidd_send_consumer_value(s_conn_id, HID_CONSUMER_VOLUME_UP, press);
  }
}

void blehid_volume_down(bool press) {
  if (s_sec_conn) {
    esp_hidd_send_consumer_value(s_conn_id, HID_CONSUMER_VOLUME_DOWN, press);
  }
}

void blehid_play_pause(bool press) {
  if (s_sec_conn) {
    esp_hidd_send_consumer_value(s_conn_id, HID_CONSUMER_PLAY, press);
  }
}

void blehid_register_callback(blehid_event_cb_t cb) { s_cb = cb; }

bool blehid_is_connected(void) { return s_sec_conn; }
bool blehid_is_started(void) { return s_started; }

void blehid_get_device_name(char *out, size_t out_len) {
  if (out && out_len) {
    strncpy(out, HID_DEVICE_NAME, out_len - 1);
    out[out_len - 1] = '\0';
  }
}

void blehid_get_device_mac(uint8_t out[6]) {
  if (out) {
    esp_read_mac(out, ESP_MAC_BT);
  }
}

bool blehid_begin(void) {
  if (s_started) {
    if (s_cb) {
      s_cb(s_sec_conn);
    }
    return true;
  }

  // NVS owned by Fuchey Storage::init(). Init if needed, but NEVER erase —
  // badge code erased on NO_FREE_PAGES which would destroy the wallet.
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(TAG, "NVS init state %s — leaving pages untouched (wallet safe)",
             esp_err_to_name(ret));
  } else if (ret != ESP_OK) {
    ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(ret));
    return false;
  }

  // Classic-BT memory is released exactly once per boot. Re-releasing (or
  // aborting via ESP_ERROR_CHECK) is what rebooted the device on re-entry.
  if (!s_classic_released) {
    ret = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK) {
      ESP_LOGW(TAG, "classic mem release: %s (continuing)",
               esp_err_to_name(ret));
    } else {
      s_classic_released = true;
    }
  }

  esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  ret = esp_bt_controller_init(&bt_cfg);
  if (ret) {
    ESP_LOGE(TAG, "BT controller init failed: %s", esp_err_to_name(ret));
    return false;
  }
  ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
  if (ret) {
    ESP_LOGE(TAG, "BT controller enable failed: %s", esp_err_to_name(ret));
    esp_bt_controller_deinit();
    return false;
  }
  ret = esp_bluedroid_init();
  if (ret) {
    ESP_LOGE(TAG, "bluedroid init failed: %s", esp_err_to_name(ret));
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    return false;
  }
  ret = esp_bluedroid_enable();
  if (ret) {
    ESP_LOGE(TAG, "bluedroid enable failed: %s", esp_err_to_name(ret));
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    return false;
  }
  if (esp_hidd_profile_init() != ESP_OK) {
    ESP_LOGE(TAG, "hidd profile init failed");
  }

  esp_ble_gap_register_callback(gap_event_handler);
  esp_hidd_register_callbacks(hidd_event_callback);

  esp_ble_auth_req_t auth_req = ESP_LE_AUTH_BOND;
  esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
  uint8_t key_size = 16;
  uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
  uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
  esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req,
                                 sizeof(uint8_t));
  esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap,
                                 sizeof(uint8_t));
  esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size,
                                 sizeof(uint8_t));
  esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key,
                                 sizeof(uint8_t));
  esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key,
                                 sizeof(uint8_t));

  s_started = true;
  ESP_LOGI(TAG, "BLE HID started as %s", HID_DEVICE_NAME);
  return true;
}

void blehid_end(void) {
  if (!s_started) {
    return;
  }
  s_sec_conn = false;
  s_conn_id = 0;
  esp_ble_gap_stop_advertising();
  esp_hidd_profile_deinit();
  esp_bluedroid_disable();
  esp_bluedroid_deinit();
  esp_bt_controller_disable();
  esp_bt_controller_deinit();
  // NOTE: BLE controller memory is intentionally NOT released here.
  // Releasing it makes re-init impossible without a reboot (that was the
  // re-entry crash). Disable/deinit already stops the radio and frees tasks.
  s_started = false;
  ESP_LOGI(TAG, "BLE HID stopped");
}
