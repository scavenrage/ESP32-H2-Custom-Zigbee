/*
 * diag.h — diagnostica persistente (esperimento)
 *
 * Contatori salvati in NVS (namespace "diag") che sopravvivono ai reset, per capire
 * se i blocchi sul campo dipendono da riavvii (brownout, watchdog, panic) o da
 * eventi di rete (ping persi, link persi, leave). Esposti a Zigbee in un cluster
 * custom a sola lettura (DIAG_CLUSTER_ID) sul primo endpoint.
 */

#pragma once

#include <stdint.h>
#include "esp_system.h"
#include "esp_zigbee_core.h"   /* ZB2: header di compatibilità 1.x (CONFIG_ZB_SDK_1xx) */

/* 0 = nessun cluster Zigbee (restano i contatori in NVS e la riga di log all'avvio).
 * 1 = cluster custom esposto a Zigbee. */
#define DIAG_ZB_CLUSTER  1

/* La documentazione ESP Zigbee SDK richiede che l'ID di un cluster custom sia maggiore
 * di ESP_ZB_CUSTOM_CLUSTER_ID_MIN_VAL (il loro esempio usa 0xff00). Con 0x7F11 lo stack
 * andava in assert (zcl_general_commands.c:612). */
#define DIAG_CLUSTER_ID  0xFF11

typedef enum {
    DIAG_CAUSE_NONE = 0,
    DIAG_CAUSE_ZB_WDT,       /* watchdog applicativo Zigbee */
    DIAG_CAUSE_HARD_RESET,   /* zigbee_hard_reset() */
    DIAG_CAUSE_PHY_RESET,    /* zigbee_phy_reset() */
    DIAG_CAUSE_OTA,          /* riavvio dopo OTA */
    DIAG_CAUSE_FACTORY,      /* factory reset da pulsante BOOT */
    DIAG_CAUSE_BLE_UPDATE,   /* ingresso/uscita dalla modalità aggiornamento BLE */
    DIAG_CAUSE_BLE_LOG,      /* attivazione/disattivazione del log via BLE */
    DIAG_CAUSE_SERIAL,       /* comando "reset" dalla seriale */
} diag_cause_t;

typedef enum {
    DIAG_EVT_PING_FAIL,
    DIAG_EVT_NO_LINKS,
    DIAG_EVT_LEAVE,
    DIAG_EVT_STEERING_FAIL,
} diag_event_t;

/* Da chiamare a inizio app_main, dopo nvs_flash_init(), con esp_reset_reason(). */
void diag_boot(esp_reset_reason_t rr);

/* true se l'ultimo riavvio software è stato deciso dall'utente (OTA, modalità BLE, log BLE,
 * factory reset): non è un recupero e non serve il deep sleep per resettare la radio. I riavvii
 * di recupero (watchdog Zigbee, hard reset, reset PHY) restituiscono false. Valido dopo diag_boot(). */
bool diag_restart_was_commanded(void);

/* Task a bassa priorità che scrive in NVS i contatori modificati. */
void diag_start_task(void);

/* Scrive subito in NVS la causa del riavvio imminente (chiamare prima di esp_restart). */
void diag_set_restart_cause(diag_cause_t cause);

void diag_event(diag_event_t evt);

/* Cluster Zigbee custom (NULL se la creazione fallisce). */
esp_zb_attribute_list_t *diag_zb_cluster_create(void);

/* Aggiorna gli attributi del cluster; chiamare dal contesto del task Zigbee. */
void diag_zb_update(uint8_t endpoint);

/* Invia al coordinator, una sola volta, il contatore "riavvii anomali" (report singolo che ignora
 * il reporting configurato da ZHA). Serve all'allarme in Home Assistant. Chiamare dal contesto
 * del task Zigbee, quando il coordinator è raggiungibile. */
void diag_zb_report_to_coordinator(uint8_t endpoint);
