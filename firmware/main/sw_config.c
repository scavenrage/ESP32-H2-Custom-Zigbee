/*
 * sw_config.c — Caricamento configurazione da NVS
 *
 * Namespace NVS: "sw_cfg"
 * Chiavi (max 15 car):
 *   num_ch       u8    numero canali (1-4)
 *   ch0_type     u8    tipo CH0
 *   ch1_type     u8    tipo CH1
 *   ch2_type     u8    tipo CH2
 *   ch3_type     u8    tipo CH3
 *   ch0_in       u8    ingresso CH0 (relay/dimmer)
 *   ch1_in       u8    ingresso CH1
 *   ch2_in       u8    ingresso CH2
 *   ch3_in       u8    ingresso CH3
 *   ch0_in_up    u8    ingresso SU  tapparella A
 *   ch0_in_dn    u8    ingresso GIÙ tapparella A
 *   ch2_in_up    u8    ingresso SU  tapparella B
 *   ch2_in_dn    u8    ingresso GIÙ tapparella B
 *   rel_dbnce    u16   debounce relay (ms)
 *   rel_imp_ms   u16   durata impulso (ms)
 *   sh_ilock     u16   interlock tapparella (ms)
 *   sh_end_ext   u16   extra finecorsa (ms)
 *   sh_dbnce     u16   debounce tapparella (ms)
 *   sh_a_up_ms   u32   salita tapparella A (ms)
 *   sh_a_dn_ms   u32   discesa tapparella A (ms)
 *   sh_b_up_ms   u32   salita tapparella B (ms)
 *   sh_b_dn_ms   u32   discesa tapparella B (ms)
 *   dim_freq     u32   frequenza PWM (Hz)
 *   dim_res      u8    risoluzione PWM (bit)
 *   dim_fade     u16   fade on/off (ms)
 *   dim_lp_ms    u16   long press (ms)
 *   dim_dbnce    u16   debounce dimmer (ms)
 *   dim_step     u8    step dimming
 *   dim_step_ms  u16   intervallo step (ms)
 *   dim_def_lvl  u8    livello predefinito
 *   dim_btn_lvl  u8    BUTTON_ACTIVE_LEVEL
 */

#include "sw_config.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG    = "SW_CFG";
#define NVS_NS             "sw_cfg"

/* ── Istanza globale ──────────────────────────────────────────────────── */
sw_cfg_t g_sw_cfg;

/* ── Macro helper per lettura NVS con fallback al default ─────────────── */
#define NVS_GET_U8(h, key, field, def) \
    do { uint8_t _v = (def); nvs_get_u8(h, key, &_v); (field) = _v; } while(0)

#define NVS_GET_U16(h, key, field, def) \
    do { uint16_t _v = (def); nvs_get_u16(h, key, &_v); (field) = _v; } while(0)

#define NVS_GET_U32(h, key, field, def) \
    do { uint32_t _v = (def); nvs_get_u32(h, key, &_v); (field) = _v; } while(0)

