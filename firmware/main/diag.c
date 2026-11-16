/*
 * diag.c — diagnostica persistente (esperimento), vedi diag.h
 *
 * Attributi del cluster DIAG_CLUSTER_ID (tutti U32, sola lettura):
 *   0x0000 uptime_s        secondi dall'avvio (un calo = riavvio)
 *   0x0001 boots           avvii totali
 *   0x0002 last_event      ultimo riavvio: esp_reset_reason_t, oppure 100+diag_cause_t
 *   0x0003 rr_poweron      power-on (alimentazione tolta e ridata)
 *   0x0004 rr_brownout     reset da brownout
 *   0x0005 rr_panic        reset da panic/eccezione
 *   0x0006 rr_wdt          reset da watchdog (interrupt/task/altri)
 *   0x0007 sw_untagged     reset software senza causa registrata dal firmware
 *   0x0008 cs_zb_wdt       riavvii voluti: watchdog applicativo Zigbee
 *   0x0009 cs_hard_reset   riavvii voluti: zigbee_hard_reset
 *   0x000A cs_phy_reset    riavvii voluti: zigbee_phy_reset
 *   0x000B cs_other        riavvii voluti: OTA / factory reset
 *   0x000C ev_ping_fail    keepalive verso il coordinator fallito
 *   0x000D ev_no_links     segnale NO_ACTIVE_LINKS_LEFT
 *   0x000E ev_leave        segnale LEAVE
 *   0x000F ev_steer_fail   steering fallito
 *   0x0010 unexpected_resets  brownout + panic + watchdog + reset software senza causa
 *                             (non salvato: è la somma dei contatori 0x0004..0x0007).
 *                             Lo invia il firmware con un report singolo dopo ogni avvio. Il reporting
 *                             configurato da ZHA su questo attributo mandava lo stack ZBOSS in crash
 *                             a ogni avvio (record di reporting senza descrittore): non va configurato.
 */

#include "diag.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <string.h>

static const char *TAG = "DIAG";

#define DIAG_NS       "diag"
#define DIAG_KEY      "data"
#define DIAG_VERSION  1

typedef struct {
    uint32_t version;
    uint32_t boots;
    uint32_t last_event;
    uint32_t rr_poweron;
    uint32_t rr_brownout;
    uint32_t rr_panic;
    uint32_t rr_wdt;
    uint32_t sw_untagged;
    uint32_t cs_zb_wdt;
    uint32_t cs_hard_reset;
    uint32_t cs_phy_reset;
    uint32_t cs_other;
    uint32_t ev_ping_fail;
    uint32_t ev_no_links;
    uint32_t ev_leave;
    uint32_t ev_steer_fail;
    uint32_t pending_cause;
} diag_data_t;

static diag_data_t   s_d;
static volatile bool s_dirty;
static bool          s_commanded;   /* l'ultimo riavvio è stato deciso dall'utente (OTA, BLE, factory reset) */

#define DIAG_ATTR_UNEXPECTED  0x0010

typedef struct {
    uint16_t  id;
    uint32_t *src;   /* NULL = valore calcolato, vedi diag_value() */
} diag_attr_t;

static const diag_attr_t s_attrs[] = {
    { 0x0000, NULL },
    { 0x0001, &s_d.boots },
    { 0x0002, &s_d.last_event },
    { 0x0003, &s_d.rr_poweron },
    { 0x0004, &s_d.rr_brownout },
    { 0x0005, &s_d.rr_panic },
    { 0x0006, &s_d.rr_wdt },
    { 0x0007, &s_d.sw_untagged },
    { 0x0008, &s_d.cs_zb_wdt },
    { 0x0009, &s_d.cs_hard_reset },
    { 0x000A, &s_d.cs_phy_reset },
    { 0x000B, &s_d.cs_other },
    { 0x000C, &s_d.ev_ping_fail },
    { 0x000D, &s_d.ev_no_links },
    { 0x000E, &s_d.ev_leave },
    { 0x000F, &s_d.ev_steer_fail },
    { DIAG_ATTR_UNEXPECTED, NULL },
};
#define DIAG_ATTR_COUNT  (sizeof(s_attrs) / sizeof(s_attrs[0]))

