/*
 * shutter.c — Gestione tapparelle (smart_switch)
 *
 * Portato da rollershutter/rollershutter.c.
 * Adattato per la configurazione #if-based di smart_switch.
 *
 * GPIO fissi (da hardware, invariabili):
 *   Tapparella A (phys_idx=0): CH0+CH1 → UP=GPIO4,  DN=GPIO5,  in-UP=GPIO1, in-DN=GPIO0
 *   Tapparella B (phys_idx=1): CH2+CH3 → UP=GPIO10, DN=GPIO11, in-UP=GPIO3, in-DN=GPIO2
 *
 * La tabella s_active[] viene costruita a compile-time tramite #if e dice
 * quali tapparelle sono configurate. Solo quelle vengono inizializzate.
 * Esempio: CH2=CH_SHUTTER → s_num_sh=1, s_active[0].phys=1 (tapparella B).
 *
 * Zigbee sync: tramite shutter_change_cb_t registrata in shutter_init().
 * NVS: flag nvs_dirty + nvs_task dedicato (mai da task Zigbee).
 */

#include "shutter.h"
#include "sw_config.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "SHUTTER";

/* ── GPIO fissi indicizzati per tapparella (phys_idx 0=A, 1=B) ─────────*/
static const gpio_num_t UP_GPIO[2]    = { GPIO_NUM_4,  GPIO_NUM_10 };
static const gpio_num_t DOWN_GPIO[2]  = { GPIO_NUM_5,  GPIO_NUM_11 };
static const gpio_num_t IN_UP_GPIO[2] = { GPIO_NUM_1,  GPIO_NUM_3  };
static const gpio_num_t IN_DN_GPIO[2] = { GPIO_NUM_0,  GPIO_NUM_2  };

/* Canale "principale" per ciascuna posizione fisica (usato per la callback) */
static const uint8_t PHYS_TO_CH[2] = { 0, 2 };

/* Tipo ingresso per phys_idx — letto da g_sw_cfg a runtime */
#define IN_UP_TYPE(p)  (g_sw_cfg.ch_in_up[PHYS_TO_CH[p]])
#define IN_DN_TYPE(p)  (g_sw_cfg.ch_in_dn[PHYS_TO_CH[p]])

/* Timing — letto da g_sw_cfg a runtime */
#define UP_MS(p)               (g_sw_cfg.sh_up_ms[p])
#define DOWN_MS(p)             (g_sw_cfg.sh_dn_ms[p])
#define SHUTTER_ENDSTOP_EXTRA_MS (g_sw_cfg.sh_endstop_ms)
#define SHUTTER_DEBOUNCE_MS      (g_sw_cfg.sh_debounce_ms)
#define SHUTTER_INTERLOCK_MS     (g_sw_cfg.sh_interlock_ms)

/* ── Tabella tapparelle attive (costruita a compile-time) ──────────────*/
typedef struct { uint8_t phys; } sh_active_t;
static sh_active_t s_active[2];
static uint8_t     s_num_sh = 0;

static void build_shutter_table(void)
{
    s_num_sh = 0;
    if (g_sw_cfg.ch_type[0] == CH_SHUTTER) s_active[s_num_sh++].phys = 0; /* A */
    if (g_sw_cfg.ch_type[2] == CH_SHUTTER) s_active[s_num_sh++].phys = 1; /* B */
}

/* ── State machine (indicizzata per phys_idx) ────────────────────────── */
typedef enum { SH_STOPPED, SH_MOVING_UP, SH_MOVING_DOWN, SH_INTERLOCK } sh_state_t;

typedef struct {
    sh_state_t    state;
    uint8_t       position;        /* 0=aperta, 100=chiusa */
    TickType_t    move_start_tick;
    uint8_t       move_start_pos;
    uint8_t       move_end_pos;
    shutter_cmd_t interlock_cmd;
    uint8_t       interlock_param;
    TimerHandle_t travel_timer;
    TimerHandle_t interlock_timer;
    TimerHandle_t position_timer;
} sh_t;

static sh_t s_sh[2];   /* sempre dimensionato 2 (A e B), solo i phys attivi usati */

/* ── Callback verso main.c ───────────────────────────────────────────── */
static shutter_change_cb_t s_cb = NULL;

/* ── NVS ─────────────────────────────────────────────────────────────── */
#define NVS_NAMESPACE "shutter"
static volatile bool s_nvs_dirty = false;

static void nvs_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    for (int i = 0; i < s_num_sh; i++) {
        uint8_t p = s_active[i].phys;
        char key[8]; snprintf(key, sizeof(key), "pos%d", p);
        nvs_set_u8(h, key, s_sh[p].position);
    }
    nvs_commit(h); nvs_close(h);
}

