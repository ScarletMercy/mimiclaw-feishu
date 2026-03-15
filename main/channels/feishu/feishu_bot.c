/**
 * @file feishu_bot.c
 * @brief Feishu Channel - Hybrid Implementation
 * 
 * This implementation combines the best features from both MimiClaw and EmbedClaw:
 * - Asynchronous task model (event queue + worker) from EmbedClaw
 * - NVS credentials management from MimiClaw
 * - FNV1a64 deduplication from MimiClaw
 * - App message filtering from EmbedClaw
 * - Network health check from EmbedClaw
 * - Complete Protobuf implementation from MimiClaw
 */

#include "feishu_bot.h"
#include "mimi_config.h"
#include "bus/message_bus.h"
#include "proxy/http_proxy.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs.h"
#include "cJSON.h"
#include "lwip/netdb.h"

static const char *TAG = "feishu";

/* ==================== Feishu API Endpoints ==================== */
#define FEISHU_API_BASE         "https://open.feishu.cn/open-apis"
#define FEISHU_AUTH_URL         FEISHU_API_BASE "/auth/v3/tenant_access_token/internal"
#define FEISHU_SEND_MSG_URL     FEISHU_API_BASE "/im/v1/messages"
#define FEISHU_REPLY_MSG_URL    FEISHU_API_BASE "/im/v1/messages/%s/reply"
#define FEISHU_WS_CONFIG_URL    "https://open.feishu.cn/callback/ws/endpoint"

/* ==================== Configuration ==================== */
#define FEISHU_TOKEN_LEN             512
#define FEISHU_WS_URL_MAX            512
#define FEISHU_EVENT_QUEUE_LEN       16
#define FEISHU_DEDUP_CACHE_SIZE      64
#define FEISHU_PING_INTERVAL_S       120
#define FEISHU_WS_RECONNECT_MS       30000
#define FEISHU_SEND_QPS_LIMIT        5
#define FEISHU_QPS_WINDOW_MS         1000

/* ==================== Credentials & Token State ==================== */
static char s_app_id[64] = MIMI_SECRET_FEISHU_APP_ID;
static char s_app_secret[128] = MIMI_SECRET_FEISHU_APP_SECRET;
static char s_tenant_token[FEISHU_TOKEN_LEN] = {0};
static int64_t s_token_expire_time = 0;

/* ==================== WebSocket State ==================== */
static esp_websocket_client_handle_t s_ws_client = NULL;
static TaskHandle_t s_ws_task = NULL;
static TaskHandle_t s_event_worker_task = NULL;
static TaskHandle_t s_token_refresh_task = NULL;
static char s_ws_url[FEISHU_WS_URL_MAX] = {0};
static int s_ws_service_id = 0;
static int s_ws_ping_interval_s = FEISHU_PING_INTERVAL_S;
static bool s_ws_connected = false;

/* ==================== Rate Limiting ==================== */
static int64_t s_send_window_start = 0;
static int s_send_count = 0;

/* ==================== Event Queue ==================== */
static QueueHandle_t s_ws_event_queue = NULL;

typedef struct {
    char *payload;
    size_t payload_len;
} feishu_event_job_t;

/* ==================== Message Deduplication (FNV1a64) ==================== */
static uint64_t s_seen_msg_keys[FEISHU_DEDUP_CACHE_SIZE] = {0};
static size_t s_seen_msg_idx = 0;

static uint64_t fnv1a64(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    if (!s) return h;
    while (*s) {
        h ^= (unsigned char)(*s++);
        h *= 1099511628211ULL;
    }
    return h;
}

static bool dedup_check_and_record(const char *message_id)
{
    uint64_t key = fnv1a64(message_id);
    for (size_t i = 0; i < FEISHU_DEDUP_CACHE_SIZE; i++) {
        if (s_seen_msg_keys[i] == key) return true;
    }
    s_seen_msg_keys[s_seen_msg_idx] = key;
    s_seen_msg_idx = (s_seen_msg_idx + 1) % FEISHU_DEDUP_CACHE_SIZE;
    return false;
}

/* ==================== HTTP Response Accumulator ==================== */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} http_resp_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_resp_t *resp = (http_resp_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (resp->len + evt->data_len >= resp->cap) {
            size_t new_cap = resp->cap * 2;
            if (new_cap < resp->len + evt->data_len + 1) {
                new_cap = resp->len + evt->data_len + 1;
            }
            char *tmp = realloc(resp->buf, new_cap);
            if (!tmp) return ESP_ERR_NO_MEM;
            resp->buf = tmp;
            resp->cap = new_cap;
        }
        memcpy(resp->buf + resp->len, evt->data, evt->data_len);
        resp->len += evt->data_len;
        resp->buf[resp->len] = '\0';
    }
    return ESP_OK;
}

/* ==================== Protobuf Frame Structures ==================== */
typedef struct {
    char key[32];
    char value[128];
} ws_header_t;

