#include "sdkconfig.h"

#include <string.h>
#include <stdlib.h>
#include "http_ep.h"
#include "cJSON.h"
#include "esp_heap_caps.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#include <inttypes.h>
#include "esp_wifi_default.h"

#include "ping/ping_sock.h"

#include "ml_config_httpd.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "http_server";

typedef struct __attribute__((packed))
{
    uint8_t count; /* Number of entries (0..16) */
    machine_t *entries;
} machine_list_t; /* 1570 bytes max */

machine_t *machines = NULL;

machine_list_t curr_list = {};
/*
    {.ip = "192.168.29.60",
     .name = "NAS",
     .mac_addr = {0x18, 0x60, 0x24, 0xbf, 0x60, 0xf3},
     .status = 0,
     .is_pinging = 0},*/

/* Helper: send JSON response */
static esp_err_t
send_json(httpd_req_t *req, cJSON *json)
{
    char *str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!str)
    {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, str);
    free(str);
    return ESP_OK;
}

/* Helper: read POST body into buffer */
static char *read_post_body(httpd_req_t *req)
{
    int content_len = req->content_len;
    if (content_len <= 0 || content_len > 4096)
    {
        return NULL;
    }
    char *buf = malloc(content_len + 1);
    if (!buf)
        return NULL;

    int received = 0;
    while (received < content_len)
    {
        int ret = httpd_req_recv(req, buf + received, content_len - received);
        if (ret <= 0)
        {
            free(buf);
            return NULL;
        }
        received += ret;
    }
    buf[content_len] = '\0';
    return buf;
}

static void test_on_ping_success(esp_ping_handle_t hdl, void *args)
{
    // optionally, get callback arguments
    // const char* str = (const char*) args;
    // printf("%s\r\n", str); // "foo"
    // int i = *(int *)args;

    uint8_t ttl;
    uint16_t seqno;
    uint32_t elapsed_time, recv_len;
    ip_addr_t target_addr;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TTL, &ttl, sizeof(ttl));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target_addr, sizeof(target_addr));
    esp_ping_get_profile(hdl, ESP_PING_PROF_SIZE, &recv_len, sizeof(recv_len));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed_time, sizeof(elapsed_time));
    ESP_LOGI(TAG, "%ld bytes from %s icmp_seq=%d ttl=%d time=%ld ms\n",
             recv_len, inet_ntoa(target_addr.u_addr.ip4), seqno, ttl, elapsed_time);
}

static void test_on_ping_timeout(esp_ping_handle_t hdl, void *args)
{
    uint16_t seqno;
    ip_addr_t target_addr;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target_addr, sizeof(target_addr));
    ESP_LOGI(TAG, "From %s icmp_seq=%d timeout\n", inet_ntoa(target_addr.u_addr.ip4), seqno);
}

static void test_on_ping_end(esp_ping_handle_t hdl, void *args)
{
    uint32_t transmitted;
    uint32_t received;
    uint32_t total_time_ms;
    machine_t *mach = (machine_t *)args;

    esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST, &transmitted, sizeof(transmitted));
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &received, sizeof(received));
    esp_ping_get_profile(hdl, ESP_PING_PROF_DURATION, &total_time_ms, sizeof(total_time_ms));
    ESP_LOGI(TAG, "%ld packets transmitted, %ld received, time %ldms\n", transmitted, received, total_time_ms);
    if (received == 0)
        mach->status = 0;
    else
        mach->status = 1;

    esp_ping_stop(hdl);
    esp_ping_delete_session(hdl); // ← add this

    mach->is_pinging = 0;
}

