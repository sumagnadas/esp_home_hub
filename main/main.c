#include "wifi_sta.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "microlink.h"
#include "ml_config_httpd.h"
#include "http_ep.h"

static const char *TAG = "main";
static void on_state_change(microlink_t *ml_handle, microlink_state_t state, void *user_data)
{
    const char *state_names[] = {
        "IDLE", "WIFI_WAIT", "CONNECTING", "REGISTERING",
        "CONNECTED", "RECONNECTING", "ERROR"};
    const char *name = (state < sizeof(state_names) / sizeof(state_names[0]))
                           ? state_names[state]
                           : "UNKNOWN";
    ESP_LOGI(TAG, "MicroLink state: %s", name);

    if (state == ML_STATE_CONNECTED)
    {
        uint32_t ip = microlink_get_vpn_ip(ml_handle);
        char ip_str[16];
        microlink_ip_to_str(ip, ip_str);
        ESP_LOGI(TAG, "Connected! VPN IP: %s", ip_str);
    }
}

static void on_peer_update(microlink_t *ml_handle, const microlink_peer_info_t *peer,
                           void *user_data)
{
    char ip_str[16];
    microlink_ip_to_str(peer->vpn_ip, ip_str);
    ESP_LOGI(TAG, "Peer: %s (%s) online=%d direct=%d",
             peer->hostname, ip_str, peer->online, peer->direct_path);
}
void app_main(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "MicroLink v2 Basic Connect + Messaging Example");
    ESP_LOGI(TAG, "Free heap: %lu bytes (PSRAM: %lu bytes)",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // Initialize WiFi first...
    wifi_init();

    /* Wait for WiFi connection */
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);

    // Configure MicroLink
    microlink_config_t config = {
        .auth_key = CONFIG_ML_TAILSCALE_AUTH_KEY,
        .device_name = CONFIG_ML_DEVICE_NAME,
        .enable_derp = true,
        .enable_stun = true,
        .enable_disco = true,
        .max_peers = CONFIG_ML_MAX_PEERS,
        .wifi_tx_power_dbm = 13, /* Reduced for thermal management */
    };

    // Initialize ML
    struct microlink_s *ml = microlink_init(&config);
    if (!ml)
    {
        ESP_LOGE(TAG, "Failed to initialize MicroLink");
        return;
    }

    /* Register callbacks */
    microlink_set_state_callback(ml, on_state_change, NULL);
    microlink_set_peer_callback(ml, on_peer_update, NULL);

    /* Start connecting */
    ESP_ERROR_CHECK(microlink_start(ml));

    /* Wait for CONNECTED state before creating UDP socket */
    while (!microlink_is_connected(ml))
    {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    start_webserver(ml);

    // Main loop
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
