/*
 * main.c — smart_switch (ESP32-H2)
 *
 * Firmware unificato: relay stabile, relay a impulso, tapparella, dimmer.
 * La configurazione è in NVS (namespace "sw_cfg"), caricata a runtime da sw_config.c.
 * Il file config.h contiene solo costanti (CH_NONE, CH_RELAY_STABLE, ...).
 *
 * Vantaggi rispetto alla versione compile-time:
 *   - Un unico binario per tutti i dispositivi
 *   - OTA aggiorna solo la partizione app; la configurazione NVS resta intatta
 *   - Prima programmazione: flash app + nvs_config.bin (generato da configure.py)
 *   - Aggiornamenti successivi: flash solo l'app via OTA Zigbee
 *
 * Endpoint Zigbee assegnati in ordine crescente a tutti i canali attivi:
 *   CH_RELAY_STABLE / CH_RELAY_IMPULSE → On/Off Output (0x0002)
 *   CH_SHUTTER                         → Window Covering (0x0202)
 *   CH_DIMMER                          → Dimmable Light (0x0101)
 */

#include "config.h"
#include "sw_config.h"
#include "ota.h"
#include "led.h"
#include "relay.h"
#include "shutter.h"
#include "dimmer.h"
#include "diag.h"
#include "ble_update.h"
#include "maint.h"
#include "ble_log.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_zigbee_core.h"   /* ZB2: header di compatibilità 1.x (CONFIG_ZB_SDK_1xx) */
#include "esp_zigbee_cluster.h"
#if ZB_OTA_ENABLED
/* ZB2: in 2.0.5 questo header ridefinisce esp_zb_ota_file_header_t / _optional_t già
 * definiti in compat/zcl/esp_zigbee_zcl_command.h: non si possono includere entrambi. */
#include "esp_zigbee_ota.h"
#endif
#include "ha/esp_zigbee_ha_standard.h"
#include "zdo/esp_zigbee_zdo_command.h"
#include "esp_partition.h"
#include "esp_sleep.h"
#include <inttypes.h>
#include <string.h>

static const char *TAG = "MAIN";

/* ── Configurazione Zigbee ───────────────────────────────────────────── */
#define INSTALLCODE_POLICY_ENABLE    false
#define MAX_CHILDREN                 10
#define ESP_ZB_PRIMARY_CHANNEL_MASK  ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK   /* canali 11-26: si associa al coordinatore su qualunque canale (come Altherma/ConsoleZ906) */
#define OTA_UPGRADE_QUERY_INTERVAL   240   /* minuti */

#define ESP_MANUFACTURER_NAME  "\x09""Handmade!"
#define ESP_MODEL_IDENTIFIER   "\x0C""SmartSwitchO"

#define ESP_ZB_ZR_CONFIG() {                                        \
    .esp_zb_role         = ESP_ZB_DEVICE_TYPE_ROUTER,               \
    .install_code_policy = INSTALLCODE_POLICY_ENABLE,               \
    .nwk_cfg.zczr_cfg    = { .max_children = MAX_CHILDREN },        \
}
#define ESP_ZB_DEFAULT_RADIO_CONFIG() { .radio_mode = ZB_RADIO_MODE_NATIVE }
#define ESP_ZB_DEFAULT_HOST_CONFIG()  { .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE }

/* ── Mappa endpoint → canale ─────────────────────────────────────────── */
static uint8_t s_ep_to_ch[5];    /* ep 1..4 */
static uint8_t s_ep_to_type[5];
static uint8_t s_ch_to_ep[4];    /* ch 0..3 → ep */
static uint8_t s_ota_ep = 1;

/* Endpoint On/Off "virtuali" (nessun relay collegato): On = azione di manutenzione dopo 1.5 s.
 * Numeri fissi fuori dal range dei canali (ep 1..4); non sono nelle tabelle s_ep_to_*. */
#define BLE_SWITCH_EP      10   /* riavvia in modalità aggiornamento BLE */
#define PROG_SWITCH_EP     11   /* riavvia in modalità programmazione (download seriale) */
#define FACTORY_SWITCH_EP  12   /* factory reset Zigbee (come BOOT premuto 5 s) */
#define LOG_SWITCH_EP      13   /* log via BLE: ON/OFF (stato salvato in NVS, riavvia) */
#define IS_VIRTUAL_EP(ep)  ((ep) >= BLE_SWITCH_EP)

static bool zigbee_ready            = false;
static bool s_coordinator_confirmed  = false;   /* true solo dopo ping ZDO riuscito */
static uint8_t s_ping_tick           = 0;
static uint8_t s_ping_fail_count     = 0;       /* fallimenti ZDO consecutivi       */
static bool    s_ping_in_flight      = false;   /* ping inviato, attesa callback    */
static uint8_t s_steering_fail_count = 0;       /* fallimenti steering consecutivi  */

/* ── Keepalive: costanti e forward declaration ───────────────────────── */
/*
 * esp_zb_scheduler_alarm tronca il delay a 16 bit internamente:
 * valori > 65535 ms vanno in overflow. Usiamo 60000 ms (sicuro, stesso valore
 * del watchdog) e contiamo COORDINATOR_PING_TICKS tick prima di ogni ping reale
 * → intervallo effettivo = 60000 × 5 = 5 minuti.
 *
 * Se il coordinator non risponde per COORDINATOR_PING_MAX_FAILS cicli
 * consecutivi (≈ 4-5 min), il dispositivo è definitivamente disconnesso
 * (tipicamente per interferenze RF) → zigbee_hard_reset(): cancella
 * credenziali (NVS "zigbee" + zb_fct) e riavvia per un join pulito.
 *
 * COORDINATOR_PING_TIMEOUT_MS: timeout di sicurezza per il caso in cui
 * la callback ZDO non venga mai chiamata (route error anziché ZDP timeout).
 */
#define COORDINATOR_PING_TICK_MS       60000    /* intervallo sicuro per scheduler  */
#define COORDINATOR_PING_TICKS         5        /* 5 × 60s = 5 min tra i ping       */
#define COORDINATOR_PING_MAX_FAILS     6        /* hard reset dopo 6 fail × ~45s ≈ 4-5 min */
#define COORDINATOR_PING_TIMEOUT_MS    40000    /* timeout se callback non arriva   */
#define STEERING_MAX_FAILS             10       /* phy reset dopo 10 fail × 30s ≈ 5 min */
static void coordinator_ping_cb(uint8_t param);         /* definita più in basso */
static void coordinator_ping_timeout_cb(uint8_t param); /* definita più in basso */

static void init_ep_map(void)
{
    memset(s_ep_to_ch,   0xFF,     sizeof(s_ep_to_ch));
    memset(s_ep_to_type, CH_NONE,  sizeof(s_ep_to_type));
    memset(s_ch_to_ep,   0,        sizeof(s_ch_to_ep));

    uint8_t ep = 1;
    for (int i = 0; i < 4; i++) {
        if (g_sw_cfg.ch_type[i] != CH_NONE) {
            s_ch_to_ep[i]    = ep;
            s_ep_to_ch[ep]   = (uint8_t)i;
            s_ep_to_type[ep] = g_sw_cfg.ch_type[i];
            ep++;
        }
    }
    s_ota_ep = 1;
}

