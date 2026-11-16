"""Handmade! SmartSwitchO: contatori diagnostici (vedi firmware/main/diag.c)."""

from typing import Final

import zigpy.types as t
from zigpy.zcl.clusters.general import OnOff
from zigpy.zcl.foundation import BaseAttributeDefs, ZCLAttributeDef

from zhaquirks.builder import EntityType, QuirkBuilder
from zhaquirks.clusters import CustomCluster


def _ro(attr_id: int) -> ZCLAttributeDef:
    # is_manufacturer_specific=False: sul firmware gli attributi sono registrati senza
    # codice costruttore, e zigpy per i cluster 0xFC00+ lo aggiungerebbe ai frame.
    return ZCLAttributeDef(
        id=attr_id, type=t.uint32_t, access="r", is_manufacturer_specific=False
    )


class SmartSwitchDiag(CustomCluster):
    """Cluster diagnostico privato."""

    cluster_id: t.uint16_t = 0xFF11
    name: str = "SmartSwitch Diag"
    ep_attribute: str = "smartswitch_diag"

    class AttributeDefs(BaseAttributeDefs):
        """Attribute definitions."""

        uptime_s: Final = _ro(0x0000)
        boots: Final = _ro(0x0001)
        last_event: Final = _ro(0x0002)
        rr_poweron: Final = _ro(0x0003)
        rr_brownout: Final = _ro(0x0004)
        rr_panic: Final = _ro(0x0005)
        rr_wdt: Final = _ro(0x0006)
        sw_untagged: Final = _ro(0x0007)
        cs_zb_wdt: Final = _ro(0x0008)
        cs_hard_reset: Final = _ro(0x0009)
        cs_phy_reset: Final = _ro(0x000A)
        cs_other: Final = _ro(0x000B)
        ev_ping_fail: Final = _ro(0x000C)
        ev_no_links: Final = _ro(0x000D)
        ev_leave: Final = _ro(0x000E)
        ev_steer_fail: Final = _ro(0x000F)
        # somma di brownout + panic + watchdog + reset software senza causa; con reporting.
        unexpected_resets: Final = ZCLAttributeDef(
            id=0x0010, type=t.uint32_t, access="rp", is_manufacturer_specific=False
        )


def _virtual_switch(builder, endpoint_id, key, name, enabled=True):
    """Dà un nome all'interruttore On/Off virtuale (nessun relay collegato)."""
    return builder.change_entity_metadata(
        endpoint_id=endpoint_id,
        cluster_id=OnOff.cluster_id,
        new_primary=False,
        new_translation_key=key,
        new_fallback_name=name,
        new_entity_registry_enabled_default=enabled,
    )


builder = QuirkBuilder("Handmade!", "SmartSwitchO").replaces(SmartSwitchDiag)
# EP10 = BLE (aggiornamento via Bluetooth); EP11 = PROG (modalità programmazione seriale);
# EP12 = RESCRED (cancella le credenziali Zigbee); EP13 = LOG (log via BLE: ON attiva e resta attivo
# anche dopo i riavvii, OFF o BOOT breve lo spegne). Tutti abilitati.
builder = _virtual_switch(builder, 10, "ble_update", "BLE")
builder = _virtual_switch(builder, 11, "programming_mode", "PROG")
builder = _virtual_switch(builder, 12, "zigbee_factory_reset", "RESCRED")
builder = _virtual_switch(builder, 13, "ble_log", "LOG")
# Sensore "Riavvii anomali": sale di 1 a ogni riavvio non voluto.
# SENZA reporting_config di proposito: con il reporting configurato da ZHA il dispositivo andava in
# crash a ogni avvio (ZBOSS, record di reporting senza descrittore). Per ora il valore non arriva da solo.
builder = builder.sensor(
    attribute_name=SmartSwitchDiag.AttributeDefs.unexpected_resets.name,
    cluster_id=SmartSwitchDiag.cluster_id,
    endpoint_id=1,
    entity_type=EntityType.DIAGNOSTIC,
    translation_key="unexpected_resets",
    fallback_name="Riavvii anomali",
)
builder.add_to_registry()
