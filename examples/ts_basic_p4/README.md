# MicroLink ESP32-P4 Ethernet Skeleton

ESP32-P4 heeft geen ingebouwde WiFi. Dit example gebruikt de interne ESP32-P4 EMAC met IP101 PHY via RMII, gebaseerd op de Olimex ESP32-P4-DevKit wiring.

## Status

Dit is nog een skeleton voor MicroLink op Ethernet, maar de eerste hardwareconfig staat erin:

1. `main/ethernet_start()` configureert `ESP_NETIF_DEFAULT_ETH()`, interne EMAC, RMII en IP101.
2. Ethernet gebruikt DHCP zodat IP, gateway en DNS van de uplink komen.
3. User LED op GPIO2 wordt gebruikt als link/IP indicatie.
4. Na Ethernet `GOT_IP` start het example MicroLink en een UDP echo socket op poort `9000`.

## Hardware

Deze configuratie is bedoeld voor de Olimex ESP32-P4-DevKit met IP101 Ethernet PHY.

De ESP32-P4 gebruikt de interne EMAC in RMII mode. De RMII data-plane signalen zoals `TXD0`, `TXD1`, `TX_EN`, `RXD0`, `RXD1` en `CRS_DV` worden door de ESP-IDF EMAC driver via IO_MUX ingesteld met `ETH_ESP32_EMAC_DEFAULT_CONFIG()`. Ze staan daarom niet als normale GPIO-configuratie in `main.c`.

De control-plane en clock pins staan expliciet in `examples/ts_basic_p4/main/main.c`:

| Functie | GPIO |
|---|---:|
| LED | 2 |
| MDC | 31 |
| MDIO | 52 |
| PHY reset | 51 |
| REF_CLK input | 50 |
| PHY addr | 1 |

Ethernet instellingen in code:

| Instelling | Waarde |
|---|---|
| MAC | ESP32-P4 internal EMAC |
| Interface | RMII |
| PHY | IP101 |
| PHY address | `1` |
| RMII clock mode | external clock into ESP32-P4 |
| REF_CLK GPIO | `GPIO50` |
| IPv4 config | DHCP client via `ESP_NETIF_DEFAULT_ETH()` |
| MicroLink UDP demo port | `9000` |

De relevante code staat in `ethernet_start()`:

1. `ESP_NETIF_DEFAULT_ETH()` maakt de Ethernet netif.
2. `ETH_ESP32_EMAC_DEFAULT_CONFIG()` maakt de interne EMAC config.
3. `emac_cfg.smi_gpio.mdc_num` en `emac_cfg.smi_gpio.mdio_num` zetten MDC/MDIO.
4. `emac_cfg.interface = EMAC_DATA_INTERFACE_RMII` kiest RMII.
5. `emac_cfg.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN` verwacht REF_CLK van de PHY.
6. `esp_eth_phy_new_ip101()` selecteert de IP101 PHY driver.
7. De default Ethernet netif start de DHCP client; `IP_EVENT_ETH_GOT_IP` start daarna MicroLink.

Als je een ander ESP32-P4 board gebruikt, pas eerst deze defines aan in `main.c`:

```c
#define ETH_MDC_GPIO         GPIO_NUM_31
#define ETH_MDIO_GPIO        GPIO_NUM_52
#define ETH_PHY_RST_GPIO     GPIO_NUM_51
#define ETH_REF_CLK_GPIO     GPIO_NUM_50
#define ETH_PHY_ADDR         1
```

Voor Tailscale control-plane/DERP is een route naar internet nodig. Daarom gebruikt dit example standaard DHCP in plaats van een back-to-back static IP.

## Nog Te Doen

Open punten:

1. Test of de ESP-IDF versie voor ESP32-P4 dezelfde EMAC/IP101 API namen gebruikt.
2. Controleer op hardware of DHCP, gateway en DNS correct binnenkomen via `IP_EVENT_ETH_GOT_IP`.

Belangrijke vervolgstap: de MicroLink component zelf heeft nu nog `esp_wifi` als dependency. Voor een zuivere ESP32-P4 build moet die WiFi dependency waarschijnlijk optioneel gemaakt worden.
