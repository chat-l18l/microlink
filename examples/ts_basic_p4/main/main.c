/**
 * @file main.c
 * @brief MicroLink v2 ESP32-P4 Ethernet skeleton example
 *
 * ESP32-P4 has no built-in WiFi. This example is intentionally Ethernet-first:
 * - initialize NVS, esp_netif and the default event loop
 * - start ESP32-P4 internal EMAC with IP101 PHY on Olimex ESP32-P4-DevKit pins
 * - wait for an Ethernet IPv4 address
 * - start MicroLink and expose a UDP echo socket on port 9000
 */

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth_phy.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "microlink.h"
#include "microlink_internal.h"  /* For task handle access (diagnostic) */

static const char *TAG = "ts_basic_p4";

#define MSG_PORT             9000
#define MSG_SEND_INTERVAL_MS 5000

/* User LED (used as link/IP status indicator) */
#define LED_GPIO             GPIO_NUM_2

/* Ethernet: ESP32-P4 internal EMAC + IP101 PHY (RMII)
 * RMII data-plane pins are bound by IO_MUX via ETH_ESP32_EMAC_DEFAULT_CONFIG(). */
#define ETH_MDC_GPIO         GPIO_NUM_31
#define ETH_MDIO_GPIO        GPIO_NUM_52
#define ETH_PHY_RST_GPIO     GPIO_NUM_51
#define ETH_REF_CLK_GPIO     GPIO_NUM_50
#define ETH_PHY_ADDR         1

static EventGroupHandle_t net_event_group;
#define ETH_GOT_IP_BIT BIT0

static microlink_t *ml = NULL;
static microlink_udp_socket_t *udp_sock = NULL;
static esp_netif_t *eth_netif = NULL;
static esp_eth_handle_t eth_handle = NULL;
static uint32_t msg_tx_count = 0;
static uint32_t msg_rx_count = 0;

#define LOG_STEP(call) do { \
        esp_err_t _err = (call); \
        if (_err != ESP_OK) { \
            ESP_LOGE(TAG, "%s -> %s (0x%x)", #call, esp_err_to_name(_err), _err); \
            return _err; \
        } \
    } while (0)

static void led_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    gpio_set_level(LED_GPIO, 0);
}

static void on_udp_rx(microlink_udp_socket_t *sock, uint32_t src_ip, uint16_t src_port,
                      const uint8_t *data, size_t len, void *user_data) {
    (void)user_data;
    msg_rx_count++;

    char ip_str[16];
    microlink_ip_to_str(src_ip, ip_str);

    char msg[256];
    size_t copy_len = (len < sizeof(msg) - 1) ? len : sizeof(msg) - 1;
    memcpy(msg, data, copy_len);
    msg[copy_len] = '\0';
    if (copy_len > 0 && msg[copy_len - 1] == '\n') msg[copy_len - 1] = '\0';

    ESP_LOGI(TAG, "UDP RX #%lu from %s:%u [%d bytes]: \"%s\"",
             (unsigned long)msg_rx_count, ip_str, src_port, (int)len, msg);

    char reply[300];
    int reply_len = snprintf(reply, sizeof(reply), "ECHO: %s", msg);
    if (reply_len > 0) {
        esp_err_t err = microlink_udp_send(sock, src_ip, src_port, reply, reply_len);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "UDP TX echo -> %s:%u", ip_str, src_port);
        } else {
            ESP_LOGW(TAG, "UDP TX echo failed: %d", err);
        }
    }
}

static void on_state_change(microlink_t *ml_handle, microlink_state_t state, void *user_data) {
    (void)user_data;
    const char *state_names[] = {
        "IDLE", "WIFI_WAIT", "CONNECTING", "REGISTERING",
        "CONNECTED", "RECONNECTING", "ERROR"
    };
    const char *name = (state < sizeof(state_names) / sizeof(state_names[0]))
                       ? state_names[state] : "UNKNOWN";
    ESP_LOGI(TAG, "MicroLink state: %s", name);

    if (state == ML_STATE_CONNECTED) {
        uint32_t ip = microlink_get_vpn_ip(ml_handle);
        char ip_str[16];
        microlink_ip_to_str(ip, ip_str);
        ESP_LOGI(TAG, "Connected! VPN IP: %s", ip_str);
    }
}

