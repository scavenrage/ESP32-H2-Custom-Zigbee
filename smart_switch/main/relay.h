/*
 * relay.h — Gestione relay stabili e a impulso (smart_switch)
 *
 * Gestisce i canali configurati come CH_RELAY_STABLE o CH_RELAY_IMPULSE.
 * Ogni canale ha un GPIO di uscita e uno di ingresso (pulsante o interruttore).
 *
 * GPIO fissi (da hardware):
 *   CH0: out=GPIO4,  in=GPIO1
 *   CH1: out=GPIO5,  in=GPIO0
 *   CH2: out=GPIO10, in=GPIO3
 *   CH3: out=GPIO11, in=GPIO2
 */

#pragma once

#include "config.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * Callback chiamata quando lo stato di un relay cambia per input fisico.
 * ch    = indice canale (0-3)
 * state = nuovo stato (true=ON, false=OFF)
 * Il main.c usa questa callback per aggiornare l'attributo Zigbee.
 */
typedef void (*relay_change_cb_t)(uint8_t ch, bool state);

/** Inizializza GPIO e avvia il task di input per tutti i canali relay attivi. */
void relay_init(relay_change_cb_t cb);

/** Imposta lo stato di un relay (chiamato da Zigbee). */
void relay_set(uint8_t ch, bool state);

/** Legge lo stato attuale di un relay. */
bool relay_get(uint8_t ch);