typedef struct {
    uint64_t seq_id;
    uint64_t log_id;
    int32_t service;
    int32_t method;
    ws_header_t headers[16];
    size_t header_count;
    const uint8_t *payload;
    size_t payload_len;
} ws_frame_t;

/* ==================== Protobuf Encoding/Decoding ==================== */
static bool pb_read_varint(const uint8_t *buf, size_t len, size_t *pos, uint64_t *out)
{
    uint64_t v = 0;
    int shift = 0;
    while (*pos < len && shift <= 63) {
        uint8_t b = buf[(*pos)++];
        v |= ((uint64_t)(b & 0x7F)) << shift;
        if ((b & 0x80) == 0) {
            *out = v;
            return true;
        }
        shift += 7;
    }
    return false;
}

static bool pb_skip_field(const uint8_t *buf, size_t len, size_t *pos, uint8_t wire_type)
{
    uint64_t n = 0;
    switch (wire_type) {
        case 0:
            return pb_read_varint(buf, len, pos, &n);
        case 1:
            if (*pos + 8 > len) return false;
            *pos += 8;
            return true;
        case 2:
            if (!pb_read_varint(buf, len, pos, &n)) return false;
            if (*pos + (size_t)n > len) return false;
            *pos += (size_t)n;
            return true;
        case 5:
            if (*pos + 4 > len) return false;
            *pos += 4;
            return true;
        default:
            return false;
    }
}

static bool pb_parse_header_msg(const uint8_t *buf, size_t len, ws_header_t *h)
{
    memset(h, 0, sizeof(*h));
    size_t pos = 0;
    while (pos < len) {
        uint64_t tag = 0, slen = 0;
        if (!pb_read_varint(buf, len, &pos, &tag)) return false;
        uint32_t field = (uint32_t)(tag >> 3);
        uint8_t wt = (uint8_t)(tag & 0x07);
        if (wt != 2) {
            if (!pb_skip_field(buf, len, &pos, wt)) return false;
            continue;
        }
        if (!pb_read_varint(buf, len, &pos, &slen)) return false;
        if (pos + (size_t)slen > len) return false;
        if (field == 1) {
            size_t n = (slen < sizeof(h->key) - 1) ? (size_t)slen : sizeof(h->key) - 1;
            memcpy(h->key, buf + pos, n);
            h->key[n] = '\0';
        } else if (field == 2) {
            size_t n = (slen < sizeof(h->value) - 1) ? (size_t)slen : sizeof(h->value) - 1;
            memcpy(h->value, buf + pos, n);
            h->value[n] = '\0';
        }
        pos += (size_t)slen;
    }
    return true;
}

static bool pb_parse_frame(const uint8_t *buf, size_t len, ws_frame_t *f)
{
    memset(f, 0, sizeof(*f));
    size_t pos = 0;
    while (pos < len) {
        uint64_t tag = 0, v = 0, blen = 0;
        if (!pb_read_varint(buf, len, &pos, &tag)) return false;
        uint32_t field = (uint32_t)(tag >> 3);
        uint8_t wt = (uint8_t)(tag & 0x07);
        if (field == 1 && wt == 0) {
            if (!pb_read_varint(buf, len, &pos, &f->seq_id)) return false;
        } else if (field == 2 && wt == 0) {
            if (!pb_read_varint(buf, len, &pos, &f->log_id)) return false;
        } else if (field == 3 && wt == 0) {
            if (!pb_read_varint(buf, len, &pos, &v)) return false;
            f->service = (int32_t)v;
        } else if (field == 4 && wt == 0) {
            if (!pb_read_varint(buf, len, &pos, &v)) return false;
            f->method = (int32_t)v;
        } else if (field == 5 && wt == 2) {
            if (!pb_read_varint(buf, len, &pos, &blen)) return false;
            if (pos + (size_t)blen > len) return false;
            if (f->header_count < 16) {
                pb_parse_header_msg(buf + pos, (size_t)blen, &f->headers[f->header_count++]);
            }
            pos += (size_t)blen;
        } else if (field == 8 && wt == 2) {
            if (!pb_read_varint(buf, len, &pos, &blen)) return false;
            if (pos + (size_t)blen > len) return false;
            f->payload = buf + pos;
            f->payload_len = (size_t)blen;
            pos += (size_t)blen;
        } else {
            if (!pb_skip_field(buf, len, &pos, wt)) return false;
        }
    }
    return true;
}

static bool pb_write_varint(uint8_t *buf, size_t cap, size_t *pos, uint64_t value)
{
    do {
        if (*pos >= cap) return false;
        uint8_t byte = (uint8_t)(value & 0x7F);
        value >>= 7;
        if (value) byte |= 0x80;
        buf[(*pos)++] = byte;
    } while (value);
    return true;
}

static bool pb_write_tag(uint8_t *buf, size_t cap, size_t *pos, uint32_t field, uint8_t wt)
{
    return pb_write_varint(buf, cap, pos, ((uint64_t)field << 3) | wt);
}