/* Helper: start ping-ing a machine */ /* credit: nopnop2002/esp-idf-ping */
static esp_err_t ping(int mach_id)
{

    esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();

    /* convert URL to IP address */
    ip_addr_t target_addr;
    memset(&target_addr, 0, sizeof(target_addr));
    struct addrinfo hint;
    memset(&hint, 0, sizeof(hint));
    struct addrinfo *res = NULL;

    int err = getaddrinfo(curr_list.entries[mach_id].ip, NULL, &hint, &res);
    if (err != 0 || res == NULL)
    {
        ESP_LOGE(TAG, "DNS lookup failed err=%d res=%p", err, res);

        return ESP_FAIL;
    }
    else
    {
        ESP_LOGI(TAG, "DNS lookup success");
    }

    if (res->ai_family == AF_INET)
    {
        struct in_addr addr4 = ((struct sockaddr_in *)(res->ai_addr))->sin_addr;
        inet_addr_to_ip4addr(ip_2_ip4(&target_addr), &addr4);
    }
    else
    {
        struct in6_addr addr6 = ((struct sockaddr_in6 *)(res->ai_addr))->sin6_addr;
        inet6_addr_to_ip6addr(ip_2_ip6(&target_addr), &addr6);
    }
    freeaddrinfo(res);
    ESP_LOGI(TAG, "target_addr.type=%d", target_addr.type);
    ESP_LOGI(TAG, "target_addr.u_addr.ip4=%d.%d.%d.%d", IP2STR(&(target_addr.u_addr.ip4)));
    ping_config.target_addr = target_addr; // target IP address

    ping_config.count = 1;
    ping_config.interval_ms = 1000;
    ping_config.task_prio = 1;

    /* set callback functions */
    esp_ping_callbacks_t cbs = {
        .on_ping_success = test_on_ping_success,
        .on_ping_timeout = test_on_ping_timeout,
        .on_ping_end = test_on_ping_end,
        .cb_args = &curr_list.entries[mach_id]};
    esp_ping_handle_t ping_hdl;
    esp_err_t ret = esp_ping_new_session(&ping_config, &cbs, &ping_hdl);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to create ping session: %d", ret);
        return ESP_FAIL;
    }
    esp_ping_start(ping_hdl);
    ESP_LOGI(TAG, "ping start");
    curr_list.entries[mach_id].is_pinging = 1;
    return ESP_OK;
}

// Background task - runs independently, updates status periodically
void ping_monitor_task(void *arg)
{
    while (1)
    {
        for (int i = 0; i < curr_list.count; i++)
        {
            if (!curr_list.entries[i].is_pinging)
            {
                ping(i);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10000)); // ping every 10s
    }
}

/* GET /status - all known endpoints in home lab */
esp_err_t lan_status_handler(httpd_req_t *req)
{
    /* Send a simple response */
    cJSON *json = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(json, "machines");
    for (int i = 0; i < curr_list.count; i++)
    {
        cJSON *mach = cJSON_CreateObject();
        cJSON_AddNumberToObject(mach, "id", i);
        cJSON_AddStringToObject(mach, "ip", curr_list.entries[i].ip);
        cJSON_AddStringToObject(mach, "name", curr_list.entries[i].name);
        cJSON_AddStringToObject(mach, "status", curr_list.entries[i].status ? "Healthy" : "Unhealthy");
        cJSON_AddItemToArray(arr, mach);
    }
    return send_json(req, json);
}

/* Helper */

/* POST /wol - Send a magic Wake-On-LAN packet to the specified machine */
esp_err_t WOL_handler(httpd_req_t *req)
{
    char *body = read_post_body(req);
    if (!body)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }
    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    // Start the UDP socket
    int sock, optval = 1;
    char mac[18] = "";
    if ((sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0)
    {
        ESP_LOGE(TAG, "Cannot open socket: %s ...!\n", strerror(errno));
        return ESP_FAIL;
    }

    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (char *)&optval, sizeof(optval)) < 0)
    {
        ESP_LOGE(TAG, "Cannot set socket options: %s ...!\n", strerror(errno));
        return ESP_FAIL;
    }

    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = inet_addr("255.255.255.255");
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(9);
    machine_t *mach = NULL;
    cJSON *item;
    if ((item = cJSON_GetObjectItem(json, "index")) && cJSON_IsNumber(item))
    {
        int index = (int)item->valuedouble;
        if (index >= 0 && index < curr_list.count)
        {
            mach = &curr_list.entries[index];
        }
        else
            return ESP_FAIL;
    }
    else if ((item = cJSON_GetObjectItem(json, "name")) && cJSON_IsString(item))
    {
        for (int i = 0; i < curr_list.count; i++)
        {
            if (strcmp(item->valuestring, curr_list.entries[i].name) == 0)
            {
                mach = &curr_list.entries[i];
                break;
            }
        }
        if (!mach)
            return ESP_FAIL;
    }
    else
        return ESP_FAIL;
    // else if (mac)
    // {
    //     // match mach by mac
    // }
    uint8_t payload[17 * 6];
    memset(payload, 0xFF, 6); // first 6 bytes = FF FF FF FF FF FF
    if (mach)
    {
        for (int i = 1; i < 17; i++)
            for (int j = 0; j < 6; j++)
                payload[i * 6 + j] = mach->mac_addr[j];

        int err = sendto(sock, payload, 17 * 6, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        close(sock);
        if (err < 0)
        {
            httpd_resp_sendstr(req, "Failure");
            cJSON_Delete(json);
            return ESP_FAIL;
        }
        httpd_resp_sendstr(req, "Done");
        cJSON_Delete(json);
        return ESP_OK;
    }
    else
    {
        close(sock);
        httpd_resp_sendstr(req, "Failure");
        cJSON_Delete(json);
        return ESP_FAIL;
    }
}