static void on_peer_update(microlink_t *ml_handle, const microlink_peer_info_t *peer,
                           void *user_data) {
    (void)ml_handle;
    (void)user_data;
    char ip_str[16];
    microlink_ip_to_str(peer->vpn_ip, ip_str);
    ESP_LOGI(TAG, "Peer: %s (%s) online=%d direct=%d",
             peer->hostname, ip_str, peer->online, peer->direct_path);
}

static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data) {
    (void)arg;
    (void)event_base;
    (void)event_data;

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Ethernet link up");
        gpio_set_level(LED_GPIO, 1);
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Ethernet link down");
        gpio_set_level(LED_GPIO, 0);
        xEventGroupClearBits(net_event_group, ETH_GOT_IP_BIT);
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet stopped");
        break;
    default:
        break;
    }
}

static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data) {
    (void)arg;
    (void)event_base;
    (void)event_id;

    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Ethernet got IP: " IPSTR " mask " IPSTR " gw " IPSTR,
             IP2STR(&event->ip_info.ip),
             IP2STR(&event->ip_info.netmask),
             IP2STR(&event->ip_info.gw));
    gpio_set_level(LED_GPIO, 1);
    xEventGroupSetBits(net_event_group, ETH_GOT_IP_BIT);
}

static esp_err_t ethernet_start(void) {
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                               eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                               got_ip_event_handler, NULL));

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    eth_netif = esp_netif_new(&netif_cfg);
    if (!eth_netif) {
        ESP_LOGE(TAG, "esp_netif_new returned NULL");
        return ESP_FAIL;
    }

    /* MAC: internal EMAC, RMII, REF_CLK driven by the IP101 into the ESP32-P4. */
    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_cfg.smi_gpio.mdc_num = ETH_MDC_GPIO;
    emac_cfg.smi_gpio.mdio_num = ETH_MDIO_GPIO;
    emac_cfg.interface = EMAC_DATA_INTERFACE_RMII;
    emac_cfg.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_cfg.clock_config.rmii.clock_gpio = ETH_REF_CLK_GPIO;
    ESP_LOGI(TAG, "MAC cfg: MDC=%d MDIO=%d REF_CLK=%d (EXT_IN)",
             ETH_MDC_GPIO, ETH_MDIO_GPIO, ETH_REF_CLK_GPIO);

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);
    if (!mac) {
        ESP_LOGE(TAG, "esp_eth_mac_new_esp32 returned NULL");
        return ESP_FAIL;
    }

    /* PHY: IP101 on Olimex ESP32-P4-DevKit. */
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = ETH_PHY_ADDR;
    phy_cfg.reset_gpio_num = ETH_PHY_RST_GPIO;
    ESP_LOGI(TAG, "PHY cfg: model=IP101 addr=%d rst_gpio=%d",
             ETH_PHY_ADDR, ETH_PHY_RST_GPIO);

    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_cfg);
    if (!phy) {
        ESP_LOGE(TAG, "esp_eth_phy_new_ip101 returned NULL");
        return ESP_FAIL;
    }

    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_LOGI(TAG, "step: esp_eth_driver_install (probes PHY over MDIO)");
    LOG_STEP(esp_eth_driver_install(&eth_cfg, &eth_handle));

    ESP_LOGI(TAG, "step: esp_netif_attach");
    LOG_STEP(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)));

    ESP_LOGI(TAG, "Ethernet netif uses DHCP client");

    ESP_LOGI(TAG, "step: esp_eth_start");
    LOG_STEP(esp_eth_start(eth_handle));
    ESP_LOGI(TAG, "ethernet_start complete");
    return ESP_OK;
}

