/*
 * ble_log.c — log via BLE (esperimento), vedi ble_log.h
 *
 * Servizio GATT con una sola caratteristica NOTIFY (testo UTF-8, le righe del log così
 * come escono sulla seriale). Il testo viene catturato con esp_log_set_vprintf(), messo
 * in un buffer circolare e inviato da un task ogni DRAIN_PERIOD_MS, a blocchi di MTU-3
 * byte: così più righe viaggiano insieme e la radio, condivisa con Zigbee, lavora poco.
 * Se il buffer si riempie i byte più vecchi vengono scartati e il client riceve un
 * avviso "[... N byte di log persi]".
 *
 * UUID: 6a1d00NN-c0de-4b1e-8b5e-5a1e7c0d0001 (NN = 10 servizio, 11 log)
 *
 * Il livello di log di NimBLE deve restare WARNING (sdkconfig.defaults): a INFO ogni
 * notifica genererebbe una riga di log, quindi un'altra notifica, all'infinito.
 */

#include "ble_log.h"
#include "diag.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
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
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "BLE_LOG";

#define LOG_NVS_NS   "ble_log"
#define LOG_NVS_KEY  "on"

#define RING_SIZE        4096
#define LINE_MAX_LEN     160    /* righe più lunghe vengono troncate (solo nel buffer BLE) */
#define DRAIN_PERIOD_MS  100
#define NOTIFY_BURST     8      /* notifiche per ciclo: non saturare i buffer del controller */
#define CHUNK_MAX        244

/* ── Buffer circolare ────────────────────────────────────────────────── */
static uint8_t  s_ring[RING_SIZE];
static size_t   s_tail, s_len;
static uint32_t s_dropped;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void ring_write(const char *d, size_t n)
{
    portENTER_CRITICAL_SAFE(&s_mux);
    if (s_len + n > RING_SIZE) {
        size_t over = s_len + n - RING_SIZE;
        s_tail = (s_tail + over) % RING_SIZE;
        s_len -= over;
        s_dropped += over;
    }
    size_t head = (s_tail + s_len) % RING_SIZE;
    for (size_t i = 0; i < n; i++) {
        s_ring[head] = (uint8_t)d[i];
        head = (head + 1) % RING_SIZE;
    }
    s_len += n;
    portEXIT_CRITICAL_SAFE(&s_mux);
}

static size_t ring_peek(uint8_t *out, size_t max)
{
    portENTER_CRITICAL(&s_mux);
    size_t n = s_len < max ? s_len : max;
    size_t idx = s_tail;
    for (size_t i = 0; i < n; i++) {
        out[i] = s_ring[idx];
        idx = (idx + 1) % RING_SIZE;
    }
    portEXIT_CRITICAL(&s_mux);
    return n;
}

static void ring_consume(size_t n)
{
    portENTER_CRITICAL(&s_mux);
    if (n > s_len) n = s_len;
    s_tail = (s_tail + n) % RING_SIZE;
    s_len -= n;
    portEXIT_CRITICAL(&s_mux);
}

/* ── Cattura del log ─────────────────────────────────────────────────── */
static vprintf_like_t s_orig_vprintf;
static TaskHandle_t   s_task;

static int log_hook(const char *fmt, va_list args)
{
    /* Il task che invia non deve alimentare il buffer con quello che stampa. */
    if (xTaskGetCurrentTaskHandle() != s_task) {
        char line[LINE_MAX_LEN];
        va_list ap;
        va_copy(ap, args);
        int n = vsnprintf(line, sizeof line, fmt, ap);
        va_end(ap);
        if (n > 0) {
            if (n >= (int)sizeof line) {
                n = sizeof line - 1;
                line[n - 1] = '\n';
            }
            ring_write(line, (size_t)n);
        }
    }
    return s_orig_vprintf ? s_orig_vprintf(fmt, args) : 0;
}