/* ── OTA ─────────────────────────────────────────────────────────────── */
static void ota_find_server(void)
{
#if ZB_OTA_ENABLED
    esp_zb_ota_upgrade_client_query_interval_set(s_ota_ep, OTA_UPGRADE_QUERY_INTERVAL);
    esp_zb_ota_upgrade_client_query_image_req(0x0000, 1);
    ESP_LOGI(TAG, "OTA: query → coordinator, polling ogni %d min", OTA_UPGRADE_QUERY_INTERVAL);
#else
    ESP_LOGI(TAG, "OTA Zigbee disattivato in questa build (ZB2): aggiornamenti via BLE o cavo");
#endif
}

/* ── Zigbee hard reset (disconnessione definitiva) ───────────────────── */
/*
 * Chiamare quando il dispositivo è definitivamente disconnesso dal coordinator
 * e i normali meccanismi di steering non bastano (tipicamente: interferenze RF
 * che corrompono lo stato dello stack Zigbee).
 *
 * Cancella le credenziali (NVS "zigbee" + zb_fct, ZB2) → al prossimo avvio lo stack riparte da zero
 * e fa un join pulito. La configurazione NVS (sw_cfg, relay, shutter, dimmer)
 * rimane intatta.
 *
 * Dopo esp_restart() il reset reason sarà ESP_RST_SW: il check in app_main()
 * triggera un deep sleep di 3s che resetta il radio, poi il device si riavvia
 * con lo storage vuoto e fa il join.
 */
static void zigbee_hard_reset(const char *reason)
{
    ESP_LOGW(TAG, "*** ZIGBEE HARD RESET (%s) — cancello credenziali Zigbee ***", reason);
    led_set_state(LED_ERROR);
    /* ZB2: causa scritta prima della cancellazione, così tra cancellazione e riavvio passa
     * il minimo tempo (lo stack Zigbee è ancora attivo e potrebbe riscrivere in NVS). */
    diag_set_restart_cause(DIAG_CAUSE_HARD_RESET);
    maint_erase_zigbee_credentials(true);
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_restart();
}

/* Cancella solo la calibrazione PHY e riavvia.
 * le credenziali (NVS "zigbee" + zb_fct) restano intatte: al prossimo avvio il device
 * ricalibra il radio da zero e fa un rejoin con le credenziali esistenti.
 * Non richiede permit join sul coordinator. */