static void log_task_diagnostics(void) {
    static const char *sn[] = {
        "Running", "Ready", "Blocked", "Suspended", "Deleted", "Invalid"
    };
    struct { const char *name; TaskHandle_t handle; } tasks[] = {
        {"net_io",  ml->net_io_task},
        {"derp_tx", ml->derp_tx_task},
        {"coord",   ml->coord_task},
        {"wg_mgr",  ml->wg_mgr_task},
    };

    for (int t = 0; t < 4; t++) {
        if (tasks[t].handle) {
            eTaskState st = eTaskGetState(tasks[t].handle);
            int si = (st <= eInvalid) ? (int)st : 5;
            ESP_LOGW(TAG, "TASK[%s]: state=%s stack_free=%lu",
                     tasks[t].name, sn[si],
                     (unsigned long)uxTaskGetStackHighWaterMark(tasks[t].handle));
        }
    }
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "MicroLink v2 ESP32-P4 Ethernet skeleton example");
    ESP_LOGI(TAG, "Free heap: %lu bytes (PSRAM: %lu bytes)",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    led_init();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    net_event_group = xEventGroupCreate();
    if (!net_event_group) {
        ESP_LOGE(TAG, "Failed to create network event group");
        return;
    }

    ret = ethernet_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet start failed: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "Fill ethernet_start() with ESP32-P4 PHY/MAC/GPIO details first");
        return;
    }

    xEventGroupWaitBits(net_event_group, ETH_GOT_IP_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);

    microlink_config_t config = {
        .auth_key = CONFIG_ML_TAILSCALE_AUTH_KEY,
        .device_name = CONFIG_ML_DEVICE_NAME,
        .enable_derp = true,
        .enable_stun = true,
        .enable_disco = true,
        .max_peers = CONFIG_ML_MAX_PEERS,
        .wifi_tx_power_dbm = 0,
    };

    ml = microlink_init(&config);
    if (!ml) {
        ESP_LOGE(TAG, "Failed to initialize MicroLink");
        return;
    }

    microlink_set_state_callback(ml, on_state_change, NULL);
    microlink_set_peer_callback(ml, on_peer_update, NULL);

    ESP_ERROR_CHECK(microlink_start(ml));

    while (!microlink_is_connected(ml)) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    udp_sock = microlink_udp_create(ml, MSG_PORT);
    if (!udp_sock) {
        ESP_LOGE(TAG, "Failed to create UDP socket");
    } else {
        ESP_LOGI(TAG, "UDP socket listening on port %d", MSG_PORT);
        microlink_udp_set_rx_callback(udp_sock, on_udp_rx, NULL);
    }

    uint32_t target_ip = 0;
    const char *target_ip_str = CONFIG_ML_EXAMPLE_TARGET_PEER_IP;
    if (target_ip_str && target_ip_str[0] != '\0') {
        target_ip = microlink_parse_ip(target_ip_str);
        if (target_ip != 0) {
            ESP_LOGI(TAG, "Will send messages to %s:%d every %dms",
                     target_ip_str, MSG_PORT, MSG_SEND_INTERVAL_MS);
        } else {
            ESP_LOGW(TAG, "Invalid target IP: '%s'", target_ip_str);
        }
    } else {
        ESP_LOGI(TAG, "No target peer IP configured (receive-only mode)");
    }

    uint64_t last_send_ms = 0;
    uint64_t last_status_ms = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        uint64_t now = (uint64_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

        if (udp_sock && target_ip != 0 && now - last_send_ms >= MSG_SEND_INTERVAL_MS) {
            last_send_ms = now;
            msg_tx_count++;

            char msg[128];
            int msg_len = snprintf(msg, sizeof(msg), "hello from ESP32-P4 #%lu",
                                   (unsigned long)msg_tx_count);
            esp_err_t err = microlink_udp_send(udp_sock, target_ip, MSG_PORT, msg, msg_len);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "UDP TX #%lu -> %s:%d: \"%s\"",
                         (unsigned long)msg_tx_count, target_ip_str, MSG_PORT, msg);
            } else {
                ESP_LOGW(TAG, "UDP TX #%lu FAILED (err=%d)",
                         (unsigned long)msg_tx_count, err);
            }
        }

        if (now - last_status_ms >= 30000) {
            last_status_ms = now;

            if (microlink_is_connected(ml)) {
                int peer_count = microlink_get_peer_count(ml);
                ESP_LOGI(TAG, "Status: CONNECTED | Peers: %d | TX: %lu | RX: %lu | Heap: %lu",
                         peer_count, (unsigned long)msg_tx_count, (unsigned long)msg_rx_count,
                         (unsigned long)esp_get_free_heap_size());

                for (int i = 0; i < peer_count; i++) {
                    microlink_peer_info_t info;
                    if (microlink_get_peer_info(ml, i, &info) == ESP_OK) {
                        char ip_str[16];
                        microlink_ip_to_str(info.vpn_ip, ip_str);
                        ESP_LOGI(TAG, "  [%d] %s (%s) %s",
                                 i, info.hostname, ip_str,
                                 info.direct_path ? "DIRECT" : "DERP");
                    }
                }
            }

            log_task_diagnostics();
        }
    }
}