/* GET /wol - Get a dropdown list of machines to send Wake-On-LAN packet to */
esp_err_t WOL_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, WOL_PAGE_HTML);
    return ESP_OK;
}

static esp_err_t update_machines_handler(httpd_req_t *req)
{
    char *body = read_post_body(req);
    if (!body)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }

    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *machines = cJSON_GetObjectItem(json, "machines");
    if (!machines || !cJSON_IsArray(machines))
    {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing machines array");
        return ESP_FAIL;
    }

    int arr_size = cJSON_GetArraySize(machines);
    curr_list.count = 0;

    machine_list_t old = {
        .count = curr_list.count,
        .entries = curr_list.entries};
    curr_list.count = 0;
    curr_list.entries = NULL;
    for (int i = 0; i < arr_size && i < MAX_MACHINES_ENTRIES; i++)
    {
        cJSON *entry = cJSON_GetArrayItem(machines, i);
        cJSON *name = cJSON_GetObjectItem(entry, "name");
        cJSON *mac = cJSON_GetObjectItem(entry, "mac");
        cJSON *ip = cJSON_GetObjectItem(entry, "ip");
        int values[6];
        if (!name || !cJSON_IsString(name) || name->valuestring[0] == '\0')
            continue;
        if (!mac || !cJSON_IsString(mac) || mac->valuestring[0] == '\0' || (6 != sscanf(mac->valuestring, "%02x:%02x:%02x:%02x:%02x:%02x", &values[0], &values[1], &values[2], &values[3], &values[4], &values[5])))
            continue;
        if (!ip || !cJSON_IsString(ip) || ip->valuestring[0] == '\0')
            continue;

        curr_list.count++;
    }
    curr_list.entries = malloc(curr_list.count * sizeof(machine_t));
    for (int i = 0, id = 0; i < arr_size && i < MAX_MACHINES_ENTRIES; i++)
    {
        cJSON *entry = cJSON_GetArrayItem(machines, i);
        cJSON *name = cJSON_GetObjectItem(entry, "name");
        cJSON *mac = cJSON_GetObjectItem(entry, "mac");
        cJSON *ip = cJSON_GetObjectItem(entry, "ip");
        int values[6];
        if (!name || !cJSON_IsString(name) || name->valuestring[0] == '\0')
            continue;
        if (!mac || !cJSON_IsString(mac) || mac->valuestring[0] == '\0' || (6 != sscanf(mac->valuestring, "%02x:%02x:%02x:%02x:%02x:%02x", &values[0], &values[1], &values[2], &values[3], &values[4], &values[5])))
            continue;
        if (!ip || !cJSON_IsString(ip) || ip->valuestring[0] == '\0')
            continue;

        strcpy(curr_list.entries[id].name, name->valuestring);
        strcpy(curr_list.entries[id].ip, ip->valuestring);
        for (int i = 0; i < 6; ++i)
            curr_list.entries[id].mac_addr[i] = values[i];
        ESP_LOGW(TAG, "IP: %s", curr_list.entries[id].ip);
        ESP_LOGW(TAG, "MAC: %02x:%02x:%02x:%02x:%02x:%02x", curr_list.entries[id].mac_addr[0], curr_list.entries[id].mac_addr[1], curr_list.entries[id].mac_addr[2], curr_list.entries[id].mac_addr[3], curr_list.entries[id].mac_addr[4], curr_list.entries[id].mac_addr[5]);
        ESP_LOGW(TAG, "NAME: %s", curr_list.entries[id].name);
        curr_list.entries[id].status = false;
        curr_list.entries[id].is_pinging = false;
        id++;
    }
    cJSON_Delete(json);

    // save
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("nvs", NVS_READWRITE, &nvs);
    if (err != ESP_OK)
    {
        if (curr_list.entries)
            free(curr_list.entries);
        curr_list.count = old.count;
        curr_list.entries = old.entries;
        return err;
    }

    size_t save_len = 1 + curr_list.count * sizeof(machine_t);

    err = nvs_set_blob(nvs, "machines", &curr_list, save_len);
    cJSON *resp = cJSON_CreateObject();
    if (err == ESP_OK)
    {
        nvs_commit(nvs);
        ESP_LOGW(TAG, "Machines list saved (%d entries, %d bytes)",
                 curr_list.count, (int)save_len);
        cJSON_AddBoolToObject(resp, "ok", true);
        cJSON_AddNumberToObject(resp, "count", curr_list.count);
        if (old.entries)
            free(old.entries);
    }
    else
    {
        ESP_LOGE(TAG, "Failed to save machines list (%d bytes): %s",
                 (int)save_len, esp_err_to_name(err));
        if (curr_list.entries)
            free(curr_list.entries);
        curr_list.count = old.count;
        curr_list.entries = old.entries;
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON_AddNumberToObject(resp, "count", curr_list.count);
    }

    return send_json(req, resp);
}