static bool pb_write_bytes(uint8_t *buf, size_t cap, size_t *pos, uint32_t field, const uint8_t *data, size_t len)
{
    if (!pb_write_tag(buf, cap, pos, field, 2)) return false;
    if (!pb_write_varint(buf, cap, pos, len)) return false;
    if (*pos + len > cap) return false;
    memcpy(buf + *pos, data, len);
    *pos += len;
    return true;
}

static bool pb_write_string(uint8_t *buf, size_t cap, size_t *pos, uint32_t field, const char *s)
{
    return pb_write_bytes(buf, cap, pos, field, (const uint8_t *)s, strlen(s));
}

static const char *frame_header_value(const ws_frame_t *f, const char *key)
{
    for (size_t i = 0; i < f->header_count; i++) {
        if (strcmp(f->headers[i].key, key) == 0) {
            return f->headers[i].value;
        }
    }
    return NULL;
}

static int ws_send_frame(const ws_frame_t *f, const uint8_t *payload, size_t payload_len, int timeout_ms)
{
    uint8_t out[2048];
    size_t pos = 0;
    if (!pb_write_tag(out, sizeof(out), &pos, 1, 0) || !pb_write_varint(out, sizeof(out), &pos, f->seq_id)) return -1;
    if (!pb_write_tag(out, sizeof(out), &pos, 2, 0) || !pb_write_varint(out, sizeof(out), &pos, f->log_id)) return -1;
    if (!pb_write_tag(out, sizeof(out), &pos, 3, 0) || !pb_write_varint(out, sizeof(out), &pos, (uint32_t)f->service)) return -1;
    if (!pb_write_tag(out, sizeof(out), &pos, 4, 0) || !pb_write_varint(out, sizeof(out), &pos, (uint32_t)f->method)) return -1;

    for (size_t i = 0; i < f->header_count; i++) {
        uint8_t hb[256];
        size_t hlen = 0;
        if (!pb_write_string(hb, sizeof(hb), &hlen, f->headers[i].key)) return -1;
        if (!pb_write_string(hb, sizeof(hb), &hlen, f->headers[i].value)) return -1;
        if (!pb_write_bytes(out, sizeof(out), &pos, 5, hb, hlen)) return -1;
    }
    if (payload && payload_len > 0) {
        if (!pb_write_bytes(out, sizeof(out), &pos, 8, payload, payload_len)) return -1;
    }
    return esp_websocket_client_send_bin(s_ws_client, (const char *)out, pos, timeout_ms);
}

/* ==================== Token Management ==================== */
static esp_err_t feishu_get_tenant_token(void)
{
    if (s_app_id[0] == '\0' || s_app_secret[0] == '\0') {
        ESP_LOGW(TAG, "No Feishu credentials configured");
        return ESP_ERR_INVALID_STATE;
    }

    int64_t now = esp_timer_get_time() / 1000000LL;
    if (s_tenant_token[0] != '\0' && s_token_expire_time > now + 300) {
        return ESP_OK;
    }

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "app_id", s_app_id);
    cJSON_AddStringToObject(body, "app_secret", s_app_secret);
    char *json_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json_str) return ESP_ERR_NO_MEM;

    http_resp_t resp = { .buf = calloc(1, 2048), .len = 0, .cap = 2048 };
    if (!resp.buf) { free(json_str); return ESP_ERR_NO_MEM; }

    esp_http_client_config_t config = {
        .url = FEISHU_AUTH_URL,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = 10000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { free(json_str); free(resp.buf); return ESP_FAIL; }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_str, strlen(json_str));

    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);
    free(json_str);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Token request HTTP failed: %s", esp_err_to_name(err));
        free(resp.buf);
        return err;
    }

    cJSON *root = cJSON_Parse(resp.buf);
    free(resp.buf);
    if (!root) { ESP_LOGE(TAG, "Failed to parse token response"); return ESP_FAIL; }

    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (!code || code->valueint != 0) {
        ESP_LOGE(TAG, "Token request failed: code=%d", code ? code->valueint : -1);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *token = cJSON_GetObjectItem(root, "tenant_access_token");
    cJSON *expire = cJSON_GetObjectItem(root, "expire");

    if (token && cJSON_IsString(token)) {
        strncpy(s_tenant_token, token->valuestring, sizeof(s_tenant_token) - 1);
        s_token_expire_time = now + (expire ? expire->valueint : 7200) - 300;
        ESP_LOGI(TAG, "Got tenant access token (expires in %ds)",
                 expire ? expire->valueint : 7200);
    }

    cJSON_Delete(root);
    return ESP_OK;
}

/* ==================== Network Health Check ==================== */
bool feishu_network_ready(void)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta) {
        return false;
    }

    esp_netif_ip_info_t ip_info = {0};
    if (esp_netif_get_ip_info(sta, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        return false;
    }

    struct addrinfo hints = {0};
    struct addrinfo *res = NULL;
    hints.ai_family = AF_UNSPEC;
    int rc = getaddrinfo("open.feishu.cn", NULL, &hints, &res);
    if (rc != 0 || !res) {
        return false;
    }

    freeaddrinfo(res);
    return true;
}