/* ── sw_cfg_load ──────────────────────────────────────────────────────── */
void sw_cfg_load(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS namespace '%s' non trovato (0x%x) — uso default di fabbrica", NVS_NS, err);
        h = 0;
    }

    /* ── Canali ── */
    NVS_GET_U8(h, "num_ch",   g_sw_cfg.num_channels, 1);

    NVS_GET_U8(h, "ch0_type", g_sw_cfg.ch_type[0],   CH_RELAY_STABLE);
    NVS_GET_U8(h, "ch1_type", g_sw_cfg.ch_type[1],   CH_NONE);
    NVS_GET_U8(h, "ch2_type", g_sw_cfg.ch_type[2],   CH_NONE);
    NVS_GET_U8(h, "ch3_type", g_sw_cfg.ch_type[3],   CH_NONE);

    NVS_GET_U8(h, "ch0_in",   g_sw_cfg.ch_in_type[0], INPUT_BUTTON);
    NVS_GET_U8(h, "ch1_in",   g_sw_cfg.ch_in_type[1], INPUT_BUTTON);
    NVS_GET_U8(h, "ch2_in",   g_sw_cfg.ch_in_type[2], INPUT_BUTTON);
    NVS_GET_U8(h, "ch3_in",   g_sw_cfg.ch_in_type[3], INPUT_BUTTON);

    NVS_GET_U8(h, "ch0_in_up", g_sw_cfg.ch_in_up[0], INPUT_BUTTON);
    NVS_GET_U8(h, "ch0_in_dn", g_sw_cfg.ch_in_dn[0], INPUT_BUTTON);
    NVS_GET_U8(h, "ch2_in_up", g_sw_cfg.ch_in_up[2], INPUT_BUTTON);
    NVS_GET_U8(h, "ch2_in_dn", g_sw_cfg.ch_in_dn[2], INPUT_BUTTON);

    /* ── Parametri relay ── */
    NVS_GET_U16(h, "rel_dbnce",  g_sw_cfg.relay_debounce_ms, 200);
    NVS_GET_U16(h, "rel_imp_ms", g_sw_cfg.impulse_ms,        300);

    /* ── Parametri tapparella ── */
    NVS_GET_U16(h, "sh_ilock",   g_sw_cfg.sh_interlock_ms, 500);
    NVS_GET_U16(h, "sh_end_ext", g_sw_cfg.sh_endstop_ms,   2000);
    NVS_GET_U16(h, "sh_dbnce",   g_sw_cfg.sh_debounce_ms,  50);
    NVS_GET_U32(h, "sh_a_up_ms", g_sw_cfg.sh_up_ms[0],     53000);
    NVS_GET_U32(h, "sh_a_dn_ms", g_sw_cfg.sh_dn_ms[0],     52000);
    NVS_GET_U32(h, "sh_b_up_ms", g_sw_cfg.sh_up_ms[1],     53000);
    NVS_GET_U32(h, "sh_b_dn_ms", g_sw_cfg.sh_dn_ms[1],     52000);

    /* ── Parametri dimmer ── */
    NVS_GET_U32(h, "dim_freq",    g_sw_cfg.pwm_freq_hz,        1000);
    NVS_GET_U8 (h, "dim_res",     g_sw_cfg.pwm_resolution,       13);
    NVS_GET_U16(h, "dim_fade",    g_sw_cfg.fade_ms,             500);
    NVS_GET_U16(h, "dim_lp_ms",   g_sw_cfg.long_press_ms,       500);
    NVS_GET_U16(h, "dim_dbnce",   g_sw_cfg.dimmer_debounce_ms,   50);
    NVS_GET_U8 (h, "dim_step",    g_sw_cfg.dimming_step,           5);
    NVS_GET_U16(h, "dim_step_ms", g_sw_cfg.dimming_step_ms,      50);
    NVS_GET_U8 (h, "dim_def_lvl", g_sw_cfg.default_level,       128);
    NVS_GET_U8 (h, "dim_btn_lvl", g_sw_cfg.btn_active_level,      0);

    if (h) nvs_close(h);

    /* ── Flag derivati ── */
    g_sw_cfg.has_relay   = false;
    g_sw_cfg.has_shutter = false;
    g_sw_cfg.has_dimmer  = false;
    for (int i = 0; i < 4; i++) {
        if (g_sw_cfg.ch_type[i] == CH_RELAY_STABLE ||
            g_sw_cfg.ch_type[i] == CH_RELAY_IMPULSE)
            g_sw_cfg.has_relay = true;
        if (g_sw_cfg.ch_type[i] == CH_SHUTTER)
            g_sw_cfg.has_shutter = true;
        if (g_sw_cfg.ch_type[i] == CH_DIMMER)
            g_sw_cfg.has_dimmer = true;
    }

    /* ── Log riepilogo ── */
    ESP_LOGI(TAG, "Config caricata: %d canali | relay=%d shutter=%d dimmer=%d",
             g_sw_cfg.num_channels,
             g_sw_cfg.has_relay, g_sw_cfg.has_shutter, g_sw_cfg.has_dimmer);
    for (int i = 0; i < 4; i++) {
        if (g_sw_cfg.ch_type[i] != CH_NONE)
            ESP_LOGI(TAG, "  CH%d: type=%d in=%d", i,
                     g_sw_cfg.ch_type[i], g_sw_cfg.ch_in_type[i]);
    }
}