static uint32_t s_zb_val[DIAG_ATTR_COUNT];

static uint32_t diag_value(size_t i)
{
    if (s_attrs[i].src != NULL)
        return *s_attrs[i].src;
    if (s_attrs[i].id == DIAG_ATTR_UNEXPECTED)
        return s_d.rr_brownout + s_d.rr_panic + s_d.rr_wdt + s_d.sw_untagged;
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

/* ── NVS ─────────────────────────────────────────────────────────────── */
static void diag_load(void)
{
    memset(&s_d, 0, sizeof(s_d));
    nvs_handle_t h;
    if (nvs_open(DIAG_NS, NVS_READONLY, &h) == ESP_OK) {
        diag_data_t tmp;
        size_t len = sizeof(tmp);
        if (nvs_get_blob(h, DIAG_KEY, &tmp, &len) == ESP_OK &&
            len == sizeof(tmp) && tmp.version == DIAG_VERSION) {
            s_d = tmp;
        }
        nvs_close(h);
    }
    s_d.version = DIAG_VERSION;
}

static void diag_flush(void)
{
    nvs_handle_t h;
    if (nvs_open(DIAG_NS, NVS_READWRITE, &h) != ESP_OK) return;
    diag_data_t snap = s_d;
    s_dirty = false;
    if (nvs_set_blob(h, DIAG_KEY, &snap, sizeof(snap)) == ESP_OK)
        nvs_commit(h);
    nvs_close(h);
}

static void diag_task(void *pv)
{
    (void)pv;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        if (s_dirty) diag_flush();
    }
}

/* ── API ─────────────────────────────────────────────────────────────── */
void diag_boot(esp_reset_reason_t rr)
{
    diag_load();
    s_d.boots++;

    uint32_t cause = s_d.pending_cause;
    s_d.pending_cause = 0;

    uint32_t code = (uint32_t)rr;
    bool new_event = true;

    switch (rr) {
    case ESP_RST_POWERON:
        s_d.rr_poweron++;
        break;
    case ESP_RST_BROWNOUT:
        s_d.rr_brownout++;
        break;
    case ESP_RST_PANIC:
        s_d.rr_panic++;
        break;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
        s_d.rr_wdt++;
        break;
    case ESP_RST_SW:
    case ESP_RST_DEEPSLEEP:
        if (cause != DIAG_CAUSE_NONE) {
            switch (cause) {
            case DIAG_CAUSE_ZB_WDT:     s_d.cs_zb_wdt++;     break;
            case DIAG_CAUSE_HARD_RESET: s_d.cs_hard_reset++; break;
            case DIAG_CAUSE_PHY_RESET:  s_d.cs_phy_reset++;  break;
            default:                    s_d.cs_other++; s_commanded = true; break;
            }
            code = 100 + cause;
        } else if (rr == ESP_RST_SW) {
            s_d.sw_untagged++;
        } else {
            /* Risveglio dal deep sleep di recupero: l'evento è già stato contato
             * all'avvio precedente. */
            new_event = false;
        }
        break;
    default:
        break;
    }

    if (new_event) s_d.last_event = code;
    diag_flush();

    ESP_LOGI(TAG, "boot #%"PRIu32" rr=%d ultimo_evento=%"PRIu32" brownout=%"PRIu32" panic=%"PRIu32
             " wdt=%"PRIu32" sw_senza_causa=%"PRIu32" poweron=%"PRIu32,
             s_d.boots, (int)rr, s_d.last_event, s_d.rr_brownout, s_d.rr_panic,
             s_d.rr_wdt, s_d.sw_untagged, s_d.rr_poweron);
}

bool diag_restart_was_commanded(void)
{
    return s_commanded;
}

void diag_start_task(void)
{
    xTaskCreate(diag_task, "diag", 3072, NULL, 1, NULL);
}

void diag_set_restart_cause(diag_cause_t cause)
{
    s_d.pending_cause = (uint32_t)cause;
    diag_flush();
}

