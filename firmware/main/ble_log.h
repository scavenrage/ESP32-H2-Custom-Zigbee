/*
 * ble_log.h — log del firmware via BLE, mentre Zigbee funziona (esperimento)
 *
 * Il flag è salvato in NVS e resta attivo, anche dopo i riavvii, finché non viene
 * disattivato dall'interruttore virtuale (EP13) o con una pressione breve di BOOT.
 * Nessun timer. Attivare o disattivare riavvia il dispositivo.
 *
 * Con il flag attivo: parte il BLE (peripheral, 1 connessione) accanto a Zigbee, tutto
 * quello che passa da ESP_LOGx viene copiato in un buffer circolare e inviato come
 * notifiche GATT al client collegato (pagina tools/ble_log.html). La seriale resta com'è.
 */

#pragma once

#include <stdbool.h>

/* true se il log via BLE è attivo (flag in NVS). */
bool ble_log_enabled(void);

/* Salva il flag e riavvia. Non ritorna. */
void ble_log_set_enabled_and_restart(bool on);

/* Se il flag è attivo avvia BLE e cattura del log, poi ritorna. Da chiamare dopo
 * esp_zb_platform_config() e prima di creare il task Zigbee. */
void ble_log_start_if_enabled(void);
