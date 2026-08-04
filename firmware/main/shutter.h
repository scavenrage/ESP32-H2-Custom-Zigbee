/*
 * shutter.h — Gestione tapparelle (smart_switch)
 *
 * Supporta fino a 2 tapparelle, configurate come coppie di canali:
 *   Tapparella A: canali 0+1 (GPIO4=SU, GPIO5=GIÙ, GPIO1=in-SU, GPIO0=in-GIÙ)
 *   Tapparella B: canali 2+3 (GPIO10=SU, GPIO11=GIÙ, GPIO3=in-SU, GPIO2=in-GIÙ)
 *
 * Il canale "secondario" di ogni coppia (CH1 per A, CH3 per B) è impostato
 * a CH_NONE nel config.h dal wizard e non ha endpoint Zigbee.
 */

#pragma once

#include "config.h"
#include <stdbool.h>
#include <stdint.h>

/* Indice tapparella: 0=A (CH0+CH1), 1=B (CH2+CH3) */
typedef enum {
    SHUTTER_CMD_UP,    /* Apri / sali                        */
    SHUTTER_CMD_DOWN,  /* Chiudi / scendi                    */
    SHUTTER_CMD_STOP,  /* Ferma                              */
    SHUTTER_CMD_GOTO,  /* Vai alla posizione param% (0=open) */
} shutter_cmd_t;

/*
 * Callback chiamata quando la posizione cambia (movimento completato o stop).
 * ch  = canale di partenza della tapparella (0=A, 2=B)
 * pos = posizione 0-100 (0=aperta, 100=chiusa)
 */
typedef void (*shutter_change_cb_t)(uint8_t ch, uint8_t pos);

/** Inizializza GPIO e task per tutte le tapparelle configurate. */
void shutter_init(shutter_change_cb_t cb);

/**
 * Esegue un comando su una tapparella.
 * ch  = canale principale (0 per A, 2 per B)
 * cmd = comando
 * param = solo per GOTO: posizione 0-100
 */
void shutter_command(uint8_t ch, shutter_cmd_t cmd, uint8_t param);

/** Posizione corrente (0=aperta, 100=chiusa). */
uint8_t shutter_get_position(uint8_t ch);

/** True se il motore è in movimento. */
bool shutter_is_moving(uint8_t ch);

/** Sincronizza posizione verso Zigbee per tutte le tapparelle attive. */
void shutter_sync_all(void);