/* GET /api/machines - all known endpoints in home lab */
esp_err_t get_machines_handler(httpd_req_t *req)
{
    /* Send a simple response */
    cJSON *json = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(json, "machines");
    for (int i = 0; i < curr_list.count; i++)
    {
        cJSON *mach = cJSON_CreateObject();
        cJSON_AddStringToObject(mach, "ip", curr_list.entries[i].ip);
        cJSON_AddStringToObject(mach, "name", curr_list.entries[i].name);
        uint8_t *mac = curr_list.entries[i].mac_addr;
        char macStr[18]; // 17 characters + null terminator
        sprintf(macStr, "%02x:%02x:%02x:%02x:%02x:%02x",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        cJSON_AddStringToObject(mach, "mac", macStr);
        cJSON_AddItemToArray(arr, mach);
    }
    return send_json(req, json);
}

/* URI objects */
static httpd_uri_t wol_post = {
    .uri = "/wol",
    .method = HTTP_POST,
    .handler = WOL_handler,
    .user_ctx = NULL};
static httpd_uri_t wol_get = {
    .uri = "/wol",
    .method = HTTP_GET,
    .handler = WOL_page,
    .user_ctx = NULL};
static httpd_uri_t status_get = {
    .uri = "/status",
    .method = HTTP_GET,
    .handler = lan_status_handler,
    .user_ctx = NULL};
static httpd_uri_t machines_post = {
    .uri = "/api/machines",
    .method = HTTP_POST,
    .handler = update_machines_handler,
    .user_ctx = NULL};
static httpd_uri_t machines_get = {
    .uri = "/api/machines",
    .method = HTTP_GET,
    .handler = get_machines_handler,
    .user_ctx = NULL};

void start_webserver(microlink_t *ml)
{
    // /* Empty handle to esp_http_server */
    httpd_handle_t server = NULL;
    server = ml_get_httpd_handle(ml->config_httpd);

    ESP_LOGI(TAG, "Server status: %s", server != NULL ? "On" : "Off");

    xTaskCreate(ping_monitor_task, "ping_monitor", 4096, NULL, 5, NULL);

    if (server)
    {
        httpd_register_uri_handler(server, &status_get);
        ESP_LOGI(TAG, "URI /status added");
        httpd_register_uri_handler(server, &wol_get);
        httpd_register_uri_handler(server, &wol_post);
        httpd_register_uri_handler(server, &machines_post);
        httpd_register_uri_handler(server, &machines_get);
        ESP_LOGI(TAG, "URI /wol added");
    }
}