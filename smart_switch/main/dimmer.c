/*
 * dimmer.c — Gestione dimmer PWM su CH0 (smart_switch)
 *
 * GPIO fissi: PWM=GPIO4 (OUT_GPIO[0]), Pulsante=GPIO1 (IN_GPIO[0])
 * Attivo solo se CH0_TYPE == CH_DIMMER in config.h.
 *
 * La sync Zigbee è delegata a main.c tramite callback.
 */

#include "dimmer.h"
#include "sw_config.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "DIMMER";

#define PWM_GPIO     GPIO_NUM_4
#define BUTTON_GPIO  GPIO_NUM_1

/* Parametri letti da g_sw_cfg a runtime (vedi sw_config.h) */
#define PWM_FREQ_HZ        (g_sw_cfg.pwm_freq_hz)
#define PWM_RESOLUTION     (g_sw_cfg.pwm_resolution)
#define FADE_TIME_MS       (g_sw_cfg.fade_ms)
#define LONG_PRESS_MS      (g_sw_cfg.long_press_ms)
#define DIMMER_DEBOUNCE_MS (g_sw_cfg.dimmer_debounce_ms)
#define DIMMING_STEP       (g_sw_cfg.dimming_step)
#define DIMMING_STEP_MS    (g_sw_cfg.dimming_step_ms)
#define DEFAULT_LEVEL      (g_sw_cfg.default_level)
#define BUTTON_ACTIVE_LEVEL (g_sw_cfg.btn_active_level)

/* ── Stato ───────────────────────────────────────────────────────────── */
static bool    s_on_off = false;
static uint8_t s_level  = 128;   /* default provvisorio; sovrascritto in dimmer_init() */

/* ── Callback ────────────────────────────────────────────────────────── */
static dimmer_change_cb_t s_cb = NULL;

/* ── NVS ─────────────────────────────────────────────────────────────── */
static volatile bool s_nvs_dirty = false;

static void nvs_save(void)
{
    nvs_handle_t h;
    if (nvs_open("dimmer", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "on_off", s_on_off ? 1 : 0);
    nvs_set_u8(h, "level",  s_level);
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open("dimmer", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v;
    if (nvs_get_u8(h, "on_off", &v) == ESP_OK) s_on_off = (v != 0);
    if (nvs_get_u8(h, "level",  &v) == ESP_OK) s_level  = v;
    nvs_close(h);
}

static void nvs_task(void *pv)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (s_nvs_dirty) { s_nvs_dirty = false; nvs_save(); }
    }
}

/* ── PWM / LEDC ──────────────────────────────────────────────────────── */
#define LEDC_TIMER    LEDC_TIMER_0
#define LEDC_CHANNEL  LEDC_CHANNEL_0
#define LEDC_MODE     LEDC_LOW_SPEED_MODE
#define PWM_MAX_DUTY  ((1 << PWM_RESOLUTION) - 1)

static void pwm_init(void)
{
    ledc_timer_config_t t = {
        .speed_mode      = LEDC_MODE,
        .timer_num       = LEDC_TIMER,
        .duty_resolution = (ledc_timer_bit_t)PWM_RESOLUTION,
        .freq_hz         = PWM_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&t);

    ledc_channel_config_t c = {
        .speed_mode = LEDC_MODE,
        .channel    = LEDC_CHANNEL,
        .timer_sel  = LEDC_TIMER,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = PWM_GPIO,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config(&c);
    ledc_fade_func_install(0);
}

static uint32_t level_to_duty(uint8_t level)
{
    if (level == 0)   return 0;
    if (level >= 254) return PWM_MAX_DUTY;
    return (uint32_t)level * PWM_MAX_DUTY / 254;
}

static void pwm_set(uint8_t level, uint32_t fade_ms)
{
    uint32_t duty = level_to_duty(level);
    if (fade_ms > 0)
        ledc_set_fade_time_and_start(LEDC_MODE, LEDC_CHANNEL, duty, fade_ms, LEDC_FADE_NO_WAIT);
    else {
        ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty);
        ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
    }
}

/* ── API pubblica ────────────────────────────────────────────────────── */

void dimmer_on(void)
{
    if (s_level == 0) s_level = DEFAULT_LEVEL;
    s_on_off = true;
    pwm_set(s_level, FADE_TIME_MS);
    s_nvs_dirty = true;
    if (s_cb) s_cb(s_on_off, s_level);
}

void dimmer_off(void)
{
    s_on_off = false;
    pwm_set(0, FADE_TIME_MS);
    s_nvs_dirty = true;
    if (s_cb) s_cb(s_on_off, s_level);
}

void dimmer_set_level(uint8_t level)
{
    if (level > 254) level = 254;
    s_level = level;
    if (s_on_off) {
        if (level == 0) { dimmer_off(); return; }
        pwm_set(level, 0);
    }
    s_nvs_dirty = true;
    if (s_cb) s_cb(s_on_off, s_level);
}

uint8_t dimmer_get_level(void)  { return s_level; }
bool    dimmer_get_on_off(void) { return s_on_off; }

/* ── Task pulsante ───────────────────────────────────────────────────── */
static void button_task(void *pv)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = (BUTTON_ACTIVE_LEVEL == 1) ? GPIO_PULLUP_DISABLE  : GPIO_PULLUP_ENABLE,
        .pull_down_en = (BUTTON_ACTIVE_LEVEL == 1) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    bool     pressed    = false;
    bool     long_on    = false;
    bool     dim_up     = true;
    TickType_t press_t  = 0;

    for (;;) {
        bool cur = (gpio_get_level(BUTTON_GPIO) == BUTTON_ACTIVE_LEVEL);

        if (cur && !pressed) {
            pressed = true; long_on = false; press_t = xTaskGetTickCount();
        } else if (cur && pressed) {
            uint32_t held = (xTaskGetTickCount() - press_t) * portTICK_PERIOD_MS;
            if (held >= LONG_PRESS_MS) {
                if (!long_on) { long_on = true; if (!s_on_off) dimmer_on(); }
                int16_t nl = (int16_t)s_level + (dim_up ? DIMMING_STEP : -DIMMING_STEP);
                if (nl >= 254) { nl = 254; dim_up = false; }
                if (nl <= 1)   { nl = 1;   dim_up = true;  }
                dimmer_set_level((uint8_t)nl);
            }
        } else if (!cur && pressed) {
            pressed = false;
            if (!long_on) {
                if (s_on_off) dimmer_off(); else dimmer_on();
            } else {
                dim_up = !dim_up;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(DIMMING_STEP_MS));
    }
}

/* ── Init ────────────────────────────────────────────────────────────── */
void dimmer_init(dimmer_change_cb_t cb)
{
    s_cb    = cb;
    s_level = DEFAULT_LEVEL;  /* default da NVS config; nvs_load() sovrascrive se c'è uno stato salvato */
    nvs_load();
    pwm_init();
    pwm_set(s_on_off ? s_level : 0, 0);

    xTaskCreate(button_task, "dimmer_btn", 2048, NULL, 5, NULL);
    xTaskCreate(nvs_task,    "dimmer_nvs", 2048, NULL, 2, NULL);

    ESP_LOGI(TAG, "init OK: on=%d level=%d", s_on_off, s_level);
}
