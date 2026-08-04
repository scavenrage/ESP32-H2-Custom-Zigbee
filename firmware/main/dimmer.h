/*
 * dimmer.h — Gestione dimmer PWM su CH0 (GPIO4, pulsante GPIO1)
 *
 * Attivo solo se CH0_TYPE == CH_DIMMER in config.h.
 */

#pragma once

#include "config.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * Callback chiamata quando lo stato del dimmer cambia per input fisico.
 * on_off = stato accensione
 * level  = livello 0-254
 */
typedef void (*dimmer_change_cb_t)(bool on_off, uint8_t level);

/** Inizializza LEDC, pulsante, NVS. */
void    dimmer_init(dimmer_change_cb_t cb);

/** Accende alla luminosità corrente. */
void    dimmer_on(void);

/** Spegne (fade a 0). */
void    dimmer_off(void);

/** Imposta il livello (0-254). */
void    dimmer_set_level(uint8_t level);

/** Legge il livello corrente. */
uint8_t dimmer_get_level(void);

/** Legge lo stato on/off. */
bool    dimmer_get_on_off(void);
