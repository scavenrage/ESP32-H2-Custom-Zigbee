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
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_zigbee_core.h"
#include "esp_zigbee_cluster.h"
#include "esp_zigbee_ota.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "zdo/esp_zigbee_zdo_command.h"
#include <inttypes.h>
#include <string.h>

static const char *TAG = "MAIN";

/* ── Configurazione Zigbee ───────────────────────────────────────────── */
#define INSTALLCODE_POLICY_ENABLE    false
#define MAX_CHILDREN                 10
#define ESP_ZB_PRIMARY_CHANNEL_MASK  ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK
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

static bool zigbee_ready = false;

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
    esp_zb_ota_upgrade_client_query_interval_set(s_ota_ep, OTA_UPGRADE_QUERY_INTERVAL);
    esp_zb_ota_upgrade_client_query_image_req(0x0000, 1);
    ESP_LOGI(TAG, "OTA: query → coordinator, polling ogni %d min", OTA_UPGRADE_QUERY_INTERVAL);
}

/* ── Callback hardware → Zigbee ──────────────────────────────────────── */

/*
 * Contesti:
 *   1. Task esterno (input_task, button_task): usare esp_zb_lock_acquire/release
 *   2. Task Zigbee (signal handler, tramite scheduler_alarm): lock già tenuto
 *
 * on_relay_changed è chiamato sia da input_task (contesto esterno) sia dal
 * timer daemon (relay a impulso). In entrambi i casi siamo fuori dal task Zigbee,
 * quindi acquisire il lock è corretto.
 */
static void on_relay_changed(uint8_t ch, bool state)
{
    if (!zigbee_ready) return;
    uint8_t ep  = s_ch_to_ep[ch];
    uint8_t val = state ? 1 : 0;
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_set_attribute_val(ep,
        ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID,
        &val, false);
    esp_zb_lock_release();
}

/* Chiamata da shutter.c — può venire da input_task (lock) o da signal handler
 * tramite shutter_sync_all (già in task Zigbee, NON acquisire lock).
 * Per semplicità: shutter_sync_all viene chiamato tramite scheduler_alarm,
 * quindi siamo nel task Zigbee → lock non necessario. */
static void on_shutter_changed(uint8_t ch, uint8_t pos)
{
    if (!zigbee_ready) return;
    uint8_t ep = s_ch_to_ep[ch];
    esp_zb_zcl_set_attribute_val(ep,
        ESP_ZB_ZCL_CLUSTER_ID_WINDOW_COVERING,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_WINDOW_COVERING_CURRENT_POSITION_LIFT_PERCENTAGE_ID,
        &pos, false);
}

/* Chiamata da dimmer.c (button_task) — task esterno: acquisire il lock */
static void on_dimmer_changed(bool on_off, uint8_t level)
{
    if (!zigbee_ready) return;
    uint8_t ep     = s_ch_to_ep[0];
    uint8_t on_val = on_off ? 1 : 0;
    esp_zb_lock_acquire(portMAX_DELAY);
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
    esp_zb_lock_release();
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

/* ── Costruzione endpoint On/Off (relay) ─────────────────────────────── */
static void add_relay_endpoint(esp_zb_ep_list_t *ep_list, uint8_t ep, uint8_t ch)
{
    esp_zb_cluster_list_t *cl = esp_zb_zcl_cluster_list_create();

    esp_zb_cluster_list_add_basic_cluster(cl, create_basic_cluster(),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_attribute_list_t *id_cl = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_IDENTIFY);
    uint8_t id_time = 0;
    esp_zb_identify_cluster_add_attr(id_cl, ESP_ZB_ZCL_ATTR_IDENTIFY_IDENTIFY_TIME_ID, &id_time);
    esp_zb_cluster_list_add_identify_cluster(cl, id_cl, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_on_off_cluster_cfg_t oo = { .on_off = 0 };
    esp_zb_cluster_list_add_on_off_cluster(cl,
        esp_zb_on_off_cluster_create(&oo), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    if (ep == s_ota_ep)
        esp_zb_cluster_list_add_ota_cluster(cl,
            ota_cluster_create(), ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

    esp_zb_endpoint_config_t epcfg = {
        .endpoint           = ep,
        .app_profile_id     = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id      = ESP_ZB_HA_ON_OFF_OUTPUT_DEVICE_ID,
        .app_device_version = 0,
    };
    esp_zb_ep_list_add_ep(ep_list, cl, epcfg);
    ESP_LOGI(TAG, "EP%d: On/Off relay (CH%d)", ep, ch);
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

    if (ep == s_ota_ep)
        esp_zb_cluster_list_add_ota_cluster(cl,
            ota_cluster_create(), ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

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

    esp_zb_cluster_list_add_ota_cluster(cl, ota_cluster_create(), ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

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
}

/* ── Handler comandi Zigbee ──────────────────────────────────────────── */

static esp_err_t handle_on_off(const esp_zb_zcl_set_attr_value_message_t *msg)
{
    uint8_t ep  = msg->info.dst_endpoint;
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

    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err == ESP_OK) {
            zigbee_ready = true;
            ESP_LOGI(TAG, "Connesso: canale %d, PAN 0x%04hx, addr 0x%04hx",
                     esp_zb_get_current_channel(),
                     esp_zb_get_pan_id(),
                     esp_zb_get_short_address());
            led_set_state(LED_OPERATIONAL);
            shutter_sync_all();
            ota_find_server();
        } else {
            ESP_LOGW(TAG, "Steering fallito, riprovo tra 5s...");
            led_set_state(LED_ZIGBEE_SEARCHING);
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_cb,
                                   ESP_ZB_BDB_MODE_NETWORK_STEERING, 5000);
        }
        break;

    default:
        ESP_LOGI(TAG, "Segnale: %s (0x%x) %s",
                 esp_zb_zdo_signal_to_string(type), type, esp_err_to_name(err));
        break;
    }
}

/* ── Task Zigbee ─────────────────────────────────────────────────────── */
static void esp_zb_task(void *pv)
{
    esp_zb_cfg_t cfg = ESP_ZB_ZR_CONFIG();
    esp_zb_init(&cfg);

    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    create_endpoints(ep_list);

    esp_zb_device_register(ep_list);
    esp_zb_core_action_handler_register(zb_action_handler);
    esp_zb_set_primary_network_channel_set(ESP_ZB_PRIMARY_CHANNEL_MASK);

    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop();
}

/* ── Entry point ─────────────────────────────────────────────────────── */
void app_main(void)
{
    esp_zb_platform_config_t config = {
        .radio_config = ESP_ZB_DEFAULT_RADIO_CONFIG(),
        .host_config  = ESP_ZB_DEFAULT_HOST_CONFIG(),
    };

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_zb_platform_config(&config));

    /* Anti-rollback OTA */
    ota_mark_valid();

    /* Carica configurazione da NVS */
    sw_cfg_load();

    /* Mappa endpoint → canale (runtime, da g_sw_cfg) */
    init_ep_map();

    /* LED di stato */
    led_init();

    /* ISR service GPIO — chiamato una volta sola */
    gpio_install_isr_service(0);

    /* Inizializza moduli hardware attivi */
    if (g_sw_cfg.has_dimmer)  dimmer_init(on_dimmer_changed);
    if (g_sw_cfg.has_relay)   relay_init(on_relay_changed);
    if (g_sw_cfg.has_shutter) shutter_init(on_shutter_changed);

    /* Avvia stack Zigbee */
    xTaskCreate(esp_zb_task, "Zigbee_main", 8192, NULL, 5, NULL);
}