static void nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    for (int i = 0; i < s_num_sh; i++) {
        uint8_t p = s_active[i].phys;
        char key[8]; snprintf(key, sizeof(key), "pos%d", p);
        nvs_get_u8(h, key, &s_sh[p].position);
    }
    nvs_close(h);
}

static void nvs_task(void *pv)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (s_nvs_dirty) { s_nvs_dirty = false; nvs_save(); }
    }
}

/* ── Helper (lavorano su phys_idx) ──────────────────────────────────── */
static uint8_t calc_position(uint8_t p)
{
    sh_t *s = &s_sh[p];
    if (s->state != SH_MOVING_UP && s->state != SH_MOVING_DOWN) return s->position;
    TickType_t el = xTaskGetTickCount() - s->move_start_tick;
    uint32_t el_ms = (uint32_t)(el * portTICK_PERIOD_MS);
    uint32_t travel = (s->state == SH_MOVING_UP) ? UP_MS(p) : DOWN_MS(p);
    uint32_t delta = (el_ms * 100UL) / travel;
    if (delta > 100) delta = 100;
    if (s->state == SH_MOVING_UP)
        return (s->move_start_pos > delta) ? (uint8_t)(s->move_start_pos - delta) : 0;
    uint32_t np = (uint32_t)s->move_start_pos + delta;
    return (np > 100) ? 100 : (uint8_t)np;
}

static void stop_hw(uint8_t p)
{
    gpio_set_level(UP_GPIO[p],   0);
    gpio_set_level(DOWN_GPIO[p], 0);
}

static void notify(uint8_t p)
{
    if (s_cb) s_cb(PHYS_TO_CH[p], s_sh[p].position);
}

static void start_moving(uint8_t p, sh_state_t dir, uint32_t ms, uint8_t end_pos)
{
    sh_t *s = &s_sh[p];
    if (ms == 0) { s->position = end_pos; s_nvs_dirty = true; notify(p); return; }
    if (end_pos == 0 || end_pos == 100) ms += SHUTTER_ENDSTOP_EXTRA_MS;
    bool up = (dir == SH_MOVING_UP);
    gpio_set_level(up ? UP_GPIO[p]   : DOWN_GPIO[p], 1);
    gpio_set_level(up ? DOWN_GPIO[p] : UP_GPIO[p],   0);
    s->move_start_tick = xTaskGetTickCount();
    s->move_start_pos  = s->position;
    s->move_end_pos    = end_pos;
    s->state           = dir;
    xTimerChangePeriod(s->travel_timer, pdMS_TO_TICKS(ms), 0);
    xTimerStart(s->position_timer, 0);
    ESP_LOGI(TAG, "[%c] %s→%d%% (%.1fs da %d%%)",
             p==0?'A':'B', up?"SU":"GIU", end_pos, ms/1000.0f, s->position);
}

/* ── Timer callbacks ─────────────────────────────────────────────────── */
static void travel_timer_cb(TimerHandle_t t)
{
    uint8_t p = (uint8_t)(uint32_t)pvTimerGetTimerID(t);
    xTimerStop(s_sh[p].position_timer, 0);
    stop_hw(p);
    s_sh[p].position = s_sh[p].move_end_pos;
    s_sh[p].state    = SH_STOPPED;
    s_nvs_dirty      = true;
    notify(p);
    ESP_LOGI(TAG, "[%c] Fine corsa→%d%%", p==0?'A':'B', s_sh[p].position);
}

static void interlock_timer_cb(TimerHandle_t t)
{
    uint8_t p = (uint8_t)(uint32_t)pvTimerGetTimerID(t);
    shutter_cmd_t cmd   = s_sh[p].interlock_cmd;
    uint8_t       param = s_sh[p].interlock_param;
    s_sh[p].state       = SH_STOPPED;
    s_sh[p].interlock_cmd = SHUTTER_CMD_STOP;
    shutter_command(PHYS_TO_CH[p], cmd, param);
}

static void position_timer_cb(TimerHandle_t t)
{
    uint8_t p = (uint8_t)(uint32_t)pvTimerGetTimerID(t);
    if (s_sh[p].state == SH_MOVING_UP || s_sh[p].state == SH_MOVING_DOWN) {
        s_sh[p].position = calc_position(p);
        notify(p);
    }
}

/* ── API pubblica ────────────────────────────────────────────────────── */

