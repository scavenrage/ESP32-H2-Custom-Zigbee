/*
 * led.c — Task FreeRTOS per la gestione del LED di stato (GPIO22)
 *
 * Il task gira in loop e implementa il pattern di lampeggio corrispondente
 * allo stato corrente. Il resto del codice chiama solo led_set_state().
 */

#include "led.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static volatile led_state_t s_state = LED_BOOT;

/* ------------------------------------------------------------------ */
/*  Helpers interni                                                    */
/* ------------------------------------------------------------------ */

static inline void led_on(void)
{
    gpio_set_level(LED_GPIO, 1);
}

static inline void led_off(void)
{
    gpio_set_level(LED_GPIO, 0);
}

/* Singolo blink: accende on_ms, spegne off_ms */
static void blink(uint32_t on_ms, uint32_t off_ms)
{
    led_on();
    vTaskDelay(pdMS_TO_TICKS(on_ms));
    led_off();
    vTaskDelay(pdMS_TO_TICKS(off_ms));
}

/* ------------------------------------------------------------------ */
/*  Task LED                                                           */
/* ------------------------------------------------------------------ */

static void led_task(void *pv)
{
    for (;;) {
        switch (s_state) {

        case LED_BOOT:
            /* Lampeggio rapido continuo — firmware in inizializzazione */
            blink(100, 100);
            break;

        case LED_ZIGBEE_SEARCHING:
            /* Doppio blink ogni ~2s — cerca / connette rete Zigbee */
            blink(100, 100);
            blink(100, 1700);
            break;

        case LED_OPERATIONAL:
            /* Singolo blink breve ogni 5s — heartbeat, tutto OK */
            blink(100, 4900);
            break;

        case LED_OTA_IN_PROGRESS:
            /* Lento 500ms on/off — download firmware in corso */
            blink(500, 500);
            break;

        case LED_ERROR:
            /* Tre blink veloci ogni 3s — errore rilevato */
            blink(100, 100);
            blink(100, 100);
            blink(100, 2600);
            break;

        case LED_FACTORY_RESET:
            /* Fisso acceso — factory reset in corso */
            led_on();
            vTaskDelay(pdMS_TO_TICKS(100));
            break;

        default:
            vTaskDelay(pdMS_TO_TICKS(100));
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  API pubblica                                                       */
/* ------------------------------------------------------------------ */

void led_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    led_off();

    xTaskCreate(led_task, "led_task", 2048, NULL, 2, NULL);
}

void led_set_state(led_state_t state)
{
    s_state = state;
}
