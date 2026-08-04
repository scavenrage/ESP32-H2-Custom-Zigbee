/*
 * config.h — Costanti del firmware smart_switch
 *
 * Contiene SOLO le costanti numeriche dei tipi canale e ingresso.
 * I valori per-dispositivo (quale tipo su quale canale, GPIO, timing, ecc.)
 * sono ora memorizzati nella partizione NVS e caricati a runtime da sw_config.c.
 *
 * Non modificare questo file — è invariante tra tutti i dispositivi.
 */

#pragma once

/* ------------------------------------------------------------------ */
/*  Tipi canale                                                        */
/* ------------------------------------------------------------------ */
#define CH_NONE           0   /* canale non usato                         */
#define CH_RELAY_STABLE   1   /* relay stabile ON/OFF                     */
#define CH_RELAY_IMPULSE  2   /* relay a impulso                          */
#define CH_SHUTTER        3   /* tapparella (occupa 2 canali consecutivi) */
#define CH_DIMMER         4   /* dimmer PWM (solo canale 0)               */

/* ------------------------------------------------------------------ */
/*  Tipi ingresso                                                      */
/* ------------------------------------------------------------------ */
#define INPUT_BUTTON  0   /* pulsante momentaneo (attivo-basso default)   */
#define INPUT_SWITCH  1   /* interruttore bistabile                       */