void diag_event(diag_event_t evt)
{
    uint32_t *c = NULL;
    switch (evt) {
    case DIAG_EVT_PING_FAIL:     c = &s_d.ev_ping_fail;  break;
    case DIAG_EVT_NO_LINKS:      c = &s_d.ev_no_links;   break;
    case DIAG_EVT_LEAVE:         c = &s_d.ev_leave;      break;
    case DIAG_EVT_STEERING_FAIL: c = &s_d.ev_steer_fail; break;
    default: break;
    }
    if (c) {
        __atomic_add_fetch(c, 1, __ATOMIC_RELAXED);
        s_dirty = true;
    }
}

#if !DIAG_ZB_CLUSTER
esp_zb_attribute_list_t *diag_zb_cluster_create(void) { return NULL; }
void diag_zb_update(uint8_t endpoint) { (void)endpoint; }
void diag_zb_report_to_coordinator(uint8_t endpoint) { (void)endpoint; }
#else
esp_zb_attribute_list_t *diag_zb_cluster_create(void)
{
    esp_zb_attribute_list_t *al = esp_zb_zcl_attr_list_create(DIAG_CLUSTER_ID);
    if (!al) {
        ESP_LOGE(TAG, "creazione cluster 0x%04x fallita", DIAG_CLUSTER_ID);
        return NULL;
    }
    for (size_t i = 0; i < DIAG_ATTR_COUNT; i++) {
        s_zb_val[i] = diag_value(i);
        /* Solo 0x0010 ha il flag "reportable": senza, esp_zb_zcl_report_attr_cmd_req risponde
         * ESP_ERR_NOT_SUPPORTED (verificato dal disassemblato). Il reporting NON va configurato
         * da ZHA su questo attributo: il quirk non ha reporting_config. */
        uint8_t access = ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY;
        if (s_attrs[i].id == DIAG_ATTR_UNEXPECTED)
            access |= ESP_ZB_ZCL_ATTR_ACCESS_REPORTING;
        esp_err_t err = esp_zb_custom_cluster_add_custom_attr(al, s_attrs[i].id,
            ESP_ZB_ZCL_ATTR_TYPE_U32, access, &s_zb_val[i]);
        if (err != ESP_OK)
            ESP_LOGW(TAG, "attributo 0x%04x non aggiunto: %s", s_attrs[i].id, esp_err_to_name(err));
    }
    return al;
}

void diag_zb_update(uint8_t endpoint)
{
    for (size_t i = 0; i < DIAG_ATTR_COUNT; i++) {
        uint32_t v = diag_value(i);
        esp_zb_zcl_set_attribute_val(endpoint, DIAG_CLUSTER_ID,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, s_attrs[i].id, &v, false);
    }
}

void diag_zb_report_to_coordinator(uint8_t endpoint)
{
    /* Report singolo verso il coordinator (0x0000, endpoint 1 di ZHA): "ignora la configurazione
     * del reporting" (esp_zigbee_zcl_command.h), quindi non passa dai record di reporting che
     * mandavano in crash lo stack. */
    uint32_t v = s_d.rr_brownout + s_d.rr_panic + s_d.rr_wdt + s_d.sw_untagged;
    esp_zb_zcl_set_attribute_val(endpoint, DIAG_CLUSTER_ID,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, DIAG_ATTR_UNEXPECTED, &v, false);

    esp_zb_zcl_report_attr_cmd_t cmd = {0};
    cmd.zcl_basic_cmd.dst_addr_u.addr_short = 0x0000;
    cmd.zcl_basic_cmd.dst_endpoint = 1;
    cmd.zcl_basic_cmd.src_endpoint = endpoint;
    cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd.clusterID = DIAG_CLUSTER_ID;
    cmd.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI;
    cmd.manuf_code = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC;
    cmd.attributeID = DIAG_ATTR_UNEXPECTED;
    esp_err_t err = esp_zb_zcl_report_attr_cmd_req(&cmd);
    ESP_LOGI(TAG, "report riavvii anomali (%"PRIu32") al coordinator: %s", v, esp_err_to_name(err));
}
#endif
