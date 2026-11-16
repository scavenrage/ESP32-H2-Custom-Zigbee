/*
 * maint.h — manutenzione: modalità programmazione (download seriale) e comandi da seriale
 */

#pragma once

/* Imposta il flag "al prossimo avvio entra nel bootloader ROM" e riavvia. Non ritorna. */
void maint_enter_download_mode(void);

/* Avvia il task che legge i comandi dalla UART di console (scrivi "help" + invio). */
void maint_serial_start(void);

#include <stdbool.h>

/* ZB2: cancella le credenziali Zigbee. Non riavvia.
 * esp-zigbee-lib 2.0.5 (livello di compatibilità 1.x) salva i dati Zigbee nella partizione NVS
 * di default "nvs", namespace "zigbee" (verificato con un dump del modulo di sviluppo l'11/11/2026;
 * nome del namespace non documentato). Cancella quel namespace, la partizione zb_fct e anche la
 * vecchia partizione FAT zb_storage (1.x), così una futura migrazione non ripristina credenziali vecchie.
 * erase_phy = true cancella anche phy_init (hard reset). La configurazione sw_cfg non viene toccata. */
void maint_erase_zigbee_credentials(bool erase_phy);