static void zigbee_phy_reset(const char *reason)
{
    ESP_LOGW(TAG, "*** ZIGBEE PHY RESET (%s) — cancello phy_init ***", reason);
    led_set_state(LED_ERROR);
    const esp_partition_t *p = esp_partition_find_first(
        ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, "phy_init");
    if (p) {
        esp_err_t err = esp_partition_erase_range(p, 0, p->size);
        ESP_LOGW(TAG, "  'phy_init' (0x%"PRIx32", %"PRIu32"B): %s",
                 p->address, p->size,
                 err == ESP_OK ? "OK" : esp_err_to_name(err));
    } else {
        ESP_LOGE(TAG, "  'phy_init': partizione non trovata!");
    }
    diag_set_restart_cause(DIAG_CAUSE_PHY_RESET);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

/* ── Callback hardware → Zigbee ──────────────────────────────────────── */

/*
 * IMPORTANTE: il firmware rollershutter (che funziona perfettamente) NON usa
 * MAI esp_zb_lock_acquire. Usa SOLO esp_zb_scheduler_alarm per tutte le
 * comunicazioni verso lo stack Zigbee. Mischiare esp_zb_lock_acquire (mutex)
 * con esp_zb_scheduler_alarm (coda) da task diversi causa race condition
 * sul QueueSet interno dello stack Zigbee → crash in prvNotifyQueueSetContainer.
 *
 * Tutte le callback hardware usano quindi esp_zb_scheduler_alarm.
 *
 * ZB2 (esp-zigbee-lib 2.x): la regola sopra valeva per la 1.x/ZBOSS. In 2.x esp_zigbee.h dice:
 * "It's mandatory to acquire the lock before calling any Zigbee SDK APIs, except that the call
 * site is in Zigbee callbacks." Senza lock, gli allarmi programmati dai task degli ingressi/timer
 * facevano sparire altri allarmi (watchdog, keepalive): visto l'11/11/2026 sul modulo di sviluppo
 * mentre si usavano i tasti fisici. Le callback hardware passano quindi da zb_alarm_any_task():
 * dal task Zigbee chiamata diretta, da altri task con il lock.
 */

static TaskHandle_t s_zb_task = NULL;   /* task Zigbee_main, impostato in esp_zb_task() */

static void zb_alarm_any_task(esp_zb_callback_t cb, uint8_t param, uint32_t ms)
{
    if (s_zb_task != NULL && xTaskGetCurrentTaskHandle() == s_zb_task) {
        /* Già nel task Zigbee (es. dimmer comandato da Zigbee): niente lock, potrebbe non essere rientrante */
        esp_zb_scheduler_alarm(cb, param, ms);
        return;
    }
    /* Attesa limitata: alcune chiamate arrivano dal task dei timer FreeRTOS, che non va bloccato a lungo */
    if (esp_zb_lock_acquire(pdMS_TO_TICKS(1000))) {
        esp_zb_scheduler_alarm(cb, param, ms);
        esp_zb_lock_release();
    } else {
        ESP_LOGW(TAG, "lock Zigbee non ottenuto in 1 s: aggiornamento verso HA saltato (lo recupera il keepalive)");
    }
}

/* ── Relay → Zigbee (via scheduler_alarm) ──────────────────────────── */
/* Codifica parametro: bit7=stato, bit0..6=canale */
static void do_relay_zigbee_sync(uint8_t param)
{
    uint8_t ch  = param & 0x7F;
    uint8_t val = (param & 0x80) ? 1 : 0;
    if (!zigbee_ready) return;
    uint8_t ep = s_ch_to_ep[ch];
    esp_zb_zcl_set_attribute_val(ep,
        ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID,
        &val, false);
}

static void on_relay_changed(uint8_t ch, bool state)
{
    if (!zigbee_ready) return;
    uint8_t param = (ch & 0x7F) | (state ? 0x80 : 0x00);
    zb_alarm_any_task(do_relay_zigbee_sync, param, 0);   /* ZB2: chiamata da task ingressi / timer impulso */
}

/* ── Shutter → Zigbee (via scheduler_alarm) ────────────────────────── */
static void do_shutter_zigbee_sync(uint8_t param)
{
    uint8_t ch = param;
    if (!zigbee_ready) return;
    uint8_t ep  = s_ch_to_ep[ch];
    uint8_t pos = shutter_get_position(ch);
    esp_zb_zcl_set_attribute_val(ep,
        ESP_ZB_ZCL_CLUSTER_ID_WINDOW_COVERING,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_WINDOW_COVERING_CURRENT_POSITION_LIFT_PERCENTAGE_ID,
        &pos, false);

    /* ZB2: a movimento finito (fine corsa o STOP) report esplicito della posizione al coordinator.
     * In esp-zigbee-lib 2.x reportable_change=N vale "variazione > N" (esp-zigbee-sdk issue #903,
     * risposta Espressif); ZHA configura N=1, quindi l'ultimo passo di 1 punto (es. 1→0) non
     * generava report e HA restava a 99% fino al report periodico (max 900 s). Con 1.x non succedeva.
     * Siamo nel contesto Zigbee (scheduler alarm): niente lock. */
    if (!shutter_is_moving(ch)) {
        esp_zb_zcl_report_attr_cmd_t cmd = {0};
        cmd.zcl_basic_cmd.dst_addr_u.addr_short = 0x0000;
        cmd.zcl_basic_cmd.dst_endpoint = 1;
        cmd.zcl_basic_cmd.src_endpoint = ep;
        cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
        cmd.clusterID = ESP_ZB_ZCL_CLUSTER_ID_WINDOW_COVERING;
        cmd.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI;
        cmd.manuf_code = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC;
        cmd.attributeID = ESP_ZB_ZCL_ATTR_WINDOW_COVERING_CURRENT_POSITION_LIFT_PERCENTAGE_ID;
        esp_err_t err = esp_zb_zcl_report_attr_cmd_req(&cmd);
        ESP_LOGI(TAG, "EP%d: report posizione finale %d%%: %s", ep, pos, esp_err_to_name(err));
    }
}

static void on_shutter_changed(uint8_t ch, uint8_t pos)
{
    if (!zigbee_ready) return;
    zb_alarm_any_task(do_shutter_zigbee_sync, ch, 0);    /* ZB2: chiamata da task ingressi / timer tapparella */
}

/* ── Dimmer → Zigbee (via scheduler_alarm) ─────────────────────────── */
/* Stato dimmer memorizzato qui per il callback asincrono */
static volatile uint8_t s_dimmer_pending_level;
static volatile bool    s_dimmer_pending_on;

static void do_dimmer_zigbee_sync(uint8_t param)
{
    (void)param;
    if (!zigbee_ready) return;
    uint8_t ep     = s_ch_to_ep[0];
    uint8_t on_val = s_dimmer_pending_on ? 1 : 0;
    uint8_t level  = s_dimmer_pending_level;
    esp_zb_zcl_set_attribute_val(ep,
        ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID,
        &on_val, false);
    esp_zb_zcl_set_attribute_val(ep,
        ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID,
        &level, false);
}

static void on_dimmer_changed(bool on_off, uint8_t level)
{
    if (!zigbee_ready) return;
    s_dimmer_pending_on    = on_off;
    s_dimmer_pending_level = level;
    zb_alarm_any_task(do_dimmer_zigbee_sync, 0, 0);      /* ZB2: task pulsante oppure task Zigbee */
}

/* ── Costruzione cluster Basic ───────────────────────────────────────── */
static esp_zb_attribute_list_t *create_basic_cluster(void)
{
    esp_zb_basic_cluster_cfg_t cfg = {
        .zcl_version  = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = 0x01,
    };
    esp_zb_attribute_list_t *c = esp_zb_basic_cluster_create(&cfg);
    esp_zb_basic_cluster_add_attr(c, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, ESP_MANUFACTURER_NAME);
    esp_zb_basic_cluster_add_attr(c, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,  ESP_MODEL_IDENTIFIER);
    esp_zb_basic_cluster_add_attr(c, ESP_ZB_ZCL_ATTR_BASIC_SW_BUILD_ID,          OTA_SW_BUILD_ID);
    return c;
}

/* Cluster diagnostico (solo sull'endpoint che ospita anche l'OTA) */
static void add_diag_cluster(esp_zb_cluster_list_t *cl)
{
    esp_zb_attribute_list_t *d = diag_zb_cluster_create();
    if (d)
        esp_zb_cluster_list_add_custom_cluster(cl, d, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
}

/* ── Costruzione endpoint On/Off (relay) ─────────────────────────────── */
static void add_relay_endpoint(esp_zb_ep_list_t *ep_list, uint8_t ep, uint8_t ch)
{
    esp_zb_cluster_list_t *cl = esp_zb_zcl_cluster_list_create();

    esp_zb_cluster_list_add_basic_cluster(cl, create_basic_cluster(),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_identify_cluster_cfg_t id_cfg = { .identify_time = 0 };
    esp_zb_cluster_list_add_identify_cluster(cl,
        esp_zb_identify_cluster_create(&id_cfg),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_on_off_cluster_cfg_t oo = { .on_off = 0 };
    esp_zb_cluster_list_add_on_off_cluster(cl,
        esp_zb_on_off_cluster_create(&oo), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    if (ep == s_ota_ep) {
#if ZB_OTA_ENABLED
        esp_zb_cluster_list_add_ota_cluster(cl,
            ota_cluster_create(), ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
#endif
        add_diag_cluster(cl);
    }

    esp_zb_endpoint_config_t epcfg = {
        .endpoint           = ep,
        .app_profile_id     = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id      = ESP_ZB_HA_ON_OFF_OUTPUT_DEVICE_ID,
        .app_device_version = 0,
    };
    esp_zb_ep_list_add_ep(ep_list, cl, epcfg);
    ESP_LOGI(TAG, "EP%d: On/Off relay (CH%d)", ep, ch);
}

/* ── Endpoint virtuali di manutenzione ────────────────────────────────── */
static void add_virtual_switch_endpoint(esp_zb_ep_list_t *ep_list, uint8_t ep, const char *what,
                                        bool initial_on)
{
    esp_zb_cluster_list_t *cl = esp_zb_zcl_cluster_list_create();

    esp_zb_cluster_list_add_basic_cluster(cl, create_basic_cluster(),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_on_off_cluster_cfg_t oo = { .on_off = initial_on ? 1 : 0 };
    esp_zb_cluster_list_add_on_off_cluster(cl,
        esp_zb_on_off_cluster_create(&oo), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_endpoint_config_t epcfg = {
        .endpoint           = ep,
        .app_profile_id     = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id      = ESP_ZB_HA_ON_OFF_OUTPUT_DEVICE_ID,
        .app_device_version = 0,
    };
    esp_zb_ep_list_add_ep(ep_list, cl, epcfg);
    ESP_LOGI(TAG, "EP%d: interruttore virtuale %s", ep, what);
}

/* ── Costruzione endpoint Window Covering (tapparella) ───────────────── */
static void add_shutter_endpoint(esp_zb_ep_list_t *ep_list, uint8_t ep, uint8_t ch)
{
    esp_zb_window_covering_cfg_t wc_cfg = ESP_ZB_DEFAULT_WINDOW_COVERING_CONFIG();
    esp_zb_cluster_list_t *cl = esp_zb_window_covering_clusters_create(&wc_cfg);

    esp_zb_attribute_list_t *basic = esp_zb_cluster_list_get_cluster(cl,
        ESP_ZB_ZCL_CLUSTER_ID_BASIC, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_basic_cluster_add_attr(basic, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, ESP_MANUFACTURER_NAME);
    esp_zb_basic_cluster_add_attr(basic, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,  ESP_MODEL_IDENTIFIER);
    esp_zb_basic_cluster_add_attr(basic, ESP_ZB_ZCL_ATTR_BASIC_SW_BUILD_ID,          OTA_SW_BUILD_ID);

    esp_zb_attribute_list_t *wc = esp_zb_cluster_list_get_cluster(cl,
        ESP_ZB_ZCL_CLUSTER_ID_WINDOW_COVERING, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    uint8_t pos = shutter_get_position(ch);
    esp_zb_window_covering_cluster_add_attr(wc,
        ESP_ZB_ZCL_ATTR_WINDOW_COVERING_CURRENT_POSITION_LIFT_PERCENTAGE_ID, &pos);

    if (ep == s_ota_ep) {
#if ZB_OTA_ENABLED
        esp_zb_cluster_list_add_ota_cluster(cl,
            ota_cluster_create(), ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
#endif
        add_diag_cluster(cl);
    }

    esp_zb_endpoint_config_t epcfg = {
        .endpoint           = ep,
        .app_profile_id     = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id      = ESP_ZB_HA_WINDOW_COVERING_DEVICE_ID,
        .app_device_version = 0,
    };
    esp_zb_ep_list_add_ep(ep_list, cl, epcfg);
    ESP_LOGI(TAG, "EP%d: Window Covering tapparella (CH%d+%d)", ep, ch, ch+1);
}

/* ── Costruzione endpoint Dimmable Light ─────────────────────────────── */
static void add_dimmer_endpoint(esp_zb_ep_list_t *ep_list, uint8_t ep)
{
    esp_zb_basic_cluster_cfg_t    basic_cfg    = { .zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE, .power_source = 0x01 };
    esp_zb_identify_cluster_cfg_t identify_cfg = { .identify_time = 0 };
    esp_zb_groups_cluster_cfg_t   groups_cfg   = { .groups_name_support_id = 0 };
    esp_zb_scenes_cluster_cfg_t   scenes_cfg   = { .scenes_count = 0, .current_scene = 0, .current_group = 0, .scene_valid = 0 };
    esp_zb_on_off_cluster_cfg_t   on_off_cfg   = { .on_off = dimmer_get_on_off() ? 1 : 0 };
    esp_zb_level_cluster_cfg_t    level_cfg    = { .current_level = dimmer_get_level() };

    esp_zb_attribute_list_t *basic_cl    = esp_zb_basic_cluster_create(&basic_cfg);
    esp_zb_attribute_list_t *identify_cl = esp_zb_identify_cluster_create(&identify_cfg);
    esp_zb_attribute_list_t *groups_cl   = esp_zb_groups_cluster_create(&groups_cfg);
    esp_zb_attribute_list_t *scenes_cl   = esp_zb_scenes_cluster_create(&scenes_cfg);
    esp_zb_attribute_list_t *on_off_cl   = esp_zb_on_off_cluster_create(&on_off_cfg);
    esp_zb_attribute_list_t *level_cl    = esp_zb_level_cluster_create(&level_cfg);

    esp_zb_basic_cluster_add_attr(basic_cl, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, ESP_MANUFACTURER_NAME);
    esp_zb_basic_cluster_add_attr(basic_cl, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,  ESP_MODEL_IDENTIFIER);
    esp_zb_basic_cluster_add_attr(basic_cl, ESP_ZB_ZCL_ATTR_BASIC_SW_BUILD_ID,          OTA_SW_BUILD_ID);

    esp_zb_cluster_list_t *cl = esp_zb_zcl_cluster_list_create();
    esp_zb_cluster_list_add_basic_cluster(cl,    basic_cl,    ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_identify_cluster(cl, identify_cl, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_groups_cluster(cl,   groups_cl,   ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_scenes_cluster(cl,   scenes_cl,   ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_on_off_cluster(cl,   on_off_cl,   ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_level_cluster(cl,    level_cl,    ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

#if ZB_OTA_ENABLED
    esp_zb_cluster_list_add_ota_cluster(cl, ota_cluster_create(), ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
#endif
    if (ep == s_ota_ep)
        add_diag_cluster(cl);

    esp_zb_endpoint_config_t epcfg = {
        .endpoint           = ep,
        .app_profile_id     = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id      = ESP_ZB_HA_DIMMABLE_LIGHT_DEVICE_ID,
        .app_device_version = 0,
    };
    esp_zb_ep_list_add_ep(ep_list, cl, epcfg);
    ESP_LOGI(TAG, "EP%d: Dimmable Light dimmer (CH0)", ep);
}

/* ── Creazione tutti gli endpoint ────────────────────────────────────── */
static void create_endpoints(esp_zb_ep_list_t *ep_list)
{
    for (int i = 0; i < 4; i++) {
        uint8_t ep = s_ch_to_ep[i];
        if (ep == 0) continue;   /* canale non assegnato */
        switch (g_sw_cfg.ch_type[i]) {
        case CH_RELAY_STABLE:
        case CH_RELAY_IMPULSE:
            add_relay_endpoint(ep_list, ep, (uint8_t)i);
            break;
        case CH_SHUTTER:
            add_shutter_endpoint(ep_list, ep, (uint8_t)i);
            break;
        case CH_DIMMER:
            add_dimmer_endpoint(ep_list, ep);
            break;
        default:
            break;
        }
    }
    add_virtual_switch_endpoint(ep_list, BLE_SWITCH_EP,     "aggiornamento BLE",       false);
    add_virtual_switch_endpoint(ep_list, PROG_SWITCH_EP,    "modalità programmazione", false);
    add_virtual_switch_endpoint(ep_list, FACTORY_SWITCH_EP, "factory reset Zigbee",    false);
    add_virtual_switch_endpoint(ep_list, LOG_SWITCH_EP,     "log via BLE",             ble_log_enabled());
}

/* ── Handler comandi Zigbee ──────────────────────────────────────────── */

/* Factory reset Zigbee: cancella le partizioni Zigbee e riavvia. Non ritorna.
 * Cancelliamo direttamente le credenziali (NVS "zigbee" + zb_fct, ZB2) senza passare
 * dallo stack Zigbee (che richiederebbe il suo task). La configurazione NVS resta intatta. */
static void zigbee_factory_reset_and_restart(void)
{
    ESP_LOGW(TAG, "*** FACTORY RESET ZIGBEE — cancello credenziali... ***");
    /* ZB2: causa scritta prima, cancellazione subito prima del riavvio (vedi zigbee_hard_reset) */
    diag_set_restart_cause(DIAG_CAUSE_FACTORY);
    maint_erase_zigbee_credentials(false);
    ESP_LOGW(TAG, "Riavvio...");
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_restart();
}

/* L'azione parte da un task a parte, così la risposta Zigbee al comando fa in tempo a partire. */
typedef void (*maint_action_t)(void);

static void maint_action_task(void *pv)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    ((maint_action_t)pv)();
    vTaskDelete(NULL);
}

static void log_switch_task(void *pv)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    ble_log_set_enabled_and_restart((bool)(uintptr_t)pv);
    vTaskDelete(NULL);
}

/* Report singolo On/Off degli EP virtuali verso il coordinator (0x0000, endpoint 1 di ZHA), come
 * diag_zb_report_to_coordinator: senza, dopo un riavvio HA continua a mostrare l'ultimo valore
 * (es. EP10 ON dopo l'aggiornamento BLE), perché l'attributo riparte al valore di creazione
 * (OFF, EP13 = stato del log BLE) senza nessuna variazione che generi un report.
 * Chiamato dalla callback del ping (contesto Zigbee): niente lock. */
static void report_virtual_switches(void)
{
    static const uint8_t eps[] = { BLE_SWITCH_EP, PROG_SWITCH_EP, FACTORY_SWITCH_EP, LOG_SWITCH_EP };
    for (size_t i = 0; i < sizeof(eps); i++) {
        esp_zb_zcl_report_attr_cmd_t cmd = {0};
        cmd.zcl_basic_cmd.dst_addr_u.addr_short = 0x0000;
        cmd.zcl_basic_cmd.dst_endpoint = 1;
        cmd.zcl_basic_cmd.src_endpoint = eps[i];
        cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
        cmd.clusterID = ESP_ZB_ZCL_CLUSTER_ID_ON_OFF;
        cmd.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI;
        cmd.manuf_code = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC;
        cmd.attributeID = ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID;
        esp_err_t err = esp_zb_zcl_report_attr_cmd_req(&cmd);
        ESP_LOGI(TAG, "EP%d: report stato interruttore virtuale: %s", eps[i], esp_err_to_name(err));
    }
}

static esp_err_t handle_virtual_switch(const esp_zb_zcl_set_attr_value_message_t *msg)
{
    static bool requested = false;
    if (msg->info.cluster != ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) return ESP_OK;
    if (msg->attribute.id != ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) return ESP_OK;
    if (msg->attribute.data.type != ESP_ZB_ZCL_ATTR_TYPE_BOOL) return ESP_OK;
    if (requested) return ESP_OK;
    const bool on = *(bool *)msg->attribute.data.value;

    /* Il log via BLE si attiva e si disattiva; gli altri reagiscono solo a ON. */
    if (msg->info.dst_endpoint == LOG_SWITCH_EP) {
        if (on == ble_log_enabled()) return ESP_OK;
        requested = true;
        ESP_LOGW(TAG, "Zigbee: log via BLE %s — riavvio tra 1.5 s", on ? "ON" : "OFF");
        xTaskCreate(log_switch_task, "log_sw", 4096, (void *)(uintptr_t)on, 5, NULL);
        return ESP_OK;
    }
    if (!on) return ESP_OK;

    maint_action_t action;
    const char *what;
    switch (msg->info.dst_endpoint) {
    case BLE_SWITCH_EP:
        action = ble_update_request_and_restart; what = "modalità aggiornamento BLE"; break;
    case PROG_SWITCH_EP:
        action = maint_enter_download_mode;      what = "modalità programmazione";    break;
    case FACTORY_SWITCH_EP:
        action = zigbee_factory_reset_and_restart; what = "factory reset Zigbee";     break;
    default:
        return ESP_OK;
    }
    requested = true;
    ESP_LOGW(TAG, "Zigbee: richiesta %s — avvio tra 1.5 s", what);
    xTaskCreate(maint_action_task, "maint_act", 4096, action, 5, NULL);
    return ESP_OK;
}

static esp_err_t handle_on_off(const esp_zb_zcl_set_attr_value_message_t *msg)
{
    uint8_t ep  = msg->info.dst_endpoint;
    if (IS_VIRTUAL_EP(ep)) return handle_virtual_switch(msg);   /* prima di indicizzare s_ep_to_* */
    uint8_t ch  = s_ep_to_ch[ep];
    uint8_t typ = s_ep_to_type[ep];
    if (ch == 0xFF) return ESP_OK;
    if (msg->info.cluster != ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) return ESP_OK;
    if (msg->attribute.id != ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) return ESP_OK;
    if (msg->attribute.data.type != ESP_ZB_ZCL_ATTR_TYPE_BOOL) return ESP_OK;

    bool state = *(bool *)msg->attribute.data.value;
    ESP_LOGI(TAG, "Zigbee On/Off → EP%d CH%d tipo=%d stato=%s",
             ep, ch, typ, state ? "ON" : "OFF");

    if (typ == CH_DIMMER) {
        if (state) dimmer_on(); else dimmer_off();
    } else {
        relay_set(ch, state);
    }
    return ESP_OK;
}

static esp_err_t handle_level(const esp_zb_zcl_set_attr_value_message_t *msg)
{
    if (msg->info.cluster != ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL) return ESP_OK;
    if (msg->attribute.id != ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID) return ESP_OK;

    uint8_t level = *(uint8_t *)msg->attribute.data.value;
    dimmer_set_level(level);
    if (level > 0 && !dimmer_get_on_off()) dimmer_on();
    return ESP_OK;
}

static esp_err_t handle_window_covering(const esp_zb_zcl_window_covering_movement_message_t *msg)
{
    uint8_t ep = msg->info.dst_endpoint;
    uint8_t ch = s_ep_to_ch[ep];
    if (ch == 0xFF) return ESP_OK;

    switch (msg->command) {
    case ESP_ZB_ZCL_CMD_WINDOW_COVERING_UP_OPEN:
        shutter_command(ch, SHUTTER_CMD_UP, 0);   break;
    case ESP_ZB_ZCL_CMD_WINDOW_COVERING_DOWN_CLOSE:
        shutter_command(ch, SHUTTER_CMD_DOWN, 0); break;
    case ESP_ZB_ZCL_CMD_WINDOW_COVERING_STOP:
        shutter_command(ch, SHUTTER_CMD_STOP, 0); break;
    case ESP_ZB_ZCL_CMD_WINDOW_COVERING_GO_TO_LIFT_PERCENTAGE:
        shutter_command(ch, SHUTTER_CMD_GOTO, msg->payload.percentage_lift_value); break;
    default:
        break;
    }
    return ESP_OK;
}

static esp_err_t zb_attribute_handler(const esp_zb_zcl_set_attr_value_message_t *msg)
{
    ESP_RETURN_ON_FALSE(msg, ESP_FAIL, TAG, "msg vuoto");
    ESP_RETURN_ON_FALSE(msg->info.status == ESP_ZB_ZCL_STATUS_SUCCESS,
        ESP_ERR_INVALID_ARG, TAG, "status errore");

    if (msg->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF)
        return handle_on_off(msg);
    if (msg->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL)
        return handle_level(msg);
    return ESP_OK;
}

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t cb_id, const void *msg)
{
    switch (cb_id) {
    case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID:
        return zb_attribute_handler((esp_zb_zcl_set_attr_value_message_t *)msg);
    case ESP_ZB_CORE_WINDOW_COVERING_MOVEMENT_CB_ID:
        return handle_window_covering((esp_zb_zcl_window_covering_movement_message_t *)msg);
#if ZB_OTA_ENABLED
    case ESP_ZB_CORE_OTA_UPGRADE_VALUE_CB_ID: {
        const esp_zb_zcl_ota_upgrade_value_message_t *ota =
            (esp_zb_zcl_ota_upgrade_value_message_t *)msg;
        if (ota->upgrade_status == ESP_ZB_ZCL_OTA_UPGRADE_STATUS_START)
            led_set_state(LED_OTA_IN_PROGRESS);
        return ota_upgrade_handler(ota);
    }
    case ESP_ZB_CORE_OTA_UPGRADE_QUERY_IMAGE_RESP_CB_ID: {
        const esp_zb_zcl_ota_upgrade_query_image_resp_message_t *r = msg;
        if (r->query_status == ESP_ZB_ZCL_STATUS_SUCCESS)
            ESP_LOGI(TAG, "OTA: nuova immagine v0x%08"PRIx32" (%"PRIu32" byte)",
                     r->file_version, r->image_size);
        else
            ESP_LOGI(TAG, "OTA: nessuna nuova immagine (0x%02x)", r->query_status);
        return ESP_OK;
    }
#endif
    case ESP_ZB_CORE_CMD_DEFAULT_RESP_CB_ID:
        return ESP_OK;
    default:
        ESP_LOGW(TAG, "Azione non gestita: 0x%x", cb_id);
        return ESP_OK;
    }
}

/* ── Segnali Zigbee ──────────────────────────────────────────────────── */
static void bdb_start_cb(uint8_t mode_mask)
{
    ESP_RETURN_ON_FALSE(
        esp_zb_bdb_start_top_level_commissioning(mode_mask) == ESP_OK,
        , TAG, "Errore commissioning BDB");
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *sig)
{
    uint32_t *p          = sig->p_app_signal;
    esp_err_t err        = sig->esp_err_status;
    esp_zb_app_signal_type_t type = *p;

    switch (type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Stack inizializzato");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;

    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Avvio steering...");
            led_set_state(LED_ZIGBEE_SEARCHING);
            esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
        } else {
            ESP_LOGW(TAG, "Init fallita, riprovo...");
            led_set_state(LED_ERROR);
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_cb,
                                   ESP_ZB_BDB_MODE_INITIALIZATION, 1000);
        }
        break;

    case ESP_ZB_ZDO_SIGNAL_LEAVE:
        diag_event(DIAG_EVT_LEAVE);
        zigbee_ready            = false;
        s_coordinator_confirmed = false;
        ESP_LOGW(TAG, "Segnale LEAVE ricevuto — riprovo steering tra 30s...");
        led_set_state(LED_ZIGBEE_SEARCHING);
        esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_cb,
                               ESP_ZB_BDB_MODE_NETWORK_STEERING, 30000);
        break;

    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err == ESP_OK) {
            /* Sanity check: se l'indirizzo è 0xFFFF il radio è in stato corrotto
             * (bug ZBOSS #727 — steering segnala successo ma il radio non è
             * effettivamente associato). Stato non recuperabile → hard reset. */
            if (esp_zb_get_short_address() == 0xFFFF) {
                zigbee_hard_reset("addr=0xFFFF");
            }
            s_steering_fail_count = 0;
            zigbee_ready = true;
            ESP_LOGI(TAG, "Rete trovata: canale %d, PAN 0x%04hx, addr 0x%04hx — verifico coordinator...",
                     esp_zb_get_current_channel(),
                     esp_zb_get_pan_id(),
                     esp_zb_get_short_address());
            /* LED rimane in SEARCHING: diventa operativo solo dopo che il ping ZDO
             * al coordinator ha avuto risposta (in coordinator_ping_result). */
            s_ping_tick = COORDINATOR_PING_TICKS - 1;   /* prossimo tick → ping subito */
            /* ZB2: unico punto di avvio del keepalive. Annulla un eventuale tick già in coda,
             * altrimenti in 2.x le catene si sommano (viste due catene, ping ogni ~2,5 min). */
            esp_zb_scheduler_alarm_cancel(coordinator_ping_cb, 0);
            esp_zb_scheduler_alarm(coordinator_ping_cb, 0, 1000);
        } else {
            s_steering_fail_count++;
            diag_event(DIAG_EVT_STEERING_FAIL);
            ESP_LOGW(TAG, "Steering fallito (%d/%d), riprovo tra 30s...",
                     s_steering_fail_count, STEERING_MAX_FAILS);
            if (s_steering_fail_count >= STEERING_MAX_FAILS) {
                /* Calibrazione PHY probabilmente corrotta (interferenza RF).
                 * PHY reset: cancella phy_init e riavvia — credenziali intatte,
                 * il device farà un rejoin senza permit join. */
                zigbee_phy_reset("steering loop");
            }
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_cb,
                                   ESP_ZB_BDB_MODE_NETWORK_STEERING, 30000);
        }
        break;

    case ESP_ZB_NWK_SIGNAL_NO_ACTIVE_LINKS_LEFT:
        diag_event(DIAG_EVT_NO_LINKS);
        if (zigbee_ready) {   /* guard: evita scheduling multiplo */
            zigbee_ready            = false;
            s_coordinator_confirmed = false;
            ESP_LOGW(TAG, "Nessun link attivo (0x18) — forzo riconnessione");
            led_set_state(LED_ZIGBEE_SEARCHING);
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_cb,
                                   ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
        }
        break;

    case ESP_ZB_NLME_STATUS_INDICATION:
        ESP_LOGD(TAG, "NLME Status Indication (0x32) — ignorato");
        break;

    case ESP_ZB_ZDO_DEVICE_UNAVAILABLE:
        ESP_LOGD(TAG, "ZDO Device Unavailable (0x3c) — ignorato");
        break;

    default:
        ESP_LOGI(TAG, "Segnale: %s (0x%x) %s",
                 esp_zb_zdo_signal_to_string(type), type, esp_err_to_name(err));
        break;
    }
}

/* ── Keepalive ZDO verso il coordinator (island detection) ──────────── */
/*
 * Ogni 5 minuti (5 tick × COORDINATOR_PING_TICK_MS) il dispositivo invia un ZDO
 * ieee_addr_req al coordinator (0x0000).
 *
 * - Se risponde: la prima volta imposta LED operativo, sync stati, OTA.
 *   Le volte successive è solo un keepalive silenzioso.
 * - Se non risponde: zigbee_ready = false, LED searching, steering tra 30s.
 *   Dopo lo steering, il ping riparte subito (1s) per verificare se il
 *   coordinator è davvero raggiungibile prima di tornare operativo.
 *
 * Tutto gira nel task Zigbee tramite scheduler_alarm → nessun lock necessario.
 */
static void coordinator_ping_result(esp_zb_zdp_status_t zdo_status,
                                    esp_zb_zdo_ieee_addr_rsp_t *resp,
                                    void *user_ctx)
{
    s_ping_in_flight = false;   /* callback ricevuta (o timeout forzato) */
    if (!zigbee_ready) return;

    if (zdo_status == ESP_ZB_ZDP_STATUS_SUCCESS) {
        s_ping_tick       = 0;
        s_ping_fail_count = 0;
        ESP_LOGD(TAG, "Keepalive coordinator: OK");

        if (!s_coordinator_confirmed) {
            /* Prima conferma dopo un (re)join: diventiamo operativi */
            s_coordinator_confirmed = true;
            ESP_LOGI(TAG, "Coordinator raggiungibile — operativo");
            led_set_state(LED_OPERATIONAL);
            ota_find_server();
            /* Dice a Home Assistant quanti riavvii anomali ci sono stati (allarme). */
            diag_zb_report_to_coordinator(s_ota_ep);
            /* Stato reale degli interruttori virtuali EP10-13 (dopo un riavvio HA mostrava l'ultimo). */
            report_virtual_switches();
        }

        /* Sync attributi con report=true: aggiorna ZHA e stimola route discovery.
         * Eseguito ad ogni ping riuscito (non solo al primo join) per mantenere
         * le route attive e il coordinator aggiornato sullo stato reale. */
        for (int i = 0; i < 4; i++) {
            uint8_t ep = s_ch_to_ep[i];
            if (ep == 0) continue;
            if (g_sw_cfg.ch_type[i] == CH_RELAY_STABLE ||
                g_sw_cfg.ch_type[i] == CH_RELAY_IMPULSE) {
                uint8_t val = relay_get((uint8_t)i) ? 1 : 0;
                esp_zb_zcl_set_attribute_val(ep,
                    ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
                    ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                    ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID,
                    &val, true);
            } else if (g_sw_cfg.ch_type[i] == CH_SHUTTER) {
                uint8_t pos = shutter_get_position((uint8_t)i);
                esp_zb_zcl_set_attribute_val(ep,
                    ESP_ZB_ZCL_CLUSTER_ID_WINDOW_COVERING,
                    ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                    ESP_ZB_ZCL_ATTR_WINDOW_COVERING_CURRENT_POSITION_LIFT_PERCENTAGE_ID,
                    &pos, true);
            }
        }

        /* Prossimo keepalive tra 5 minuti */
        esp_zb_scheduler_alarm(coordinator_ping_cb, 0, COORDINATOR_PING_TICK_MS);
    } else {
        /* Coordinator non raggiungibile */
        s_ping_fail_count++;
        diag_event(DIAG_EVT_PING_FAIL);
        ESP_LOGW(TAG, "Coordinator irraggiungibile (0x%02x) — fail %d/%d",
                 zdo_status, s_ping_fail_count, COORDINATOR_PING_MAX_FAILS);
        zigbee_ready            = false;
        s_coordinator_confirmed = false;
        s_ping_tick             = 0;
        led_set_state(LED_ZIGBEE_SEARCHING);

        if (s_ping_fail_count >= COORDINATOR_PING_MAX_FAILS) {
            /* Disconnessione definitiva (tipicamente per interferenze RF):
             * i retry normali non bastano. Hard reset: cancella lo storage
             * Zigbee e riavvia per un join pulito. */
            zigbee_hard_reset("ping fail");
        }

        esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_cb,
                               ESP_ZB_BDB_MODE_NETWORK_STEERING, 30000);
    }
}

/* Timeout di sicurezza: se la callback ZDO non arriva entro COORDINATOR_PING_TIMEOUT_MS
 * (es. route error anziché ZDP timeout), forziamo il fallimento noi stessi.
 * Senza questo, il dispositivo resterebbe bloccato in s_coordinator_confirmed=false
 * con nessun altro allarme schedulato — doppio lampeggio fisso per sempre. */
static void coordinator_ping_timeout_cb(uint8_t param)
{
    (void)param;
    if (!s_ping_in_flight) return;   /* callback già ricevuta normalmente */
    ESP_LOGW(TAG, "Keepalive: nessuna risposta ZDO in %ds — forzo fallimento",
             COORDINATOR_PING_TIMEOUT_MS / 1000);
    coordinator_ping_result(ESP_ZB_ZDP_STATUS_TIMEOUT, NULL, NULL);
}

static void coordinator_ping_cb(uint8_t param)
{
    (void)param;
    if (!zigbee_ready) return;

    s_ping_tick++;
    if (s_ping_tick < COORDINATOR_PING_TICKS) {
        /* Non ancora il momento del ping: aspetta il prossimo tick */
        esp_zb_scheduler_alarm(coordinator_ping_cb, 0, COORDINATOR_PING_TICK_MS);
        return;
    }
    s_ping_tick = 0;

    esp_zb_zdo_ieee_addr_req_param_t req = {
        .dst_nwk_addr     = 0x0000,
        .addr_of_interest = 0x0000,
        .request_type     = 0,
        .start_index      = 0,
    };
    ESP_LOGD(TAG, "Keepalive: ZDO ieee_addr_req → coordinator");
    s_ping_in_flight = true;
    esp_zb_zdo_ieee_addr_req(&req, coordinator_ping_result, NULL);
    /* Timeout di sicurezza: se route error (non ZDP timeout), la callback
     * non viene mai chiamata. L'allarme la simula dopo 40s. */
    esp_zb_scheduler_alarm(coordinator_ping_timeout_cb, 0, COORDINATOR_PING_TIMEOUT_MS);
}

/* ── Watchdog applicativo Zigbee ─────────────────────────────────────── */
/*
 * La callback zb_wdt_feed_cb viene schedulata ogni 60s dallo scheduler
 * Zigbee. Se lo stack si blocca silenziosamente, la callback smette di
 * essere chiamata. Il task zb_watchdog_task verifica ogni 30s che il feed
 * sia arrivato; se manca da più di 3 minuti forza esp_restart().
 *
 * Tutto si avvia in esp_zb_task() dopo esp_zb_start(), quindi solo quando
 * lo stack è già attivo.
 */
#define ZB_WDT_FEED_INTERVAL_MS   60000    /* feed ogni 60s                */
#define ZB_WDT_TIMEOUT_MS         180000   /* reset se fermo > 3 min       */
#define ZB_WDT_CHECK_INTERVAL_MS  30000    /* check ogni 30s               */

static volatile TickType_t s_zb_last_feed = 0;

static void zb_wdt_feed_cb(uint8_t param)
{
    (void)param;
    s_zb_last_feed = xTaskGetTickCount();
    diag_zb_update(s_ota_ep);
    esp_zb_scheduler_alarm(zb_wdt_feed_cb, 0, ZB_WDT_FEED_INTERVAL_MS);
}

static void zb_watchdog_task(void *pv)
{
    /* Aspetta che lo stack parta e il primo feed sia registrato */
    vTaskDelay(pdMS_TO_TICKS(ZB_WDT_FEED_INTERVAL_MS + 30000));

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(ZB_WDT_CHECK_INTERVAL_MS));
        TickType_t elapsed = xTaskGetTickCount() - s_zb_last_feed;
        if (elapsed > pdMS_TO_TICKS(ZB_WDT_TIMEOUT_MS)) {
            ESP_LOGE("ZB_WDT", "Stack Zigbee non risponde da %lu s — deep sleep 3s",
                     (unsigned long)(elapsed * portTICK_PERIOD_MS / 1000));
            diag_set_restart_cause(DIAG_CAUSE_ZB_WDT);
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_sleep_enable_timer_wakeup(3ULL * 1000 * 1000);
            esp_deep_sleep_start();
        }
    }
}

/* ── Factory reset Zigbee (pulsante BOOT / GPIO9) ───────────────────── */
/*
 * Un task dedicato monitora GPIO9 in continuazione. Se il pulsante viene
 * tenuto premuto per FACTORY_RESET_HOLD_S secondi (in qualsiasi momento,
 * non solo all'avvio), il firmware esegue esp_zb_factory_reset() e
 * riavvia il dispositivo. La configurazione NVS resta intatta.
 *
 * Feedback LED: durante la pressione il LED passa a LED_FACTORY_RESET
 * (acceso fisso). Se rilasci prima del tempo, torna allo stato precedente.
 */
#define FACTORY_RESET_GPIO       GPIO_NUM_9
#define FACTORY_RESET_HOLD_S     5

static void factory_reset_task(void *pv)
{
    gpio_config_t btn = {
        .pin_bit_mask = (1ULL << FACTORY_RESET_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn);

    /* Ignora i primi 10s: durante il boot il chip USB-seriale può tenere
       DTR/RTS basso → GPIO9 risulta falsamente premuto */
    vTaskDelay(pdMS_TO_TICKS(10000));

    for (;;) {
        /* Aspetta che GPIO9 venga premuto (attivo-basso) */
        if (gpio_get_level(FACTORY_RESET_GPIO) != 0) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        /* Premuto — inizia conteggio */
        ESP_LOGW(TAG, "GPIO9 premuto — tieni premuto %ds per factory reset Zigbee...",
                 FACTORY_RESET_HOLD_S);
        led_set_state(LED_FACTORY_RESET);

        bool held = true;
        int held_ticks = 0;
        for (int i = 0; i < FACTORY_RESET_HOLD_S * 10; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
            if (gpio_get_level(FACTORY_RESET_GPIO) != 0) {
                held = false;
                held_ticks = i + 1;
                break;
            }
        }

        if (!held) {
            /* Pressione breve (0.3 s - 5 s) con il log via BLE attivo: lo disattiva e riavvia. */
            if (held_ticks >= 3 && ble_log_enabled()) {
                ESP_LOGW(TAG, "GPIO9: pressione breve — disattivo il log via BLE");
                ble_log_set_enabled_and_restart(false);
            }
            ESP_LOGI(TAG, "GPIO9 rilasciato — annullato");
            led_set_state(zigbee_ready ? LED_OPERATIONAL : LED_ZIGBEE_SEARCHING);
            continue;
        }

        /* Tenuto abbastanza — factory reset Zigbee e riavvio */
        zigbee_factory_reset_and_restart();
    }
}

/* ── Task Zigbee ─────────────────────────────────────────────────────── */
static void esp_zb_task(void *pv)
{
    s_zb_task = xTaskGetCurrentTaskHandle();   /* ZB2: per zb_alarm_any_task() */
    esp_zb_cfg_t cfg = ESP_ZB_ZR_CONFIG();
    esp_zb_init(&cfg);

    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    create_endpoints(ep_list);

    esp_zb_device_register(ep_list);
    esp_zb_core_action_handler_register(zb_action_handler);
    esp_zb_set_primary_network_channel_set(ESP_ZB_PRIMARY_CHANNEL_MASK);

    ESP_ERROR_CHECK(esp_zb_start(false));

    /* Avvia il feed periodico del watchdog applicativo */
    s_zb_last_feed = xTaskGetTickCount();
    esp_zb_scheduler_alarm(zb_wdt_feed_cb, 0, ZB_WDT_FEED_INTERVAL_MS);

    /* ZB2: il keepalive ZDO verso il coordinator (island detection) parte solo quando il
     * dispositivo entra in rete (ESP_ZB_BDB_SIGNAL_STEERING riuscito), non più anche qui. */

    esp_zb_stack_main_loop();
}

/* ── Entry point ─────────────────────────────────────────────────────── */
/* ZB2: migrazione una tantum dei dati Zigbee 1.x (zb_storage, FAT) → 2.x (nvs, namespace "zigbee").
 * Come l'esempio Espressif examples/nvram_migration: dopo nvs_flash_init, prima dell'avvio dello stack.
 * La libreria non tocca zb_storage; il marcatore "zb2_mig/done" evita di ripeterla.
 * Diverso dall'esempio: un solo tentativo, riuscito o no, e nessun blocco dell'avvio se fallisce
 * (il valore restituito con zb_storage vuota, cioè modulo flashato da zero, non è documentato). */
static void zb2_migrate_legacy_nvram(void)
{
    nvs_handle_t h;
    if (nvs_open("zb2_mig", NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "migrazione 1.x: nvs_open fallito, salto");
        return;
    }
    uint8_t done = 0;
    if (nvs_get_u8(h, "done", &done) == ESP_OK) {
        nvs_close(h);
        return;                                       /* già fatta */
    }
    esp_err_t err = esp_zigbee_nvram_migrate(NULL);   /* default: zb_storage → nvs, IEEE = MAC */
    ESP_LOGW(TAG, "migrazione dati Zigbee 1.x → 2.x: %s", esp_err_to_name(err));
    nvs_set_u8(h, "done", 1);                         /* un solo tentativo, riuscito o no */
    nvs_commit(h);
    nvs_close(h);
}

void app_main(void)
{
    /* ── Controllo reset reason ──────────────────────────────────────────
     * Solo POWERON, DEEPSLEEP e EXT garantiscono il radio IEEE 802.15.4 in
     * stato pulito. Qualsiasi altro reset (SW_CPU dall'handler del brownout,
     * BROWNOUT hardware, PANIC, WDT) può lasciare il radio in uno stato
     * degradato che impedisce il rejoin anche se lo stack segnala successo.
     * In quel caso forziamo un deep sleep di 3s: il wakeup è equivalente a
     * un power cycle (rst:0x5 DEEPSLEEP) e garantisce la reinizializzazione
     * completa del radio prima di tentare qualsiasi connessione Zigbee. */
    /* NVS prima del controllo: la diagnostica deve registrare il motivo del reset
     * prima dell'eventuale deep sleep di recupero. */
    ESP_ERROR_CHECK(nvs_flash_init());
    /* ZB2: esp-zigbee-lib 2.0.5 salva i dati Zigbee nella partizione "nvs" (namespace "zigbee"),
     * già inizializzata qui sopra. zb_storage resta FAT come in 1.x e non viene usata dallo stack. */

    {
        esp_reset_reason_t rr = esp_reset_reason();
        diag_boot(rr);
        if (rr != ESP_RST_POWERON &&
            rr != ESP_RST_DEEPSLEEP &&
            rr != ESP_RST_EXT) {
            if (diag_restart_was_commanded()) {
                /* Riavvio deciso dall'utente (OTA, BLE, factory reset): avvio normale. */
                ESP_LOGI(TAG, "Riavvio comandato (reason=%d) — avvio normale, niente deep sleep", (int)rr);
            } else {
                ESP_LOGW(TAG, "Reset non-pulito (reason=%d) — deep sleep 3s per reset radio", (int)rr);
                esp_sleep_enable_timer_wakeup(3ULL * 1000 * 1000);
                esp_deep_sleep_start();
            }
        }
    }

    /* Modalità aggiornamento BLE: niente Zigbee, niente relè (le uscite restano a riposo). */
    if (ble_update_pending()) {
        ble_update_run();
    }

    /* ZB2: prima dell'avvio dello stack, recupera le credenziali di un modulo aggiornato da 1.x */
    zb2_migrate_legacy_nvram();

    esp_zb_platform_config_t config = {
        .radio_config = ESP_ZB_DEFAULT_RADIO_CONFIG(),
        .host_config  = ESP_ZB_DEFAULT_HOST_CONFIG(),
    };

    ESP_ERROR_CHECK(esp_zb_platform_config(&config));
    diag_start_task();
    maint_serial_start();
    ble_log_start_if_enabled();

    /* Anti-rollback OTA */
    ota_mark_valid();

    /* Carica configurazione da NVS */
    sw_cfg_load();

    /* Mappa endpoint → canale (runtime, da g_sw_cfg) */
    init_ep_map();

    /* LED di stato */
    led_init();

    /* Task monitor GPIO9 (BOOT) — factory reset Zigbee se premuto 5s */
    xTaskCreate(factory_reset_task, "factory_rst", 2048, NULL, 1, NULL);

    /* Task watchdog applicativo — riavvia se lo stack Zigbee si blocca */
    xTaskCreate(zb_watchdog_task, "zb_wdt", 2048, NULL, 2, NULL);

    /* ISR service GPIO — chiamato una volta sola */
    gpio_install_isr_service(0);

    /* Inizializza moduli hardware attivi */
    if (g_sw_cfg.has_dimmer)  dimmer_init(on_dimmer_changed);
    if (g_sw_cfg.has_relay)   relay_init(on_relay_changed);
    if (g_sw_cfg.has_shutter) shutter_init(on_shutter_changed);

    /* Avvia stack Zigbee */
    xTaskCreate(esp_zb_task, "Zigbee_main", 8192, NULL, 5, NULL);
}
