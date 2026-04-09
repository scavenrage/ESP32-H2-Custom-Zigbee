/*
 * ota.c — Zigbee OTA Upgrade client per ESP32-H2
 *
 * Nota ZBOSS: STATUS_FINISH (0x5) non corrisponde a ESP_ZB_ZCL_OTA_UPGRADE_STATUS_FINISH
 * nel SDK v1.6. Workaround: esp_ota_end() e esp_ota_set_boot_partition() vengono
 * eseguiti in STATUS_APPLY, che viene chiamato correttamente da ZBOSS.
 */

#include "ota.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>

static const char *TAG = "OTA";

/* Stato corrente del trasferimento OTA */
static esp_ota_handle_t      s_ota_handle        = 0;
static const esp_partition_t *s_ota_part         = NULL;
static bool                   s_skip_subelement  = false; /* salta header sub-elemento ZCL OTA */

/* ------------------------------------------------------------------ */
/*  Cluster OTA                                                        */
/* ------------------------------------------------------------------ */

esp_zb_attribute_list_t *ota_cluster_create(void)
{
    esp_zb_ota_cluster_cfg_t cfg = {
        .ota_upgrade_file_version         = OTA_FILE_VERSION,
        .ota_upgrade_manufacturer         = OTA_MANUFACTURER_CODE,
        .ota_upgrade_image_type           = OTA_IMAGE_TYPE,
        .ota_min_block_reque              = 0,
        .ota_upgrade_file_offset          = 0xFFFFFFFF,
        .ota_upgrade_downloaded_file_ver  = 0xFFFFFFFF,
        .ota_upgrade_server_id            = ESP_ZB_ZCL_OTA_UPGRADE_SERVER_DEF_VALUE,
        .ota_image_upgrade_status         = ESP_ZB_ZCL_OTA_UPGRADE_IMAGE_STATUS_DEF_VALUE,
    };
    esp_zb_attribute_list_t *cluster = esp_zb_ota_cluster_create(&cfg);

    /* Inizializza la variabile client OTA.
     * Il default SDK è 1440 min (24h) — senza questa init la struttura
     * CLIENT_DATA (attr 0xfff1) è NULL e causa un assert alla prima query. */
    esp_zb_zcl_ota_upgrade_client_variable_t client_var = {
        .timer_query   = ESP_ZB_ZCL_OTA_UPGRADE_QUERY_TIMER_COUNT_DEF,
        .hw_version    = OTA_HW_VERSION,
        .max_data_size = OTA_MAX_DATA_SIZE,
    };
    uint16_t server_addr = 0xffff;
    uint8_t  server_ep   = 0xff;

    esp_zb_ota_cluster_add_attr(cluster,
        ESP_ZB_ZCL_ATTR_OTA_UPGRADE_CLIENT_DATA_ID,
        (void *)&client_var);
    esp_zb_ota_cluster_add_attr(cluster,
        ESP_ZB_ZCL_ATTR_OTA_UPGRADE_SERVER_ADDR_ID,
        (void *)&server_addr);
    esp_zb_ota_cluster_add_attr(cluster,
        ESP_ZB_ZCL_ATTR_OTA_UPGRADE_SERVER_ENDPOINT_ID,
        (void *)&server_ep);

    return cluster;
}

/* ------------------------------------------------------------------ */
/*  Handler OTA                                                        */
/* ------------------------------------------------------------------ */

