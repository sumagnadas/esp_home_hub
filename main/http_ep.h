#include "esp_http_server.h"
#include "microlink_internal.h"

#define MAX_MACHINES_ENTRIES 16

typedef struct
{
    char ip[16];
    char name[100];
    uint8_t mac_addr[6];
    volatile int status;
    volatile int is_pinging;
} machine_t;

void start_webserver(microlink_t *);