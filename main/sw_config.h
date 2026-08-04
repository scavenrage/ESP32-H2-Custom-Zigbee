/*
 * sw_config.h — Configurazione runtime del dispositivo (letta da NVS)
 *
 * Tutti i parametri che erano in config.h (generato da configure.py) sono ora
 * memorizzati nella partizione NVS sotto il namespace "sw_cfg".
 *
 * La prima programmazione carica il file nvs_config.bin generato da configure.py.
 * Gli aggiornamenti OTA toccano solo la partizione app; la NVS resta intatta.
 *
 * Uso:
 *   #include "sw_config.h"
 *   sw_cfg_load();          // chiamare prima di qualsiasi init hardware
 *   if (g_sw_cfg.has_relay) relay_init(...);
 */

#pragma once

#include "config.h"
#include <stdint.h>
#include <stdbool.h>

/* ── Struttura configurazione ─────────────────────────────────────────── */

typedef struct {

    /* ── Canali ── */
    uint8_t  num_channels;       /* numero canali attivi (1-4)          */
    uint8_t  ch_type[4];         /* CH_NONE / CH_RELAY_* / CH_SHUTTER / CH_DIMMER */
    uint8_t  ch_in_type[4];      /* INPUT_BUTTON / INPUT_SWITCH  (relay, dimmer)  */
    uint8_t  ch_in_up[4];        /* tipo ingresso SU  tapparella (per ch 0 e 2)   */
    uint8_t  ch_in_dn[4];        /* tipo ingresso GIÙ tapparella (per ch 0 e 2)   */

    /* ── Flag derivati (calcolati da ch_type[]) ── */
    bool     has_relay;
    bool     has_shutter;
    bool     has_dimmer;

    /* ── Parametri relay ── */
    uint16_t relay_debounce_ms;  /* debounce ingresso (ms)              */
    uint16_t impulse_ms;         /* durata impulso uscita (ms)          */

    /* ── Parametri tapparella ── */
    uint16_t sh_interlock_ms;    /* pausa inversione motore (ms)        */
    uint16_t sh_endstop_ms;      /* margine extra finecorsa (ms)        */
    uint16_t sh_debounce_ms;     /* debounce ingressi tapparella (ms)   */
    uint32_t sh_up_ms[2];        /* tempo salita  [0]=A [1]=B (ms)      */
    uint32_t sh_dn_ms[2];        /* tempo discesa [0]=A [1]=B (ms)      */

    /* ── Parametri dimmer ── */
    uint32_t pwm_freq_hz;        /* frequenza PWM (Hz)                  */
    uint8_t  pwm_resolution;     /* risoluzione bit (es. 13 = 0..8191)  */
    uint16_t fade_ms;            /* durata fade on/off (ms)             */
    uint16_t long_press_ms;      /* soglia pressione lunga (ms)         */
    uint16_t dimmer_debounce_ms; /* debounce pulsante dimmer (ms)       */
    uint8_t  dimming_step;       /* step per pressione lunga (0-255)    */
    uint16_t dimming_step_ms;    /* intervallo tra step (ms)            */
    uint8_t  default_level;      /* livello predefinito all'accensione  */
    uint8_t  btn_active_level;   /* 0=attivo-basso, 1=attivo-alto       */

} sw_cfg_t;

/* ── Istanza globale ──────────────────────────────────────────────────── */
extern sw_cfg_t g_sw_cfg;

/* ── API ──────────────────────────────────────────────────────────────── */

/**
 * Carica la configurazione dalla NVS (namespace "sw_cfg").
 * Valori mancanti vengono sostituiti con i default di fabbrica.
 * Calcola anche i flag has_relay / has_shutter / has_dimmer.
 * Deve essere chiamata PRIMA di qualsiasi init hardware.
 */
void sw_cfg_load(void);