esp_err_t ota_upgrade_handler(const esp_zb_zcl_ota_upgrade_value_message_t *message)
{
    if (!message) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = ESP_OK;

    switch (message->upgrade_status) {

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_START:
        ESP_LOGI(TAG, "OTA avviato — versione 0x%08"PRIx32", size %"PRIu32" byte",
                 message->ota_header.file_version,
                 message->ota_header.image_size);

        s_ota_part = esp_ota_get_next_update_partition(NULL);
        if (!s_ota_part) {
            ESP_LOGE(TAG, "Nessuna partizione OTA disponibile");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Scrittura su partizione: %s", s_ota_part->label);

        ret = esp_ota_begin(s_ota_part, OTA_WITH_SEQUENTIAL_WRITES, &s_ota_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(ret));
        } else {
            /* Il primo chunk contiene l'header del sub-elemento ZCL OTA:
             * tag (uint16_t) + length (uint32_t) = 6 byte che ZBOSS non striscia.
             * Dobbiamo saltarli prima di passare i dati a esp_ota_write(). */
            s_skip_subelement = true;
        }
        break;

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_RECEIVE: {
        if (!s_ota_handle) return ESP_FAIL;
        const uint8_t *data = (const uint8_t *)message->payload;
        uint16_t       size = message->payload_size;

        if (s_skip_subelement) {
            /* Log primi 8 byte grezzi per diagnostica */
            ESP_LOGI(TAG, "RECEIVE raw[0..7]: %02x %02x %02x %02x %02x %02x %02x %02x (size=%d)",
                     size > 0 ? data[0] : 0xFF, size > 1 ? data[1] : 0xFF,
                     size > 2 ? data[2] : 0xFF, size > 3 ? data[3] : 0xFF,
                     size > 4 ? data[4] : 0xFF, size > 5 ? data[5] : 0xFF,
                     size > 6 ? data[6] : 0xFF, size > 7 ? data[7] : 0xFF,
                     size);
            /* Salta i 6 byte: tag(2) + length(4) del sub-elemento */
            if (size >= 6) {
                data += 6;
                size -= 6;
            }
            s_skip_subelement = false;
            ESP_LOGI(TAG, "RECEIVE dopo skip: primo byte=0x%02x (atteso 0xe9), size=%d",
                     data[0], size);
        }

        ret = esp_ota_write(s_ota_handle, data, size);
        if (ret != ESP_OK)
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(ret));
        break;
    }

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_FINISH:
        /* ZBOSS non chiama questo status in modo affidabile (invia 0x5 non mappato).
         * Il completamento viene gestito in STATUS_APPLY qui sotto. */
        ESP_LOGI(TAG, "OTA STATUS_FINISH ricevuto (inatteso ma gestito)");
        if (s_ota_handle) {
            ret = esp_ota_end(s_ota_handle);
            s_ota_handle = 0;
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_end FALLITO: %s (0x%x)", esp_err_to_name(ret), ret);
                s_ota_part = NULL;
                break;
            }
            ESP_LOGI(TAG, "esp_ota_end OK — immagine valida");
            ret = esp_ota_set_boot_partition(s_ota_part);
            if (ret == ESP_OK)
                ESP_LOGI(TAG, "Boot partition impostata a %s", s_ota_part->label);
            else
                ESP_LOGE(TAG, "esp_ota_set_boot_partition FALLITO: %s (0x%x)",
                         esp_err_to_name(ret), ret);
        }
        break;

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_APPLY:
        /* Workaround: ZBOSS manda STATUS 0x5 (non gestito) invece di STATUS_FINISH,
         * poi chiama STATUS_APPLY. Eseguiamo qui la validazione e il cambio partizione. */
        ESP_LOGI(TAG, "OTA APPLY — chiudo scrittura flash...");
        if (s_ota_handle) {
            ret = esp_ota_end(s_ota_handle);
            s_ota_handle = 0;
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_end FALLITO: %s (0x%x)", esp_err_to_name(ret), ret);
                break;
            }
            ESP_LOGI(TAG, "esp_ota_end OK — immagine valida");
            ret = esp_ota_set_boot_partition(s_ota_part);
            if (ret == ESP_OK)
                ESP_LOGI(TAG, "Boot partition impostata a %s", s_ota_part->label);
            else {
                ESP_LOGE(TAG, "esp_ota_set_boot_partition FALLITO: %s (0x%x)",
                         esp_err_to_name(ret), ret);
                break;
            }
        } else {
            ESP_LOGW(TAG, "APPLY: s_ota_handle già zero (FINISH già eseguito? ok)");
        }
        ESP_LOGI(TAG, "Riavvio...");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
        break;

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ABORT:
        ESP_LOGW(TAG, "OTA annullato");
        if (s_ota_handle) {
            esp_ota_abort(s_ota_handle);
            s_ota_handle = 0;
        }
        s_ota_part        = NULL;
        s_skip_subelement = false;
        break;

    default:
        ESP_LOGW(TAG, "OTA status non gestito: 0x%x", message->upgrade_status);
        break;
    }

    return ret;
}

/* ------------------------------------------------------------------ */
/*  Anti-rollback                                                      */
/* ------------------------------------------------------------------ */

void ota_mark_valid(void)
{
    /* Se il bootloader ha ROLLBACK abilitato, dobbiamo segnalare che il
     * firmware è funzionante, altrimenti al prossimo riavvio torna al
     * firmware precedente. Chiamare subito all'avvio, prima di Zigbee. */
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
        if (state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "Firmware verificato — segno come valido");
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }
}