void shutter_command(uint8_t ch, shutter_cmd_t cmd, uint8_t param)
{
    /* ch 0 → phys 0 (A), ch 2 → phys 1 (B) */
    uint8_t p = (ch == 0) ? 0 : 1;
    sh_t *s = &s_sh[p];

    switch (cmd) {
    case SHUTTER_CMD_UP:
        switch (s->state) {
        case SH_STOPPED:
            if (s->position == 0) { ESP_LOGI(TAG, "[%c] Gia aperta", p==0?'A':'B'); return; }
            start_moving(p, SH_MOVING_UP, (uint32_t)s->position * UP_MS(p) / 100, 0);
            break;
        case SH_MOVING_UP:   shutter_command(ch, SHUTTER_CMD_STOP, 0); break;
        case SH_MOVING_DOWN:
            s->position = calc_position(p);
            xTimerStop(s->travel_timer,0); xTimerStop(s->position_timer,0); stop_hw(p);
            s->interlock_cmd=SHUTTER_CMD_UP; s->interlock_param=0; s->state=SH_INTERLOCK;
            xTimerReset(s->interlock_timer, 0);
            break;
        case SH_INTERLOCK: s->interlock_cmd = SHUTTER_CMD_UP; break;
        }
        break;

    case SHUTTER_CMD_DOWN:
        switch (s->state) {
        case SH_STOPPED:
            if (s->position == 100) { ESP_LOGI(TAG, "[%c] Gia chiusa", p==0?'A':'B'); return; }
            start_moving(p, SH_MOVING_DOWN, (uint32_t)(100-s->position)*DOWN_MS(p)/100, 100);
            break;
        case SH_MOVING_DOWN: shutter_command(ch, SHUTTER_CMD_STOP, 0); break;
        case SH_MOVING_UP:
            s->position = calc_position(p);
            xTimerStop(s->travel_timer,0); xTimerStop(s->position_timer,0); stop_hw(p);
            s->interlock_cmd=SHUTTER_CMD_DOWN; s->interlock_param=0; s->state=SH_INTERLOCK;
            xTimerReset(s->interlock_timer, 0);
            break;
        case SH_INTERLOCK: s->interlock_cmd = SHUTTER_CMD_DOWN; break;
        }
        break;

    case SHUTTER_CMD_STOP:
        if (s->state==SH_MOVING_UP || s->state==SH_MOVING_DOWN) s->position=calc_position(p);
        xTimerStop(s->travel_timer,0); xTimerStop(s->interlock_timer,0); xTimerStop(s->position_timer,0);
        stop_hw(p); s->state=SH_STOPPED; s_nvs_dirty=true; notify(p);
        ESP_LOGI(TAG, "[%c] STOP→%d%%", p==0?'A':'B', s->position);
        break;

    case SHUTTER_CMD_GOTO: {
        uint8_t target = param;
        if (s->state==SH_MOVING_UP || s->state==SH_MOVING_DOWN) {
            bool was_up = (s->state==SH_MOVING_UP);
            s->position = calc_position(p);
            xTimerStop(s->travel_timer,0); xTimerStop(s->position_timer,0); stop_hw(p);
            if (was_up != (target < s->position)) {
                s->interlock_cmd=SHUTTER_CMD_GOTO; s->interlock_param=target;
                s->state=SH_INTERLOCK; xTimerReset(s->interlock_timer, 0); break;
            }
            s->state = SH_STOPPED;
        } else if (s->state==SH_INTERLOCK) {
            s->interlock_cmd=SHUTTER_CMD_GOTO; s->interlock_param=target; break;
        }
        if (target == s->position) break;
        if (target < s->position)
            start_moving(p, SH_MOVING_UP,   (uint32_t)(s->position-target)*UP_MS(p)/100, target);
        else
            start_moving(p, SH_MOVING_DOWN, (uint32_t)(target-s->position)*DOWN_MS(p)/100, target);
        break;
    }
    }
}

uint8_t shutter_get_position(uint8_t ch)
{
    uint8_t p = (ch == 0) ? 0 : 1;
    return s_sh[p].position;
}

bool shutter_is_moving(uint8_t ch)
{
    uint8_t p = (ch == 0) ? 0 : 1;
    return s_sh[p].state == SH_MOVING_UP || s_sh[p].state == SH_MOVING_DOWN;
}

void shutter_sync_all(void)
{
    for (int i = 0; i < s_num_sh; i++) notify(s_active[i].phys);
}

/* ── Input ISR + task ────────────────────────────────────────────────── */
typedef struct { uint8_t i; uint8_t dir; } sh_evt_t;   /* i = indice in s_active[] */
static QueueHandle_t s_input_q;

static void IRAM_ATTR sh_isr(void *arg)
{
    sh_evt_t e; uint32_t code=(uint32_t)arg;
    e.i=(uint8_t)(code>>1); e.dir=(uint8_t)(code&1);
    xQueueSendFromISR(s_input_q, &e, NULL);
}