/* ── Flag in NVS ─────────────────────────────────────────────────────── */
bool ble_log_enabled(void)
{
    nvs_handle_t h;
    uint8_t on = 0;
    if (nvs_open(LOG_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    nvs_get_u8(h, LOG_NVS_KEY, &on);
    nvs_close(h);
    return on != 0;
}

void ble_log_set_enabled_and_restart(bool on)
{
    nvs_handle_t h;
    if (nvs_open(LOG_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, LOG_NVS_KEY, on ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGW(TAG, "log via BLE %s — riavvio", on ? "ATTIVATO" : "DISATTIVATO");
    diag_set_restart_cause(DIAG_CAUSE_BLE_LOG);
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

/* ── GATT ────────────────────────────────────────────────────────────── */
static uint8_t  s_own_addr_type;
static char     s_name[20];
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_mtu = 23;
static uint16_t s_log_handle;
static volatile bool s_subscribed;

static int log_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)ctxt; (void)arg;
    return BLE_ATT_ERR_READ_NOT_PERMITTED;   /* solo notifiche */
}

static const ble_uuid128_t svc_uuid = BLE_UUID128_INIT(
    0x01, 0x00, 0x0d, 0x7c, 0x1e, 0x5a, 0x5e, 0x8b, 0x1e, 0x4b, 0xde, 0xc0, 0x10, 0x00, 0x1d, 0x6a);
static const ble_uuid128_t log_uuid = BLE_UUID128_INIT(
    0x01, 0x00, 0x0d, 0x7c, 0x1e, 0x5a, 0x5e, 0x8b, 0x1e, 0x4b, 0xde, 0xc0, 0x11, 0x00, 0x1d, 0x6a);

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &log_uuid.u,
                .access_cb = log_access,
                .val_handle = &s_log_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            }, {
                0,
            }
        },
    }, {
        0,
    },
};

static bool send_chunk(const uint8_t *p, size_t n)
{
    struct os_mbuf *om = ble_hs_mbuf_from_flat(p, n);
    if (!om) return false;
    return ble_gatts_notify_custom(s_conn, s_log_handle, om) == 0;
}

static void drain_task(void *arg)
{
    (void)arg;
    uint8_t chunk[CHUNK_MAX];

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(DRAIN_PERIOD_MS));
        if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_subscribed) continue;

        size_t max = s_mtu > 3 ? (size_t)(s_mtu - 3) : 20;
        if (max > sizeof chunk) max = sizeof chunk;

        uint32_t dropped;
        portENTER_CRITICAL(&s_mux);
        dropped = s_dropped;
        s_dropped = 0;
        portEXIT_CRITICAL(&s_mux);
        if (dropped) {
            char m[48];
            int k = snprintf(m, sizeof m, "[... %"PRIu32" byte di log persi]\n", dropped);
            if (k > 0 && !send_chunk((const uint8_t *)m, (size_t)k)) {
                portENTER_CRITICAL(&s_mux);
                s_dropped += dropped;
                portEXIT_CRITICAL(&s_mux);
            }
        }

        for (int i = 0; i < NOTIFY_BURST; i++) {
            size_t n = ring_peek(chunk, max);
            if (n == 0) break;
            if (!send_chunk(chunk, n)) break;   /* buffer del controller pieno: riprova al ciclo dopo */
            ring_consume(n);
        }
    }
}

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
            /* Intervallo lungo (50-100 ms): la radio è condivisa con Zigbee e un log
             * non ha bisogno di velocità. Il centrale può non accettare. */
            struct ble_gap_upd_params p = {
                .itvl_min = 40, .itvl_max = 80, .latency = 0,
                .supervision_timeout = 600, .min_ce_len = 0, .max_ce_len = 0,
            };
            ble_gap_update_params(s_conn, &p);
        } else {
            advertise();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnesso (reason=%d)", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        s_mtu = 23;
        advertise();
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;
    case BLE_GAP_EVENT_MTU:
        s_mtu = event->mtu.value;
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_log_handle)
            s_subscribed = event->subscribe.cur_notify;
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

void ble_log_start_if_enabled(void)
{
    if (!ble_log_enabled()) return;

    ESP_LOGW(TAG, "*** LOG VIA BLE ATTIVO (nessun timer): disattiva con l'interruttore LOG "
                  "o con una pressione breve di BOOT ***");

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init fallita: %s — log via BLE non attivo", esp_err_to_name(err));
        return;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb  = on_sync;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc == 0) rc = ble_gatts_add_svcs(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "registrazione servizio GATT fallita rc=%d — log via BLE non attivo", rc);
        return;
    }

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(s_name, sizeof(s_name), "SmartLog-%02X%02X", mac[4], mac[5]);
    ble_svc_gap_device_name_set(s_name);

    xTaskCreate(drain_task, "ble_log", 3072, NULL, 2, &s_task);
    s_orig_vprintf = esp_log_set_vprintf(log_hook);

    nimble_port_freertos_init(host_task);
}
