/*
 * ble_update.c — modalità aggiornamento BLE (esperimento), vedi ble_update.h
 *
 * Servizio GATT con due caratteristiche:
 *   CTRL (write + notify)  comandi dal client e notifiche del dispositivo
 *   DATA (write no rsp)    blocchi dell'immagine, in ordine
 *
 * Comandi su CTRL:      0x01 START [size u32 LE] | 0x02 FINISH | 0x03 ABORT
 *                       0x04 ERASE_ON_EXIT = cancella le credenziali Zigbee all'uscita dalla modalità BLE
 *                       0x05 ERASE_NOW     = cancella subito le credenziali Zigbee ed esce dalla modalità BLE
 * Notifiche su CTRL:    [codice u8][valore u32 LE]
 *   0x10 READY     valore = byte utili per blocco (MTU-3)
 *   0x11 PROGRESS  valore = byte ricevuti finora (ad ogni 4096)
 *   0x12 DONE      immagine valida, riavvio imminente
 *   0x13 ERASE     valore = 1 cancellazione programmata, 2 credenziali cancellate (riavvio imminente)
 *   0x7F ERROR     valore = codice errore (E_*)
 *
 * ZB2: le credenziali Zigbee sono il namespace NVS "zigbee" più la partizione "zb_fct"
 * (come il factory reset Zigbee, vedi maint_erase_zigbee_credentials); la configurazione
 * (namespace sw_cfg: canali, tapparelle) non viene toccata.
 */

#include "ble_update.h"
#include "diag.h"
#include "led.h"
#include "maint.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "BLE_UPD";

#define BLE_NVS_NS   "ble_upd"
#define BLE_NVS_KEY  "go"

#define CMD_START   0x01
#define CMD_FINISH  0x02
#define CMD_ABORT   0x03
#define CMD_ERASE_ON_EXIT  0x04
#define CMD_ERASE_NOW      0x05

#define N_READY     0x10
#define N_PROGRESS  0x11
#define N_DONE      0x12
#define N_ERASE     0x13
#define N_ERROR     0x7F

#define E_STATE     1   /* comando fuori sequenza */
#define E_SIZE      2   /* dimensione non valida o diversa da quella annunciata */
#define E_BEGIN     3   /* esp_ota_begin fallita */
#define E_WRITE     4   /* scrittura in flash fallita */
#define E_VERIFY    5   /* immagine non valida (esp_ota_end) */
#define E_PROJECT   6   /* immagine di un altro progetto */
#define E_BOOT      7   /* impostazione partizione di avvio fallita */

#define ACK_STEP         4096
#define IDLE_TIMEOUT_S   120
#define MAX_SESSION_S    3600

static uint8_t  s_own_addr_type;
static char     s_name[20];
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_mtu = 23;
static uint16_t s_ctrl_handle;
static uint16_t s_data_handle;
static volatile int64_t s_last_activity_ms;
static volatile bool    s_finish_ok;
static volatile bool    s_erase_zb_on_exit;   /* cancella le credenziali Zigbee prima del riavvio */
static volatile bool    s_erase_zb_now;       /* ... subito, senza aggiornamento */

static bool                   s_active;
static esp_ota_handle_t       s_ota;
static const esp_partition_t *s_part;
static uint32_t               s_expected, s_received;
static uint8_t                s_rx[512];

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