/* ==================== WebSocket Configuration ==================== */
static esp_err_t feishu_pull_ws_config(void)
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "AppID", s_app_id);
    cJSON_AddStringToObject(body, "AppSecret", s_app_secret);
    char *json_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json_str) return ESP_ERR_NO_MEM;

    http_resp_t resp = { .buf = calloc(1, 4096), .len = 0, .cap = 4096 };
    if (!resp.buf) {
        free(json_str);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t config = {
        .url = FEISHU_WS_CONFIG_URL,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(json_str);
        free(resp.buf);
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "locale", "zh");
    esp_http_client_set_post_field(client, json_str, strlen(json_str));
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(json_str);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "WS config request failed: err=%s http=%d", esp_err_to_name(err), status);
        free(resp.buf);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(resp.buf);
    free(resp.buf);
    if (!root) return ESP_FAIL;

    cJSON *code = cJSON_GetObjectItem(root, "code");
    cJSON *data = cJSON_GetObjectItem(root, "data");
    cJSON *url = data ? cJSON_GetObjectItem(data, "URL") : NULL;
    cJSON *ccfg = data ? cJSON_GetObjectItem(data, "ClientConfig") : NULL;
    if (!code || code->valueint != 0 || !url || !cJSON_IsString(url)) {
        ESP_LOGE(TAG, "Invalid WS config response");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    strncpy(s_ws_url, url->valuestring, sizeof(s_ws_url) - 1);
    
    /* Parse service_id from URL */
    const char *q = strchr(s_ws_url, '?');
    if (q) {
        const char *sid = strstr(q, "service_id=");
        if (sid) {
            sid += 11;
            char buf[24] = {0};
            size_t i = 0;
            while (i < sizeof(buf) - 1 && sid[i] && sid[i] != '&') {
                buf[i] = sid[i];
                i++;
            }
            s_ws_service_id = atoi(buf);
        }
    }

    if (ccfg) {
        cJSON *pi = cJSON_GetObjectItem(ccfg, "PingInterval");
        if (pi && cJSON_IsNumber(pi)) s_ws_ping_interval_s = pi->valueint;
    }
    
    cJSON_Delete(root);
    ESP_LOGI(TAG, "WS config ready: service_id=%d ping=%ds", s_ws_service_id, s_ws_ping_interval_s);
    return ESP_OK;
}

