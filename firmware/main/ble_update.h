/*
 * ble_update.h — modalità aggiornamento BLE (esperimento)
 *
 * Al riavvio successivo alla richiesta il firmware NON avvia Zigbee: parte solo il
 * BLE per una finestra di BLE_UPDATE_WINDOW_S secondi, poi riavvia in modalità normale.
 * Il flag in NVS viene azzerato prima di avviare il BLE, così un crash non crea cicli.
 */

#pragma once

#include <stdbool.h>

#define BLE_UPDATE_WINDOW_S  600

/* Salva il flag in NVS e riavvia. */
void ble_update_request_and_restart(void);

/* true se è stata richiesta la modalità BLE (e azzera il flag). Da chiamare a inizio app_main. */
bool ble_update_pending(void);

/* Esegue la finestra BLE e poi riavvia. Non ritorna. */
void ble_update_run(void);
