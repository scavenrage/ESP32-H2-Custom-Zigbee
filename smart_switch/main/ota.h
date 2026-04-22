/*
 * ota.h — Zigbee OTA Upgrade client per ESP32-H2 (smart_switch)
 */

#pragma once

#include "esp_err.h"
#include "esp_zigbee_core.h"

#define OTA_MANUFACTURER_CODE   0x1001
#define OTA_IMAGE_TYPE          0x0104   /* 0x0104 = smart_switch */
#define OTA_HW_VERSION          0x0001
#define OTA_MAX_DATA_SIZE       223

#define OTA_FILE_VERSION        0x01030000   /* v1.3.0 */
#define OTA_SW_BUILD_ID         "\x06""v1.3.0"

esp_zb_attribute_list_t *ota_cluster_create(void);
esp_err_t ota_upgrade_handler(const esp_zb_zcl_ota_upgrade_value_message_t *message);
void ota_mark_valid(void);