/* ==================== Message Processing ==================== */
static void process_ws_event_payload(const char *payload, size_t payload_len)
{
    if (!payload || payload_len == 0) {
        return;
    }

    cJSON *root = cJSON_Parse(payload);
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse event payload");
        return;
    }

    cJSON *header = cJSON_GetObjectItem(root, "header");
    cJSON *event = cJSON_GetObjectItem(root, "event");
    if (!header || !event) {
        ESP_LOGI(TAG, "Feishu WS: no header/event in payload");
        cJSON_Delete(root);
        return;
    }

    cJSON *event_type = cJSON_GetObjectItem(header, "event_type");
    const char *ev_type_str = (event_type && cJSON_IsString(event_type)) ? event_type->valuestring : "(null)";
    
    if (!cJSON_IsString(event_type) || strcmp(event_type->valuestring, "im.message.receive_v1") != 0) {
        cJSON_Delete(root);
        return;
    }

    /* Deduplication */
    cJSON *event_id_node = cJSON_GetObjectItem(header, "event_id");
    const char *event_id = (event_id_node && cJSON_IsString(event_id_node)) ? event_id_node->valuestring : NULL;
    if (event_id && event_id[0]) {
        if (dedup_check_and_record(event_id)) {
            ESP_LOGD(TAG, "Duplicate event_id=%.48s, skipping", event_id);
            cJSON_Delete(root);
            return;
        }
    }

    /* Extract message */
    cJSON *message = cJSON_GetObjectItem(event, "message");
    if (!message) {
        ESP_LOGW(TAG, "Feishu WS: event missing message");
        cJSON_Delete(root);
        return;
    }

    cJSON *message_id_j = cJSON_GetObjectItem(message, "message_id");
    cJSON *chat_id_j = cJSON_GetObjectItem(message, "chat_id");
    cJSON *chat_type_j = cJSON_GetObjectItem(message, "chat_type");
    cJSON *content_j = cJSON_GetObjectItem(message, "content");

    if (!chat_id_j || !cJSON_IsString(chat_id_j) || !content_j || !cJSON_IsString(content_j)) {
        cJSON_Delete(root);
        return;
    }

    const char *chat_id = chat_id_j->valuestring;
    const char *chat_type = cJSON_IsString(chat_type_j) ? chat_type_j->valuestring : "p2p";
    const char *content_str = content_j->valuestring;

    /* Parse content JSON to extract text */
    cJSON *content_obj = cJSON_Parse(content_str);
    if (!content_obj) {
        ESP_LOGW(TAG, "Failed to parse message content JSON");
        cJSON_Delete(root);
        return;
    }

    cJSON *text_j = cJSON_GetObjectItem(content_obj, "text");
    if (!text_j || !cJSON_IsString(text_j)) {
        cJSON_Delete(content_obj);
        cJSON_Delete(root);
        return;
    }

    const char *text = text_j->valuestring;

    /* Strip @bot mention prefix if present */
    const char *cleaned = text;
    if (strncmp(cleaned, "@_user_1 ", 9) == 0) {
        cleaned += 9;
    }
    while (*cleaned == ' ' || *cleaned == '\n') cleaned++;

    if (cleaned[0] == '\0') {
        cJSON_Delete(content_obj);
        cJSON_Delete(root);
        return;
    }

    /* Get sender info for app message filtering */
    const char *sender_id = "";
    cJSON *sender = cJSON_GetObjectItem(event, "sender");
    bool is_app_message = false;
    if (sender) {
        cJSON *sender_type = cJSON_GetObjectItem(sender, "sender_type");
        if (sender_type && cJSON_IsString(sender_type)) {
            if (strcmp(sender_type->valuestring, "app") == 0) {
                is_app_message = true;
                ESP_LOGI(TAG, "Feishu WS: filtered (sender_type=app), not pushing to agent");
            }
        }
        cJSON *sender_id_obj = cJSON_GetObjectItem(sender, "sender_id");
        if (sender_id_obj) {
            cJSON *open_id = cJSON_GetObjectItem(sender_id_obj, "open_id");
            if (open_id && cJSON_IsString(open_id)) {
                sender_id = open_id->valuestring;
            }
        }
    }

    /* Skip app messages */
    if (is_app_message) {
        cJSON_Delete(content_obj);
        cJSON_Delete(root);
        return;
    }

    ESP_LOGI(TAG, "Message from %s in %s(%s): %.60s%s",
             sender_id, chat_id, chat_type, cleaned,
             strlen(cleaned) > 60 ? "..." : "");

    /* Determine route_id */
    const char *route_id = chat_id;
    if (strcmp(chat_type, "p2p") == 0 && sender_id[0]) {
        route_id = sender_id;
    }

    /* Push to inbound message bus */
    mimi_msg_t msg = {0};
    strncpy(msg.channel, MIMI_CHAN_FEISHU, sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, route_id, sizeof(msg.chat_id) - 1);
    msg.content = strdup(cleaned);

    if (msg.content) {
        if (message_bus_push_inbound(&msg) != ESP_OK) {
            ESP_LOGW(TAG, "Inbound queue full, dropping feishu message");
            free(msg.content);
        }
    }

    cJSON_Delete(content_obj);
    cJSON_Delete(root);
}

static void feishu_event_worker_task(void *arg)
{
    (void)arg;
    feishu_event_job_t job = {0};
    while (1) {
        if (xQueueReceive(s_ws_event_queue, &job, portMAX_DELAY) == pdTRUE) {
            process_ws_event_payload(job.payload, job.payload_len);
            free(job.payload);
        }
    }
}

/* ==================== WebSocket Event Handler ==================== */
static void feishu_handle_ws_frame(const uint8_t *buf, size_t len)
{
    ws_frame_t frame = {0};
    if (!pb_parse_frame(buf, len, &frame)) {
        ESP_LOGW(TAG, "WS frame parse failed");
        return;
    }

    const char *type = frame_header_value(&frame, "type");
    if (frame.method == 0) {
        if (type && strcmp(type, "pong") == 0 && frame.payload && frame.payload_len > 0) {
            cJSON *cfg = cJSON_ParseWithLength((const char *)frame.payload, frame.payload_len);
            if (cfg) {
                cJSON *pi = cJSON_GetObjectItem(cfg, "PingInterval");
                if (pi && cJSON_IsNumber(pi)) s_ws_ping_interval_s = pi->valueint;
                cJSON_Delete(cfg);
            }
        }
        return;
    }
    if (!type || strcmp(type, "event") != 0) return;
    if (!frame.payload || frame.payload_len == 0) return;

    int code = 200;
    process_ws_event_payload((const char *)frame.payload, frame.payload_len);

    char ack[32];
    int ack_len = snprintf(ack, sizeof(ack), "{\"code\":%d}", code);
    ws_frame_t resp = frame;
    ws_send_frame(&resp, (const uint8_t *)ack, (size_t)ack_len, 1000);
}