static void input_task(void *pv)
{
    sh_evt_t e;
    bool sw_prev[2][2] = {0};
    for (int i = 0; i < s_num_sh; i++) {
        uint8_t p = s_active[i].phys;
        sw_prev[i][0] = (gpio_get_level(IN_UP_GPIO[p]) == 0);
        sw_prev[i][1] = (gpio_get_level(IN_DN_GPIO[p]) == 0);
    }
    for (;;) {
        if (xQueueReceive(s_input_q, &e, portMAX_DELAY) != pdTRUE) continue;
        vTaskDelay(pdMS_TO_TICKS(SHUTTER_DEBOUNCE_MS));
        { sh_evt_t tmp; while (xQueueReceive(s_input_q, &tmp, 0) == pdTRUE); }

        for (int i = 0; i < s_num_sh; i++) {
            uint8_t p = s_active[i].phys;
            uint8_t ch = PHYS_TO_CH[p];
            for (int dir = 0; dir < 2; dir++) {
                gpio_num_t gpio = dir ? IN_DN_GPIO[p] : IN_UP_GPIO[p];
                uint8_t    inp  = dir ? IN_DN_TYPE(p) : IN_UP_TYPE(p);
                bool       cur  = (gpio_get_level(gpio) == 0);
                if (inp == INPUT_BUTTON) {
                    if (!cur) continue;
                    if (shutter_is_moving(ch)) shutter_command(ch, SHUTTER_CMD_STOP, 0);
                    else shutter_command(ch, dir ? SHUTTER_CMD_DOWN : SHUTTER_CMD_UP, 0);
                    ESP_LOGI(TAG, "[%c] Pulsante %s", p==0?'A':'B', dir?"GIU":"SU");
                } else {
                    if (cur == sw_prev[i][dir]) continue;
                    sw_prev[i][dir] = cur;
                    if (cur) shutter_command(ch, dir ? SHUTTER_CMD_DOWN : SHUTTER_CMD_UP, 0);
                    else     shutter_command(ch, SHUTTER_CMD_STOP, 0);
                    ESP_LOGI(TAG, "[%c] Switch %s→%s", p==0?'A':'B', dir?"GIU":"SU", cur?"ON":"OFF");
                }
            }
        }
    }
}

/* ── Init ────────────────────────────────────────────────────────────── */
void shutter_init(shutter_change_cb_t cb)
{
    s_cb = cb;
    memset(s_sh, 0, sizeof(s_sh));
    build_shutter_table();

    if (s_num_sh == 0) return;

    nvs_load();

    /* Coda creata PRIMA di registrare gli ISR */
    s_input_q = xQueueCreate(8, sizeof(sh_evt_t));

    for (int i = 0; i < s_num_sh; i++) {
        uint8_t p = s_active[i].phys;

        /* GPIO output */
        gpio_config_t out = {
            .pin_bit_mask = (1ULL<<UP_GPIO[p]) | (1ULL<<DOWN_GPIO[p]),
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&out);
        gpio_set_level(UP_GPIO[p],   0);
        gpio_set_level(DOWN_GPIO[p], 0);

        /* GPIO input */
        gpio_config_t in = {
            .pin_bit_mask = (1ULL<<IN_UP_GPIO[p]) | (1ULL<<IN_DN_GPIO[p]),
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,   /* impostato per pin sotto */
        };
        gpio_config(&in);

        /* ISR per ingresso UP */
        gpio_set_intr_type(IN_UP_GPIO[p],
            (IN_UP_TYPE(p)==INPUT_BUTTON) ? GPIO_INTR_NEGEDGE : GPIO_INTR_ANYEDGE);
        gpio_isr_handler_add(IN_UP_GPIO[p], sh_isr, (void*)((uint32_t)(i<<1)|0));

        /* ISR per ingresso DOWN */
        gpio_set_intr_type(IN_DN_GPIO[p],
            (IN_DN_TYPE(p)==INPUT_BUTTON) ? GPIO_INTR_NEGEDGE : GPIO_INTR_ANYEDGE);
        gpio_isr_handler_add(IN_DN_GPIO[p], sh_isr, (void*)((uint32_t)(i<<1)|1));

        /* Timer FreeRTOS */
        s_sh[p].travel_timer = xTimerCreate("sh_trv",
            pdMS_TO_TICKS(DOWN_MS(p)), pdFALSE, (void*)(uint32_t)p, travel_timer_cb);
        s_sh[p].interlock_timer = xTimerCreate("sh_il",
            pdMS_TO_TICKS(SHUTTER_INTERLOCK_MS), pdFALSE, (void*)(uint32_t)p, interlock_timer_cb);
        s_sh[p].position_timer = xTimerCreate("sh_pos",
            pdMS_TO_TICKS(1000), pdTRUE, (void*)(uint32_t)p, position_timer_cb);

        ESP_LOGI(TAG, "[%c] UP=GPIO%d DN=GPIO%d pos=%d%%",
                 p==0?'A':'B', UP_GPIO[p], DOWN_GPIO[p], s_sh[p].position);
    }

    xTaskCreate(input_task, "shutter_in",  4096, NULL, 5, NULL);
    xTaskCreate(nvs_task,   "shutter_nvs", 2048, NULL, 2, NULL);
}
