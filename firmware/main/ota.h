/*
 * ota.h — Zigbee OTA Upgrade client per ESP32-H2 (smart_switch)
 */

#pragma once

#include "esp_err.h"
/* ZB2: con CONFIG_ZB_SDK_1xx l'header da includere è quello di compatibilità
 * (compat/esp_zigbee_core.h, che include esp_zigbee.h e tutte le API 1.x). */
#include "esp_zigbee_core.h"

/* ZB2: OTA Zigbee disattivato. Nel livello di compatibilità di esp-zigbee-lib 2.0.5
 * il cluster OTA è uno stub: esp_zb_ota_cluster_create() -> NULL,
 * esp_zb_cluster_list_add_ota_cluster() e le query client -> ESP_ERR_NOT_SUPPORTED
 * (compat/esp_zigbee_cluster.h, compat/esp_zigbee_ota.h). Aggiornamenti solo via BLE o cavo. */
#define ZB_OTA_ENABLED          0

#define OTA_MANUFACTURER_CODE   0x1001
#define OTA_IMAGE_TYPE          0x0104   /* 0x0104 = smart_switch */
#define OTA_HW_VERSION          0x0001
#define OTA_MAX_DATA_SIZE       223

#define OTA_FILE_VERSION        0x02000000   /* v2.0.0-zb2: prova esp-zigbee-lib 2.x (livello di compatibilità 1.x) */
#define OTA_SW_BUILD_ID         "\x06""v2.0.0"

#if ZB_OTA_ENABLED
esp_zb_attribute_list_t *ota_cluster_create(void);
esp_err_t ota_upgrade_handler(const esp_zb_zcl_ota_upgrade_value_message_t *message);
#endif
void ota_mark_valid(void);
