# BLE-MAC-Adressen auf Nordic-Chips (Zephyr / NCS)

> 🌐 [English version](../en/ble-mac-addresses.md)

## Adresstypen

BLE unterscheidet zwei grundlegende Adresskategorien:

| Typ | Beschreibung |
|-----|-------------|
| **Public** | Global eindeutig, IEEE-registriertes OUI. Vom Hersteller ins Hardware eingebrannt. Nordic-nRF52/nRF53-Chips besitzen **keine** werkseitig programmierte Public-Adresse. |
| **Random** | Softwareseitig generiert. Unterteilt in *Static*, *Private Resolvable* (RPA) und *Private Non-Resolvable* (NRPA). |

## Standardverhalten auf Nordic-Chips

Ohne explizite Konfiguration erzeugt der Zephyr-BT-Stack beim ersten Boot eine **Random Static Address**:

- Bit 47 und 46 der Adresse sind beide auf `1` gesetzt (gemäß BLE-Spezifikation §1.3).
- Die Adresse wird im NVS/Settings-Flash gespeichert und **überlebt Neustarts**.
- Sie wirkt „zufällig", ist jedoch für die gesamte Lebensdauer des Geräts stabil (oder bis der Flash gelöscht wird).

Beispiel aus dem seriellen Log:
```
Identity: CC:8F:EB:B8:55:B8 (random)
```

## Wirkung von `BT_LE_ADV_OPT_USE_IDENTITY`

Standardmäßig verwendet Zephyr während des Advertisings **Resolvable Private Addresses (RPA)**, um Tracking zu verhindern. Das Setzen von `BT_LE_ADV_OPT_USE_IDENTITY` in den Advertising-Parametern zwingt den Stack dazu, stattdessen die Identity-Adresse (die Random Static Address) zu verwenden:

```c
BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_USE_IDENTITY, ...)
```

Für BThome-Sensoren ist dies die **richtige Wahl**: Home-Automation-Hubs (z. B. Home Assistant) identifizieren Sensoren anhand ihrer MAC-Adresse. Eine sich ändernde RPA würde nach jedem Rotationsintervall als neues, unbekanntes Gerät behandelt.

## Kconfig-Optionen

| Option | Wirkung |
|--------|---------|
| `CONFIG_BT_PRIVACY=y` | Aktiviert die RPA-Generierung (standardmäßig an). Ohne `USE_IDENTITY` verwendet Advertising RPAs. |
| `CONFIG_BT_SETTINGS=y` | Speichert die Identity-Adresse im NVS über Neustarts hinweg persistent. |
| `CONFIG_BT_FIXED_ADDRESS` | (Auf nRF52 ohne Custom-HAL nicht verfügbar) — würde eine fest kodierte Adresse ermöglichen. |

## Benutzerdefinierte Adresse zur Laufzeit setzen

Wenn eine deterministische Adresse benötigt wird (z. B. basierend auf der Seriennummer des Geräts), kann sie vor `bt_enable()` gesetzt werden:

```c
bt_addr_le_t addr = {
    .type = BT_ADDR_LE_RANDOM,
    .a    = { .val = { 0xC0, 0xFF, 0xEE, 0x01, 0x02, 0xC3 } },
    /*                                               ^^-- Bits 47:46 müssen 11 sein */
};
bt_id_create(&addr, NULL);
bt_enable(bt_ready);
```

> **Hinweis:** Bits 47 und 46 einer Random Static Address müssen beide `1` sein. Im obigen Beispiel wird `0xC3` als höchstwertiges Byte verwendet (`1100 0011`), was diese Anforderung erfüllt.

## Ändert sich die Adresse bei jedem Flashvorgang?

**Ohne `CONFIG_BT_SETTINGS=y`** (Standardeinstellung / aktueller Zustand der Samples):

- Die Adresse wird zur Laufzeit aus `sys_rand_get()` generiert — bei jedem Boot ein neuer Wert.
- Jeder Neustart und jedes `west flash` erzeugt eine andere Adresse.
- Home Assistant / bthome-logger behandelt das Gerät nach jedem Neustart als **neuen, unbekannten Sensor**.

**Mit `CONFIG_BT_SETTINGS=y`**:

- Die Adresse wird einmalig generiert und in die NVS-Flash-Partition geschrieben.
- `west flash` (ohne `--erase`) schreibt nur das Anwendungs-Image — die NVS-Partition wird **nicht berührt** → die Adresse bleibt über Reflash-Vorgänge hinweg erhalten.
- Ein vollständiges Chip-Erase (`west flash --erase` oder `nrfjprog --eraseall`) löscht den NVS → beim nächsten Boot wird eine neue Adresse generiert.

Erforderliche Kconfig-Ergänzungen für stabile Adressen:

```kconfig
CONFIG_BT_SETTINGS=y
CONFIG_SETTINGS=y
CONFIG_NVS=y          # oder CONFIG_ZMS=y bei neueren NCS-Versionen
CONFIG_FLASH=y
CONFIG_FLASH_PAGE_LAYOUT=y
CONFIG_FLASH_MAP=y
```

## Zusammenfassung für BThome-Nodes

- Ohne `CONFIG_BT_SETTINGS` ändert sich die Adresse bei jedem Neustart — **nicht für den Produktiveinsatz geeignet**.
- Den Settings-Stack (siehe oben) hinzufügen, um eine stabile Adresse über normale Reflash-Vorgänge hinweg zu erhalten.
- `BT_LE_ADV_OPT_USE_IDENTITY` in allen Advertising-Parameter-Strukturen verwenden, damit der Hub stets dieselbe MAC-Adresse sieht.
