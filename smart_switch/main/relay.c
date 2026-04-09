/*
 * relay.c — Gestione relay stabili e a impulso (smart_switch)
 *
 * Portato direttamente da relay_board_4/relay_board.c.
 * Adattato per la configurazione #if-based multi-canale di smart_switch.
 *
 * GPIO fissi (da hardware):
 *   CH0: out=GPIO4,  in=GPIO1
 *   CH1: out=GPIO5,  in=GPIO0
 *   CH2: out=GPIO10, in=GPIO3
 *   CH3: out=GPIO11, in=GPIO2
 *
 * Input:
 *   INPUT_BUTTON → ISR su fronte di discesa, debounce, toggle/impulso
 *   INPUT_SWITCH → ISR su entrambi i fronti, ogni cambio = toggle/impulso
 *
 * Impulso:
 *   FreeRTOS timer one-shot (mai vTaskDelay nel task Zigbee).
 *   Al termine il timer callback resetta l'attributo ZCL a OFF via s_cb.
 *
 * NVS:
 *   Flag nvs_dirty + task dedicato a bassa priorità. Mai scritto
 *   direttamente dal task Zigbee o dal timer callback.
 */

#include "relay.h"
#include "sw_config.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "RELAY";

/* Parametri letti da g_sw_cfg a runtime (vedi sw_config.h) */
#define RELAY_DEBOUNCE_MS   (g_sw_cfg.relay_debounce_ms)
#define IMPULSE_DURATION_MS (g_sw_cfg.impulse_ms)

/* ── GPIO fissi da hardware ──────────────────────────────────────────── */
static const gpio_num_t OUT_GPIO[4] = {
    GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_10, GPIO_NUM_11
};
static const gpio_num_t IN_GPIO[4] = {
    GPIO_NUM_1, GPIO_NUM_0, GPIO_NUM_3, GPIO_NUM_2
};

/* ── Tabella canali relay attivi (costruita a compile-time) ──────────── */
typedef struct {
    uint8_t ch;
    int     type;   /* CH_RELAY_STABLE o CH_RELAY_IMPULSE */
    int     input;  /* INPUT_BUTTON o INPUT_SWITCH */
} relay_ch_t;

static relay_ch_t s_relay_ch[4];
static uint8_t    s_num_relay = 0;

static void build_channel_table(void)
{
    s_num_relay = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t t = g_sw_cfg.ch_type[i];
        if (t == CH_RELAY_STABLE || t == CH_RELAY_IMPULSE)
            s_relay_ch[s_num_relay++] = (relay_ch_t){(uint8_t)i, t, g_sw_cfg.ch_in_type[i]};
    }
}

/* ── Stato relay ─────────────────────────────────────────────────────── */
static bool s_state[4] = {false, false, false, false};

/* ── Callback verso main.c ───────────────────────────────────────────── */
static relay_change_cb_t s_cb = NULL;

/* ── NVS ─────────────────────────────────────────────────────────────── */
#define NVS_NAMESPACE  "relay"
#define NVS_KEY_STATES "states"

static volatile bool s_nvs_dirty = false;

static void nvs_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    uint8_t mask = 0;
    for (int i = 0; i < 4; i++) if (s_state[i]) mask |= (1 << i);
    nvs_set_u8(h, NVS_KEY_STATES, mask);
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_load(void)
{
    nvs_handle_t h;
    uint8_t mask = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    nvs_get_u8(h, NVS_KEY_STATES, &mask);
    nvs_close(h);
    for (int i = 0; i < 4; i++) s_state[i] = (mask >> i) & 1;
}

static void nvs_task(void *pv)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (s_nvs_dirty) { s_nvs_dirty = false; nvs_save(); }
    }
}

/* ── Timer impulso (one-shot) ────────────────────────────────────────── */
static TimerHandle_t s_impulse_timers[4] = {NULL};

