/*
 * led.h — Gestione LED di stato su GPIO22
 *
 * Pattern di lampeggio:
 *   LED_BOOT              → lampeggio rapido 100ms (firmware sta partendo)
 *   LED_ZIGBEE_SEARCHING  → doppio blink ogni 2s  (cerca rete / steering)
 *   LED_OPERATIONAL       → singolo blink ogni 5s (operativo, heartbeat)
 *   LED_OTA_IN_PROGRESS   → lento 500ms on/off    (download firmware)
 *   LED_ERROR             → 3 blink veloci ogni 3s (problema)
 *   LED_FACTORY_RESET     → acceso fisso           (reset in corso)
 */

#pragma once

#define LED_GPIO 22

typedef enum {
    LED_BOOT = 0,
    LED_ZIGBEE_SEARCHING,
    LED_OPERATIONAL,
    LED_OTA_IN_PROGRESS,
    LED_ERROR,
    LED_FACTORY_RESET,
} led_state_t;

/**
 * @brief Inizializza il GPIO e avvia il task FreeRTOS del LED.
 *        Chiamare una sola volta in app_main(), prima dello stack Zigbee.
 */
void led_init(void);

/**
 * @brief Cambia lo stato del LED. Thread-safe.
 *        Il task lo rileva al prossimo ciclo senza bloccare il chiamante.
 */
void led_set_state(led_state_t state);
