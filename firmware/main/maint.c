/*
 * maint.c — manutenzione: modalità programmazione (download seriale) e comandi da seriale
 *
 * La modalità programmazione usa il bit FORCE_DOWNLOAD_BOOT di LP_AON_SYS_CFG_REG
 * (stesso meccanismo che l'IDF usa su altri chip in usb_console.c con RTC_CNTL_OPTION1_REG):
 * il bootloader ROM lo legge al reset successivo ed entra in download mode.
 * Su ESP32-H2 l'IDF non lo usa: da verificare su banco, in particolare che il bit non resti
 * attivo dopo il flash (in quel caso basta togliere e ridare l'alimentazione).
 */

#include "maint.h"

#include <string.h>

#include "diag.h"

#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "nvs.h"
#include <inttypes.h>
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "soc/lp_aon_reg.h"
#include "soc/soc.h"

static const char *TAG = "MAINT";

void maint_erase_zigbee_credentials(bool erase_phy)
{
    /* 1. Namespace "zigbee" nella partizione NVS di default ("nvs"), dove lo stack 2.x salva i dati */
    nvs_handle_t h;
    esp_err_t err = nvs_open("zigbee", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_erase_all(h);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGW(TAG, "NVS namespace 'zigbee': %s", err == ESP_OK ? "CANCELLATO" : esp_err_to_name(err));

    /* 2. Partizioni: zb_fct, vecchia zb_storage (FAT 1.x, inutilizzata dallo stack 2.x), phy_init se richiesto */
    const char *parts[] = { "zb_fct", "zb_storage", "phy_init" };
    int n = erase_phy ? 3 : 2;
    for (int i = 0; i < n; i++) {
        const esp_partition_t *p = esp_partition_find_first(
            ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, parts[i]);
        if (!p) {
            ESP_LOGE(TAG, "partizione '%s' non trovata", parts[i]);
            continue;
        }
        err = esp_partition_erase_range(p, 0, p->size);
        ESP_LOGW(TAG, "partizione '%s' (0x%"PRIx32", %"PRIu32" byte): %s",
                 parts[i], p->address, p->size, err == ESP_OK ? "CANCELLATA" : esp_err_to_name(err));
    }
}

void maint_enter_download_mode(void)
{
    ESP_LOGW(TAG, "riavvio in modalità programmazione (download seriale)");
    REG_SET_BIT(LP_AON_SYS_CFG_REG, LP_AON_FORCE_DOWNLOAD_BOOT);
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

#if CONFIG_ESP_CONSOLE_UART

#define CMD_UART  ((uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM)

static void handle_command(const char *cmd)
{
    if (strcmp(cmd, "prog") == 0) {
        maint_enter_download_mode();
    } else if (strcmp(cmd, "reset") == 0) {
        /* Riavvio voluto: causa registrata, quindi non conta come anomalo e salta il deep sleep. */
        ESP_LOGW(TAG, "riavvio da comando seriale");
        diag_set_restart_cause(DIAG_CAUSE_SERIAL);
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    } else if (strcmp(cmd, "help") == 0) {
        ESP_LOGI(TAG, "comandi: prog = riavvia in modalità programmazione (download seriale); "
                      "reset = riavvio normale");
    } else {
        ESP_LOGW(TAG, "comando sconosciuto '%s' (scrivi help)", cmd);
    }
}

static void serial_task(void *pv)
{
    char line[32];
    size_t n = 0;

    for (;;) {
        uint8_t c;
        if (uart_read_bytes(CMD_UART, &c, 1, pdMS_TO_TICKS(200)) != 1)
            continue;
        if (c == '\r' || c == '\n') {
            line[n] = '\0';
            n = 0;
            if (line[0] != '\0')
                handle_command(line);
        } else if (n < sizeof(line) - 1) {
            line[n++] = (char)c;
        }
    }
}

void maint_serial_start(void)
{
    /* Se il bit fosse rimasto attivo da un ciclo precedente, lo azzera: il prossimo reset è normale. */
    REG_CLR_BIT(LP_AON_SYS_CFG_REG, LP_AON_FORCE_DOWNLOAD_BOOT);

    esp_err_t err = uart_driver_install(CMD_UART, 256, 0, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "uart_driver_install: %s — comandi seriali non attivi", esp_err_to_name(err));
        return;
    }
    xTaskCreate(serial_task, "serial_cmd", 2560, NULL, 1, NULL);
}

#else

void maint_serial_start(void)
{
    REG_CLR_BIT(LP_AON_SYS_CFG_REG, LP_AON_FORCE_DOWNLOAD_BOOT);
}

#endif