/* ── Flag in NVS ─────────────────────────────────────────────────────── */
void ble_update_request_and_restart(void)
{
    nvs_handle_t h;
    if (nvs_open(BLE_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, BLE_NVS_KEY, 1);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGW(TAG, "richiesta modalità aggiornamento BLE — riavvio");
    diag_set_restart_cause(DIAG_CAUSE_BLE_UPDATE);
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

bool ble_update_pending(void)
{
    nvs_handle_t h;
    uint8_t go = 0;
    if (nvs_open(BLE_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    nvs_get_u8(h, BLE_NVS_KEY, &go);
    nvs_close(h);
    if (!go) return false;

    if (nvs_open(BLE_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, BLE_NVS_KEY, 0);
        nvs_commit(h);
        nvs_close(h);
    }
    return true;
}

/* ── Ricezione immagine ──────────────────────────────────────────────── */
static void notify(uint8_t code, uint32_t value)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) return;
    uint8_t b[5] = { code, (uint8_t)value, (uint8_t)(value >> 8),
                     (uint8_t)(value >> 16), (uint8_t)(value >> 24) };
    struct os_mbuf *om = ble_hs_mbuf_from_flat(b, sizeof b);
    if (!om) return;
    int rc = ble_gatts_notify_custom(s_conn, s_ctrl_handle, om);
    if (rc != 0) ESP_LOGW(TAG, "notifica 0x%02x non inviata, rc=%d", code, rc);
}

static void ota_abort(void)
{
    if (s_active) {
        esp_ota_abort(s_ota);
        s_active = false;
        ESP_LOGW(TAG, "trasferimento annullato");
    }
}

static void cmd_start(uint32_t size)
{
    ota_abort();
    s_part = esp_ota_get_next_update_partition(NULL);
    if (!s_part || size == 0 || size > s_part->size) {
        ESP_LOGE(TAG, "dimensione non valida: %"PRIu32, size);
        notify(N_ERROR, E_SIZE);
        return;
    }
    esp_err_t err = esp_ota_begin(s_part, OTA_WITH_SEQUENTIAL_WRITES, &s_ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        notify(N_ERROR, E_BEGIN);
        return;
    }
    s_active = true;
    s_expected = size;
    s_received = 0;
    ESP_LOGI(TAG, "inizio: %"PRIu32" byte su '%s' (MTU=%d)", size, s_part->label, s_mtu);
    notify(N_READY, s_mtu > 3 ? (uint32_t)(s_mtu - 3) : 20);
}

static void cmd_finish(void)
{
    if (!s_active) {
        notify(N_ERROR, E_STATE);
        return;
    }
    if (s_received != s_expected) {
        ESP_LOGE(TAG, "ricevuti %"PRIu32" byte su %"PRIu32, s_received, s_expected);
        ota_abort();
        notify(N_ERROR, E_SIZE);
        return;
    }
    esp_err_t err = esp_ota_end(s_ota);
    s_active = false;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        notify(N_ERROR, E_VERIFY);
        return;
    }

    esp_app_desc_t new_desc, run_desc;
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (esp_ota_get_partition_description(s_part, &new_desc) != ESP_OK ||
        esp_ota_get_partition_description(run, &run_desc) != ESP_OK ||
        strncmp(new_desc.project_name, run_desc.project_name,
                sizeof(new_desc.project_name)) != 0) {
        ESP_LOGE(TAG, "immagine di un altro progetto, rifiutata");
        notify(N_ERROR, E_PROJECT);
        return;
    }

    err = esp_ota_set_boot_partition(s_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        notify(N_ERROR, E_BOOT);
        return;
    }
    ESP_LOGW(TAG, "aggiornamento completato (versione '%s') — riavvio", new_desc.version);
    notify(N_DONE, 0);
    s_finish_ok = true;
}

static int ctrl_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_READ_NOT_PERMITTED;

    uint8_t buf[16];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof buf, &len) != 0 || len < 1)
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    s_last_activity_ms = now_ms();

    switch (buf[0]) {
    case CMD_START:
        if (len != 5) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        cmd_start((uint32_t)buf[1] | ((uint32_t)buf[2] << 8) |
                  ((uint32_t)buf[3] << 16) | ((uint32_t)buf[4] << 24));
        break;
    case CMD_FINISH:
        cmd_finish();
        break;
    case CMD_ABORT:
        ota_abort();
        break;
    case CMD_ERASE_ON_EXIT:
        s_erase_zb_on_exit = true;
        ESP_LOGW(TAG, "credenziali Zigbee: verranno cancellate all'uscita dalla modalità BLE");
        notify(N_ERASE, 1);
        break;
    case CMD_ERASE_NOW:
        if (s_active) {
            notify(N_ERROR, E_STATE);   /* aggiornamento in corso */
            break;
        }
        s_erase_zb_on_exit = true;
        s_erase_zb_now = true;          /* lo esegue il ciclo di ble_update_run() */
        break;
    default:
        return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    }
    return 0;
}

static int data_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_READ_NOT_PERMITTED;
    if (!s_active) return BLE_ATT_ERR_WRITE_NOT_PERMITTED;

    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, s_rx, sizeof s_rx, &len) != 0)
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    if (s_received + len > s_expected) {
        ESP_LOGE(TAG, "dati oltre la dimensione annunciata");
        ota_abort();
        notify(N_ERROR, E_SIZE);
        return 0;
    }
    esp_err_t err = esp_ota_write(s_ota, s_rx, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
        ota_abort();
        notify(N_ERROR, E_WRITE);
        return 0;
    }
    uint32_t prev = s_received;
    s_received += len;
    s_last_activity_ms = now_ms();
    if (s_received / ACK_STEP > prev / ACK_STEP)
        notify(N_PROGRESS, s_received);
    return 0;
}

/* UUID: 6a1d000N-c0de-4b1e-8b5e-5a1e7c0d0001 (N = 1 servizio, 2 CTRL, 3 DATA) */
static const ble_uuid128_t svc_uuid = BLE_UUID128_INIT(
    0x01, 0x00, 0x0d, 0x7c, 0x1e, 0x5a, 0x5e, 0x8b, 0x1e, 0x4b, 0xde, 0xc0, 0x01, 0x00, 0x1d, 0x6a);
static const ble_uuid128_t ctrl_uuid = BLE_UUID128_INIT(
    0x01, 0x00, 0x0d, 0x7c, 0x1e, 0x5a, 0x5e, 0x8b, 0x1e, 0x4b, 0xde, 0xc0, 0x02, 0x00, 0x1d, 0x6a);
