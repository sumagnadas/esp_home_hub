#include "wifi_sta.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif.h"
#include <string.h>
#include "microlink_internal.h" /* For task handle access (diagnostic) */

#include "lwip/err.h"
#include "lwip/sys.h"

static char wifi_ssid[33] = CONFIG_ML_WIFI_SSID;
static char wifi_password[65] = CONFIG_ML_WIFI_PASSWORD;
#define WIFI_MAX_RETRIES_PER_SSID 10 // CONFIG_ESP_MAXIMUM_RETRY

#if 1
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HUNT_AND_PECK
#define EXAMPLE_H2E_IDENTIFIER ""
#elif CONFIG_ESP_WPA3_SAE_PWE_HASH_TO_ELEMENT
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HASH_TO_ELEMENT
#define EXAMPLE_H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID
#elif CONFIG_ESP_WPA3_SAE_PWE_BOTH
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_BOTH
#define EXAMPLE_H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID
#endif
#if CONFIG_ESP_WIFI_AUTH_OPEN
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_OPEN
#elif CONFIG_ESP_WIFI_AUTH_WEP
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WEP
#elif CONFIG_ESP_WIFI_AUTH_WPA_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA3_PSK
#elif 1
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WAPI_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WAPI_PSK
#endif

static ml_config_wifi_list_t wifi_list;
static int wifi_list_count = 0; /* 0 = single SSID mode */
static int current_wifi_idx = 0;
static int wifi_retry_count = 0;

static const char *TAG = "wifi_sta";
EventGroupHandle_t wifi_event_group;

/* ============================================================================
 * WiFi Setup
 * ========================================================================== */

/* Switch to the next WiFi SSID in the list (round-robin) */
static void wifi_try_next(void)
{
    if (wifi_list_count <= 1)
    {
        /* Single SSID: just reconnect */
        esp_wifi_connect();
        return;
    }

    wifi_retry_count++;
    if (wifi_retry_count >= WIFI_MAX_RETRIES_PER_SSID)
    {
        wifi_retry_count = 0;
        current_wifi_idx = (current_wifi_idx + 1) % wifi_list_count;
    }

    /* Update wifi_config with new SSID/pass */
    ml_config_wifi_entry_t *e = &wifi_list.entries[current_wifi_idx];
    wifi_config_t wifi_config = {
        .sta = {.threshold.authmode = WIFI_AUTH_WPA2_PSK},
    };
    strncpy((char *)wifi_config.sta.ssid, e->ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, e->pass, sizeof(wifi_config.sta.password) - 1);

    ESP_LOGI(TAG, "WiFi trying #%d/%d: %s (retry %d/%d)",
             current_wifi_idx + 1, wifi_list_count, e->ssid,
             wifi_retry_count + 1, WIFI_MAX_RETRIES_PER_SSID);

    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_connect();
}

void wifi_event_handler(void *arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "WiFi disconnected, reason=%d", disc->reason);
        wifi_try_next();
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi connected to %s, IP: " IPSTR,
                 wifi_list_count > 0 ? wifi_list.entries[current_wifi_idx].ssid : wifi_ssid,
                 IP2STR(&event->ip_info.ip));
        wifi_retry_count = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init(void)
{

    /* Check NVS for saved WiFi credentials (from web config UI) */
    memset(&wifi_list, 0, sizeof(wifi_list));
    wifi_list.active_idx = 0xFF;

    if (ml_config_get_wifi_list(&wifi_list) && wifi_list.count > 1)
    {
        /* Multi-SSID mode */
        wifi_list_count = wifi_list.count;
        current_wifi_idx = 0;
        strncpy(wifi_ssid, wifi_list.entries[0].ssid, sizeof(wifi_ssid) - 1);
        strncpy(wifi_password, wifi_list.entries[0].pass, sizeof(wifi_password) - 1);
        ESP_LOGI(TAG, "WiFi multi-SSID: %d networks (first: %s)", wifi_list_count, wifi_ssid);
        for (int i = 0; i < wifi_list_count; i++)
        {
            ESP_LOGI(TAG, "  WiFi #%d: %s", i + 1, wifi_list.entries[i].ssid);
        }
    }
    else if (ml_config_get_nvs_wifi(wifi_ssid, sizeof(wifi_ssid),
                                    wifi_password, sizeof(wifi_password)))
    {
        ESP_LOGI(TAG, "Using NVS WiFi: %s", wifi_ssid);
    }
    else
    {
        ESP_LOGI(TAG, "Using Kconfig WiFi: %s", wifi_ssid);
    }

    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char *)wifi_config.sta.ssid, wifi_ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, wifi_password, sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Disable WiFi power save for low-latency WireGuard traffic.
     * ESP-IDF defaults to WIFI_PS_MIN_MODEM which adds up to one DTIM
     * interval (100-300ms) of delay per packet wake cycle. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "WiFi init complete (PS=NONE), connecting to %s...", wifi_ssid);
}