/*
 * Callback del timer: spegne il relay fisico e notifica main.c (OFF).
 * Esegue nel timer daemon di FreeRTOS — task normale, nessun blocco Zigbee.
 */
static void impulse_timer_cb(TimerHandle_t xTimer)
{
    uint8_t ch = (uint8_t)(uint32_t)pvTimerGetTimerID(xTimer);
    s_state[ch] = false;
    gpio_set_level(OUT_GPIO[ch], 0);
    s_nvs_dirty = true;
    ESP_LOGI(TAG, "CH%d impulso terminato → OFF", ch);
    if (s_cb) s_cb(ch, false);   /* resetta attributo ZCL a OFF */
}

/* ── ISR + coda input ────────────────────────────────────────────────── */
static QueueHandle_t s_input_q;

static void IRAM_ATTR input_isr(void *arg)
{
    uint32_t idx = (uint32_t)arg;  /* indice in s_relay_ch[] */
    xQueueSendFromISR(s_input_q, &idx, NULL);
}

/* ── Task input ──────────────────────────────────────────────────────── */
static void input_task(void *pv)
{
    uint32_t idx;
    TickType_t last_press[4] = {0};
    bool switch_prev[4] = {false};

    /* Leggi stato iniziale degli switch per evitare falsi trigger */
    for (uint8_t i = 0; i < s_num_relay; i++) {
        if (s_relay_ch[i].input == INPUT_SWITCH)
            switch_prev[i] = (gpio_get_level(IN_GPIO[s_relay_ch[i].ch]) == 0);
    }

    for (;;) {
        if (xQueueReceive(s_input_q, &idx, portMAX_DELAY) != pdTRUE) continue;
        if (idx >= s_num_relay) continue;

        uint8_t ch   = s_relay_ch[idx].ch;
        int     type = s_relay_ch[idx].type;
        int     inp  = s_relay_ch[idx].input;
        bool    new_state;

        if (inp == INPUT_BUTTON) {
            /* Debounce temporale */
            TickType_t now = xTaskGetTickCount();
            if ((now - last_press[idx]) < pdMS_TO_TICKS(RELAY_DEBOUNCE_MS)) continue;
            last_press[idx] = now;
            new_state = !s_state[ch];
        } else {
            /* Interruttore: filtra falsi trigger (ISR ANYEDGE) */
            bool cur = (gpio_get_level(IN_GPIO[ch]) == 0);
            if (cur == switch_prev[idx]) continue;
            switch_prev[idx] = cur;
            new_state = !s_state[ch];
        }

        if (type == CH_RELAY_STABLE) {
            s_state[ch] = new_state;
            gpio_set_level(OUT_GPIO[ch], new_state ? 1 : 0);
            s_nvs_dirty = true;
            ESP_LOGI(TAG, "CH%d → %s (input fisico)", ch, new_state ? "ON" : "OFF");
            if (s_cb) s_cb(ch, new_state);
        } else {
            /* Impulso: avvia timer (o resettalo se già in corso) */
            ESP_LOGI(TAG, "CH%d impulso (input fisico)", ch);
            s_state[ch] = true;
            gpio_set_level(OUT_GPIO[ch], 1);
            if (s_impulse_timers[ch]) xTimerReset(s_impulse_timers[ch], 0);
            /* il timer callback chiamerà s_cb(ch, false) al termine */
        }
    }
}

/* ── API pubblica ────────────────────────────────────────────────────── */