static const ble_uuid128_t data_uuid = BLE_UUID128_INIT(
    0x01, 0x00, 0x0d, 0x7c, 0x1e, 0x5a, 0x5e, 0x8b, 0x1e, 0x4b, 0xde, 0xc0, 0x03, 0x00, 0x1d, 0x6a);

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &ctrl_uuid.u,
                .access_cb = ctrl_access,
                .val_handle = &s_ctrl_handle,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
            }, {
                .uuid = &data_uuid.u,
                .access_cb = data_access,
                .val_handle = &s_data_handle,
                .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE,
            }, {
                0,
            }
        },
    }, {
        0,
    },
};

/* ── GAP ─────────────────────────────────────────────────────────────── */
static void advertise(void);

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "connessione %s (status=%d)",
                 event->connect.status == 0 ? "stabilita" : "fallita", event->connect.status);
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            s_last_activity_ms = now_ms();
            /* Più throughput: intervallo 7.5-15 ms e pacchetti radio da 251 byte.
             * Sono richieste: l'ultima parola spetta al centrale. */
            struct ble_gap_upd_params p = {
                .itvl_min = 6, .itvl_max = 12, .latency = 0,
                .supervision_timeout = 400, .min_ce_len = 0, .max_ce_len = 0,
            };
            int urc = ble_gap_update_params(s_conn, &p);
            int drc = ble_gap_set_data_len(s_conn, 251, 2120);
            ESP_LOGI(TAG, "richiesti parametri veloci: update rc=%d, data_len rc=%d", urc, drc);
        } else {
            advertise();
        }
        return 0;
    case BLE_GAP_EVENT_CONN_UPDATE: {
        struct ble_gap_conn_desc d;
        if (ble_gap_conn_find(event->conn_update.conn_handle, &d) == 0)
            ESP_LOGI(TAG, "parametri di connessione: intervallo=%.2f ms, latenza=%d, status=%d",
                     d.conn_itvl * 1.25, d.conn_latency, event->conn_update.status);
        return 0;
    }
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnesso (reason=%d)", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_mtu = 23;
        ota_abort();
        advertise();
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;
    case BLE_GAP_EVENT_MTU:
        s_mtu = event->mtu.value;
        ESP_LOGI(TAG, "MTU=%d", event->mtu.value);
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "notifiche %s", event->subscribe.cur_notify ? "attive" : "disattivate");
        return 0;
    default:
        return 0;
    }
}

static void advertise(void)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof fields);
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof adv_params);
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start rc=%d", rc);
        return;
    }
    ESP_LOGI(TAG, "annuncio attivo: '%s'", name);
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "reset dello stack BLE, reason=%d", reason);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto rc=%d", rc);
        return;
    }
    advertise();
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void erase_zigbee_credentials(void)
{
    /* ZB2: le credenziali sono nel namespace NVS "zigbee" (+ zb_fct), vedi maint.h */
    maint_erase_zigbee_credentials(false);
}

static void leave_ble_mode(void)
{
    if (s_erase_zb_on_exit) {
        s_erase_zb_on_exit = false;
        erase_zigbee_credentials();
    }
    diag_set_restart_cause(DIAG_CAUSE_BLE_UPDATE);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

void ble_update_run(void)
{
    ESP_LOGW(TAG, "*** MODALITA' AGGIORNAMENTO BLE (%d s) — Zigbee non avviato ***",
             BLE_UPDATE_WINDOW_S);

    led_init();
    led_set_state(LED_OTA_IN_PROGRESS);

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init fallita: %s — torno alla modalità normale",
                 esp_err_to_name(err));
        esp_restart();
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb  = on_sync;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc == 0) rc = ble_gatts_add_svcs(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "registrazione servizio GATT fallita rc=%d — torno alla modalità normale", rc);
        esp_restart();
    }

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(s_name, sizeof(s_name), "SmartSwitch-%02X%02X", mac[4], mac[5]);
    ble_svc_gap_device_name_set(s_name);

    nimble_port_freertos_init(host_task);

    int64_t t0 = now_ms();
    s_last_activity_ms = t0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));

        if (s_erase_zb_now) {
            s_erase_zb_now = false;
            s_erase_zb_on_exit = false;
            erase_zigbee_credentials();
            notify(N_ERASE, 2);
            vTaskDelay(pdMS_TO_TICKS(1500));
            ESP_LOGW(TAG, "credenziali cancellate — riavvio in modalità normale");
            leave_ble_mode();
        }

        if (s_finish_ok) {
            vTaskDelay(pdMS_TO_TICKS(1500));
            ESP_LOGW(TAG, "riavvio con il nuovo firmware");
            leave_ble_mode();
        }

        int64_t now = now_ms();
        bool connected = (s_conn != BLE_HS_CONN_HANDLE_NONE);
        if (!connected && (now - t0) >= (int64_t)BLE_UPDATE_WINDOW_S * 1000) break;
        if (connected && (now - s_last_activity_ms) >= (int64_t)IDLE_TIMEOUT_S * 1000) break;
        if ((now - t0) >= (int64_t)MAX_SESSION_S * 1000) break;
    }

    ESP_LOGW(TAG, "finestra BLE conclusa — riavvio in modalità normale");
    leave_ble_mode();
}