static void feishu_ws_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    esp_websocket_event_data_t *e = (esp_websocket_event_data_t *)event_data;
    static uint8_t *rx_buf = NULL;
    static size_t rx_cap = 0;
    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        s_ws_connected = true;
        ESP_LOGI(TAG, "Feishu WS connected");
    } else if (event_id == WEBSOCKET_EVENT_DISCONNECTED) {
        s_ws_connected = false;
        ESP_LOGW(TAG, "Feishu WS disconnected");
    } else if (event_id == WEBSOCKET_EVENT_DATA) {
        if (e->op_code != WS_TRANSPORT_OPCODES_BINARY) return;
        size_t need = e->payload_offset + e->data_len;
        if (e->payload_offset == 0) {
            if (rx_buf) free(rx_buf);
            rx_cap = (e->payload_len > need) ? e->payload_len : need;
            rx_buf = malloc(rx_cap);
            if (!rx_buf) return;
        } else if (!rx_buf || need > rx_cap) {
            return;
        }
        memcpy(rx_buf + e->payload_offset, e->data_ptr, e->data_len);
        if (need >= e->payload_len) {
            feishu_handle_ws_frame(rx_buf, e->payload_len);
            free(rx_buf);
            rx_buf = NULL;
            rx_cap = 0;
        }
    }
}

/* ==================== Token Refresh Task ==================== */
static void feishu_token_refresh_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000)); /* Check every 1 min */
        if (s_app_id[0] && s_app_secret[0]) {
            feishu_get_tenant_token();
        }
    }
}