void relay_init(relay_change_cb_t cb)
{
    s_cb = cb;
    build_channel_table();

    if (s_num_relay == 0) return;

    /* Carica stati da NVS (relay stabili) */
    nvs_load();

    /* GPIO output */
    for (uint8_t i = 0; i < s_num_relay; i++) {
        uint8_t ch = s_relay_ch[i].ch;
        gpio_config_t out = {
            .pin_bit_mask = (1ULL << OUT_GPIO[ch]),
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&out);
        /* Ripristina stato salvato (solo stable; impulso sempre OFF) */
        bool init_state = (s_relay_ch[i].type == CH_RELAY_STABLE) ? s_state[ch] : false;
        s_state[ch] = init_state;
        gpio_set_level(OUT_GPIO[ch], init_state ? 1 : 0);
        ESP_LOGI(TAG, "CH%d init: out=GPIO%d tipo=%s ingresso=%s stato=%s",
                 ch, OUT_GPIO[ch],
                 s_relay_ch[i].type  == CH_RELAY_STABLE ? "STABLE"  : "IMPULSE",
                 s_relay_ch[i].input == INPUT_BUTTON    ? "BUTTON"  : "SWITCH",
                 init_state ? "ON" : "OFF");
    }

    /* GPIO input + ISR (gpio_install_isr_service chiamato da main.c) */
    s_input_q = xQueueCreate(8, sizeof(uint32_t));

    for (uint8_t i = 0; i < s_num_relay; i++) {
        uint8_t ch  = s_relay_ch[i].ch;
        int     inp = s_relay_ch[i].input;
        gpio_config_t in = {
            .pin_bit_mask = (1ULL << IN_GPIO[ch]),
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = (inp == INPUT_BUTTON) ? GPIO_INTR_NEGEDGE : GPIO_INTR_ANYEDGE,
        };
        gpio_config(&in);
        gpio_isr_handler_add(IN_GPIO[ch], input_isr, (void *)(uint32_t)i);
    }

    /* Timer one-shot per canali impulso */
    for (uint8_t i = 0; i < s_num_relay; i++) {
        if (s_relay_ch[i].type == CH_RELAY_IMPULSE) {
            uint8_t ch = s_relay_ch[i].ch;
            s_impulse_timers[ch] = xTimerCreate(
                "relay_imp",
                pdMS_TO_TICKS(IMPULSE_DURATION_MS),
                pdFALSE,                    /* one-shot */
                (void *)(uint32_t)ch,
                impulse_timer_cb
            );
            ESP_LOGI(TAG, "CH%d: timer impulso %dms creato", ch, IMPULSE_DURATION_MS);
        }
    }

    xTaskCreate(input_task, "relay_input", 2048, NULL, 5, NULL);
    xTaskCreate(nvs_task,   "relay_nvs",   1024, NULL, 2, NULL);
}

/*
 * relay_set() — chiamata dal task Zigbee (callback ZCL).
 *
 * STABLE: imposta GPIO direttamente (nessun delay).
 * IMPULSE: avvia/resetta il timer FreeRTOS — ritorna subito,
 *          il timer callback spegnerà il relay dopo IMPULSE_DURATION_MS
 *          e chiamerà s_cb(ch, false) per resettare l'attributo ZCL.
 */
void relay_set(uint8_t ch, bool state)
{
    /* Trova il tipo del canale */
    int type = CH_RELAY_STABLE;
    for (uint8_t i = 0; i < s_num_relay; i++) {
        if (s_relay_ch[i].ch == ch) { type = s_relay_ch[i].type; break; }
    }

    if (type == CH_RELAY_IMPULSE) {
        if (!state) return;   /* impulso solo su ON; OFF arriva dal timer */
        ESP_LOGI(TAG, "CH%d impulso (Zigbee) → timer", ch);
        s_state[ch] = true;
        gpio_set_level(OUT_GPIO[ch], 1);
        if (s_impulse_timers[ch]) xTimerReset(s_impulse_timers[ch], 0);
        /* timer callback chiamerà s_cb(ch, false) al termine */
    } else {
        s_state[ch] = state;
        gpio_set_level(OUT_GPIO[ch], state ? 1 : 0);
        s_nvs_dirty = true;
        ESP_LOGI(TAG, "CH%d → %s (Zigbee)", ch, state ? "ON" : "OFF");
        /* Non chiamiamo s_cb: l'attributo ZCL è già stato scritto dallo stack */
    }
}

bool relay_get(uint8_t ch)
{
    return s_state[ch];
}
