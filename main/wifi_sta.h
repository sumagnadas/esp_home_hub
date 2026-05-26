#include "esp_wifi.h"

/* FreeRTOS event group to signal when we are connected*/
extern EventGroupHandle_t wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

void wifi_init(void);

void wifi_event_handler(void *arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data);