/* ==================== WebSocket Task ==================== */
static void feishu_ws_task(void *arg)
{
    (void)arg;
    while (1) {
        if (s_app_id[0] == '\0' || s_app_secret[0] == '\0') {
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        if (!feishu_network_ready()) {
            ESP_LOGW(TAG, "Network not ready, waiting...");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        if (feishu_pull_ws_config() != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        esp_websocket_client_config_t ws_cfg = {
            .uri = s_ws_url,
            .buffer_size = 2048,
            .task_stack = MIMI_FEISHU_POLL_STACK,
            .reconnect_timeout_ms = FEISHU_WS_RECONNECT_MS,
            .network_timeout_ms = 10000,
            .disable_auto_reconnect = false,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };

        s_ws_client = esp_websocket_client_init(&ws_cfg);
        if (!s_ws_client) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        esp_websocket_register_events(s_ws_client, WEBSOCKET_EVENT_ANY, feishu_ws_event_handler, NULL);
        esp_websocket_client_start(s_ws_client);

        int64_t last_ping = 0;
        while (s_ws_client) {
            if (s_ws_connected) {
                int64_t now = esp_timer_get_time() / 1000;
                if (now - last_ping >= s_ws_ping_interval_s * 1000) {
                    ws_frame_t ping = {0};
                    ping.seq_id = 0;
                    ping.log_id = 0;
                    ping.service = s_ws_service_id;
                    ping.method = 0;
                    ping.header_count = 1;
                    strncpy(ping.headers[0].key, "type", sizeof(ping.headers[0].key) - 1);
                    strncpy(ping.headers[0].value, "ping", sizeof(ping.headers[0].value) - 1);
                    ws_send_frame(&ping, NULL, 0, 1000);
                    last_ping = now;
                }
            }
            if (!esp_websocket_client_is_connected(s_ws_client) && !s_ws_connected) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
        }

        esp_websocket_client_stop(s_ws_client);
        esp_websocket_client_destroy(s_ws_client);
        s_ws_client = NULL;
        s_ws_connected = false;
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

/* ==================== Rate Limiting ==================== */
static void feishu_check_rate_limit(void)
{
    int64_t now = esp_timer_get_time() / 1000LL;
    if (now - s_send_window_start >= FEISHU_QPS_WINDOW_MS) {
        s_send_count = 0;
        s_send_window_start = now;
    }
}

static bool feishu_should_rate_limit(void)
{
    int64_t now = esp_timer_get_time() / 1000LL;
    if (now - s_send_window_start >= FEISHU_QPS_WINDOW_MS) {
        return false;
    }
    return s_send_count >= FEISHU_SEND_QPS_LIMIT;
}

/* ==================== Public API ==================== */
esp_err_t feishu_bot_init(void)
{
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_FEISHU, NVS_READONLY, &nvs) == ESP_OK) {
        char tmp_id[64] = {0};
        char tmp_secret[128] = {0};
        size_t len_id = sizeof(tmp_id);
        size_t len_secret = sizeof(tmp_secret);

        if (nvs_get_str(nvs, MIMI_NVS_KEY_FEISHU_APP_ID, tmp_id, &len_id) == ESP_OK && tmp_id[0]) {
            strncpy(s_app_id, tmp_id, sizeof(s_app_id) - 1);
        }
        if (nvs_get_str(nvs, MIMI_NVS_KEY_FEISHU_APP_SECRET, tmp_secret, &len_secret) == ESP_OK && tmp_secret[0]) {
            strncpy(s_app_secret, tmp_secret, sizeof(s_app_secret) - 1);
        }
        nvs_close(nvs);
    }

    if (s_app_id[0] && s_app_secret[0]) {
        ESP_LOGI(TAG, "Feishu credentials loaded (app_id=%.8s...)", s_app_id);
    } else {
        ESP_LOGW(TAG, "No Feishu credentials. Use CLI: set_feishu_creds <APP_ID> <APP_SECRET>");
    }

    return ESP_OK;
}

esp_err_t feishu_bot_start(void)
{
    if (s_app_id[0] == '\0' || s_app_secret[0] == '\0') {
        ESP_LOGW(TAG, "Feishu not configured, skipping WebSocket start");
        return ESP_OK;
    }

    /* Create event queue */
    s_ws_event_queue = xQueueCreate(FEISHU_EVENT_QUEUE_LEN, sizeof(feishu_event_job_t));
    if (!s_ws_event_queue) {
        ESP_LOGE(TAG, "Failed to create Feishu event queue");
        return ESP_ERR_NO_MEM;
    }

    /* Create event worker task */
    BaseType_t worker_ok = xTaskCreatePinnedToCore(
        feishu_event_worker_task,
        "feishu_evt",
        MIMI_FEISHU_POLL_STACK,
        NULL,
        MIMI_FEISHU_POLL_PRIO,
        NULL,
        MIMI_FEISHU_POLL_CORE);
    if (worker_ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Feishu event worker task");
        return ESP_FAIL;
    }

    /* Create token refresh task */
    BaseType_t token_ok = xTaskCreatePinnedToCore(
        feishu_token_refresh_task,
        "feishu_tok",
        MIMI_FEISHU_POLL_STACK,
        NULL,
        MIMI_FEISHU_POLL_PRIO,
        NULL,
        MIMI_FEISHU_POLL_CORE);
    if (token_ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Feishu token refresh task");
        return ESP_FAIL;
    }

    /* Create WebSocket task */
    if (s_ws_task) {
        ESP_LOGW(TAG, "Feishu WebSocket task already running");
        return ESP_OK;
    }
    BaseType_t ws_ok = xTaskCreatePinnedToCore(
        feishu_ws_task,
        "feishu_ws",
        MIMI_FEISHU_POLL_STACK,
        NULL,
        MIMI_FEISHU_POLL_PRIO,
        &s_ws_task,
        MIMI_FEISHU_POLL_CORE);
    if (ws_ok != pdPASS) {
        s_ws_task = NULL;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Feishu WebSocket mode enabled (async task model)");
    return ESP_OK;
}

esp_err_t feishu_send_message(const char *chat_id, const char *text)
{
    if (s_app_id[0] == '\0' || s_app_secret[0] == '\0') {
        ESP_LOGW(TAG, "Cannot send: no credentials configured");
        return ESP_ERR_INVALID_STATE;
    }

    feishu_check_rate_limit();
    if (feishu_should_rate_limit()) {
        int64_t now = esp_timer_get_time() / 1000LL;
        int wait_ms = FEISHU_QPS_WINDOW_MS - (int)(now - s_send_window_start);
        ESP_LOGW(TAG, "Rate limited, waiting %dms", wait_ms);
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }

    if (feishu_get_tenant_token() != ESP_OK) {
        return ESP_FAIL;
    }

    /* Determine receive_id_type based on ID prefix */
    const char *id_type = "chat_id";
    if (strncmp(chat_id, "ou_", 3) == 0) {
        id_type = "open_id";
    }

    char url[256];
    snprintf(url, sizeof(url), "%s?receive_id_type=%s", FEISHU_SEND_MSG_URL, id_type);

    size_t text_len = strlen(text);
    size_t offset = 0;
    int all_ok = 1;

    while (offset < text_len) {
        size_t chunk = text_len - offset;
        if (chunk > MIMI_FEISHU_MAX_MSG_LEN) {
            chunk = MIMI_FEISHU_MAX_MSG_LEN;
        }

        char *segment = malloc(chunk + 1);
        if (!segment) return ESP_ERR_NO_MEM;
        memcpy(segment, text + offset, chunk);
        segment[chunk] = '\0';

        /* Build content JSON: {"text":"..."} */
        cJSON *content = cJSON_CreateObject();
        cJSON_AddStringToObject(content, "text", segment);
        char *content_str = cJSON_PrintUnformatted(content);
        cJSON_Delete(content);
        free(segment);

        if (!content_str) { offset += chunk; all_ok = 0; continue; }

        /* Build message body */
        cJSON *body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "receive_id", chat_id);
        cJSON_AddStringToObject(body, "msg_type", "text");
        cJSON_AddStringToObject(body, "content", content_str);
        free(content_str);

        char *json_str = cJSON_PrintUnformatted(body);
        cJSON_Delete(body);

        if (json_str) {
            http_resp_t resp = { .buf = calloc(1, 4096), .len = 0, .cap = 4096 };
            if (!resp.buf) {
                free(json_str);
                offset += chunk;
                all_ok = 0;
                continue;
            }

            esp_http_client_config_t config = {
                .url = url,
                .event_handler = http_event_handler,
                .user_data = &resp,
                .timeout_ms = 15000,
                .buffer_size = 2048,
                .buffer_size_tx = 2048,
                .crt_bundle_attach = esp_crt_bundle_attach,
            };
            esp_http_client_handle_t client = esp_http_client_init(&config);
            if (client) {
                char auth_header[600];
                snprintf(auth_header, sizeof(auth_header), "Bearer %s", s_tenant_token);
                esp_http_client_set_header(client, "Authorization", auth_header);
                esp_http_client_set_header(client, "Content-Type", "application/json; charset=utf-8");
                esp_http_client_set_method(client, HTTP_METHOD_POST);
                esp_http_client_set_post_field(client, json_str, strlen(json_str));

                esp_err_t err = esp_http_client_perform(client);
                if (err == ESP_OK) {
                    cJSON *root = cJSON_Parse(resp.buf);
                    if (root) {
                        cJSON *code = cJSON_GetObjectItem(root, "code");
                        if (code && code->valueint != 0) {
                            cJSON *msg = cJSON_GetObjectItem(root, "msg");
                            ESP_LOGW(TAG, "Send failed: code=%d, msg=%s",
                                     code->valueint, msg ? msg->valuestring : "unknown");
                            all_ok = 0;
                        } else {
                            ESP_LOGI(TAG, "Sent to %s (%d bytes)", chat_id, (int)chunk);
                            s_send_count++;
                        }
                        cJSON_Delete(root);
                    }
                } else {
                    ESP_LOGE(TAG, "Failed to send message chunk: %s", esp_err_to_name(err));
                    all_ok = 0;
                }
                esp_http_client_cleanup(client);
            }
            free(json_str);
            free(resp.buf);
        }

        offset += chunk;
    }

    return all_ok ? ESP_OK : ESP_FAIL;
}

esp_err_t feishu_reply_message(const char *message_id, const char *text)
{
    if (s_app_id[0] == '\0' || s_app_secret[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    if (feishu_get_tenant_token() != ESP_OK) {
        return ESP_FAIL;
    }

    char url[256];
    snprintf(url, sizeof(url), FEISHU_REPLY_MSG_URL, message_id);

    cJSON *content = cJSON_CreateObject();
    cJSON_AddStringToObject(content, "text", text);
    char *content_str = cJSON_PrintUnformatted(content);
    cJSON_Delete(content);
    if (!content_str) return ESP_ERR_NO_MEM;

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "msg_type", "text");
    cJSON_AddStringToObject(body, "content", content_str);
    free(content_str);

    char *json_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json_str) return ESP_ERR_NO_MEM;

    http_resp_t resp = { .buf = calloc(1, 4096), .len = 0, .cap = 4096 };
    if (!resp.buf) { free(json_str); return ESP_ERR_NO_MEM; }

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { free(json_str); free(resp.buf); return ESP_FAIL; }

    char auth_header[600];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", s_tenant_token);
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_post_field(client, json_str, strlen(json_str));

    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);
    free(json_str);

    esp_err_t ret = ESP_FAIL;
    if (err == ESP_OK && resp.buf) {
        cJSON *root = cJSON_Parse(resp.buf);
        if (root) {
            cJSON *code = cJSON_GetObjectItem(root, "code");
            if (code && code->valueint == 0) {
                ret = ESP_OK;
            } else {
                cJSON *msg = cJSON_GetObjectItem(root, "msg");
                ESP_LOGW(TAG, "Reply failed: code=%d, msg=%s",
                         code ? code->valueint : -1, msg ? msg->valuestring : "unknown");
            }
            cJSON_Delete(root);
        }
    }

    free(resp.buf);
    return ret;
}

esp_err_t feishu_set_credentials(const char *app_id, const char *app_secret)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_FEISHU, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_FEISHU_APP_ID, app_id));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_FEISHU_APP_SECRET, app_secret));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    strncpy(s_app_id, app_id, sizeof(s_app_id) - 1);
    strncpy(s_app_secret, app_secret, sizeof(s_app_secret) - 1);

    /* Clear cached token to force re-auth */
    s_tenant_token[0] = '\0';
    s_token_expire_time = 0;

    ESP_LOGI(TAG, "Feishu credentials saved");
    return ESP_OK;
}

/* ==================== Test Helper Functions ==================== */
void feishu_parse_chat_id(const char *chat_id, char *out_type, size_t type_len,
                          char *out_id, size_t id_len)
{
    out_type[0] = '\0';
    out_id[0] = '\0';
    const char *colon = strchr(chat_id, ':');
    if (!colon || colon == chat_id) {
        return;
    }
    size_t pre_len = (size_t)(colon - chat_id);
    if (pre_len >= type_len) {
        return;
    }
    memcpy(out_type, chat_id, pre_len);
    out_type[pre_len] = '\0';
    const char *id = colon + 1;
    strncpy(out_id, id, id_len - 1);
    out_id[id_len - 1] = '\0';
}
