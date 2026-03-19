/* Sensor for home automation, based on WiFi Connection Example using WPA2 Enterprise (EAP-TLS)
 *
 * Original Copyright (C) 2006-2016, ARM Limited, All Rights Reserved, Apache 2.0 License.
 * Additions Copyright (C) Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD, Apache 2.0 License.
 * Additions Copyright (C) 2026 Fritz Elfert, Apache 2.0 License
 *
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <string>
#include <cstring>
#include <cstdlib>
#include <deque>

#ifdef __cplusplus
extern "C" {
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_eap_client.h"
#include "esp_event.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "mqtt_client.h"
#include "esp_ota_ops.h"

#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
#include "esp_log.h"

#ifdef __cplusplus
}
#endif

#include "x509helper.h"
#include "https_ota.h"
#include "app.h"
#include "tonneda.h"

std::string identity;
esp_mqtt_client_handle_t client;

EventGroupHandle_t appState;

const EventBits_t WIFI_CONNECTED = BIT0;
const EventBits_t MQTT_CONNECTED = BIT1;
const EventBits_t OTA_REQUIRED   = BIT2;
const EventBits_t OTA_DONE       = BIT3;
const EventBits_t NTP_SYNCED     = BIT4;
const EventBits_t SYSLOG_QUEUED  = BIT5;

static const esp_app_desc_t *ad;

static esp_netif_t *sta_netif = nullptr;

const char* TAG      = "tonneda";
static const char* TAG_MEM  = "heap";
static const char* TAG_MQTT = "mqtt";

#include "embed.h"

static void wifi_event_handler(void* arg, esp_event_base_t event_base, 
        int32_t event_id, void* event_data)
{
    ESP_LOGD(TAG, "%s", __FUNCTION__);
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        wifi_event_sta_connected_t *evd = (wifi_event_sta_connected_t*)event_data;
        ESP_LOGI(TAG, "Connected to access point with BSSID " MACSTR, MAC2STR(evd->bssid));

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *evd = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "Disconnected from AP with BSSID " MACSTR ". Reason: %d", MAC2STR(evd->bssid), evd->reason);
#if 0 // FE: WIFI_REASON_BASIC_RATE_NOT_SUPPORT gone in new SDK ?
        if (evd->reason == WIFI_REASON_BASIC_RATE_NOT_SUPPORT) {
            // Switch to 802.11 bgn mode
            esp_wifi_set_protocol(ESP_IF_WIFI_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
        }
#endif
        esp_wifi_connect();
        xEventGroupClearBits(appState, WIFI_CONNECTED | NTP_SYNCED);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(appState, WIFI_CONNECTED);
    }
}

/**
 *  Get the CN from our client certificate, because we need it as identity for EAP-TLS
 */
static void init_identity(void) {
    if (identity.empty()) {
        mbedtls_x509_crt mycert;
        mbedtls_x509_crt_init(&mycert);
        if (0 > mbedtls_x509_crt_parse(&mycert, client_crt_start, client_crt_bytes)) {
            ESP_LOGE(TAG, "Unable to parse client cert");
            abort();
        }
        getOidByName(&(&mycert)->subject, "CN", identity);
        mbedtls_x509_crt_free(&mycert);
    }
}

static void wifi_setup(void)
{

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_WIFI_SSID,
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
        },
    };
    ESP_LOGI(TAG, "Connecting to WiFi SSID %s ...", wifi_config.sta.ssid);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_eap_client_set_identity((uint8_t*)identity.c_str(), identity.length()));
    ESP_ERROR_CHECK(esp_eap_client_set_ca_cert(ca_crt_start, ca_crt_bytes));
    ESP_ERROR_CHECK(esp_eap_client_set_certificate_and_key(client_crt_start, client_crt_bytes,
                client_key_start, client_key_bytes, nullptr, 0));
    ESP_ERROR_CHECK(esp_wifi_sta_enterprise_enable());
    ESP_ERROR_CHECK(esp_wifi_start());
}

/**
 * Publish current version to MQTT.
 */
static void publish_version() {
    char topic[50];
    snprintf(topic, sizeof(topic), "esp32/version/%s", ad->version);
    esp_mqtt_client_publish(client, topic, identity.c_str(), 0, 0, 0);
}

static void enable_debug(bool enable) {
    if (enable) {
        esp_log_level_set("wifi", ESP_LOG_DEBUG);
        esp_log_level_set("sensor", ESP_LOG_DEBUG);
        esp_log_level_set("OTA update", ESP_LOG_DEBUG);
        esp_log_level_set("mqtt", ESP_LOG_DEBUG);
        esp_log_level_set("heap", ESP_LOG_DEBUG);
        esp_log_level_set("HTTP_CLIENT", ESP_LOG_DEBUG);
        //esp_log_level_set("syslog", ESP_LOG_DEBUG);
        ESP_LOGI(TAG, "debug enabled");
    } else {
        esp_log_level_set("wifi", ESP_LOG_INFO);
        esp_log_level_set("sensor", ESP_LOG_INFO);
        esp_log_level_set("OTA update", ESP_LOG_INFO);
        esp_log_level_set("mqtt", ESP_LOG_INFO);
        esp_log_level_set("heap", ESP_LOG_INFO);
        esp_log_level_set("syslog", ESP_LOG_INFO);
        ESP_LOGI(TAG, "debug disabled");
    }
}

static void mqtt_action(const std::string &topic, const std::string &data) {
    bool match_exact = 0 == data.compare(identity);
    bool match_any = data.empty();
    if (match_exact && (0 == topic.compare("esp32/free"))) {
        uint32_t minfree = esp_get_minimum_free_heap_size();
        syslog(LOG_NOTICE, "Minimum free heap: %lu bytes", minfree);
        ESP_LOGI(TAG, "Minimum free heap: %lu bytes", minfree);
        return;
    }
    if (match_exact && (0 == topic.compare("esp32/nvserase"))) {
        ESP_LOGD(TAG, "Erasing non volatile storage");
        syslog(LOG_NOTICE, "Erasing non volatile storage");
        ESP_ERROR_CHECK(nvs_flash_erase());
        return;
    }
    if (match_exact && (0 == topic.compare("esp32/reboot"))) {
        ESP_LOGD(TAG, "Rebooting...");
        syslog(LOG_NOTICE, "Rebooting...");
        closelog();
        esp_restart();
        return;
    }
    if (match_exact && (0 == topic.compare("esp32/calibrate"))) {
        if (!calibrating) {
            ESP_LOGD(TAG, "Calibrating...");
            syslog(LOG_NOTICE, "Calibrating...");
            calibrating = true;
        }
        return;
    }
    if (match_exact && (0 == topic.compare("esp32/endcalibrate"))) {
        if (calibrating) {
            ESP_LOGD(TAG, "Saving calibration...");
            syslog(LOG_NOTICE, "Saving calibration...");
            io_calibration(false);
            calibrating = false;
        }
        return;
    }
    if (0 == topic.compare("esp32/update")) {
        if (match_exact || match_any) {
            xEventGroupSetBits(appState, OTA_REQUIRED);
        }
        return;
    }
    if ((0 == topic.compare("esp32/debug")) || (0 == topic.compare("esp32/nodebug"))) {
        if (match_exact || match_any) {
            enable_debug(0 == topic.compare("esp32/debug"));
        }
        return;
    }
    if (match_exact && topic.starts_with("esp32/blink?") && topic.length() == 13
            && topic[12] >= '0' && topic[12] < '3') {
        uint8_t index = std::stoi(topic.substr(12));
        const char *blinking = is_blinking(index) ? "true" : "false";
        char topic[50];
        snprintf(topic, sizeof(topic), "esp32/blinking%d/%s", index, blinking);
        esp_mqtt_client_publish(client, topic, identity.c_str(), 0, 0, 0);
        return;
    }
    if (match_exact && topic.starts_with("esp32/blink") && topic.length() == 12
            && topic[11] >= '0' && topic[11] < '3') {
        uint8_t index = std::stoi(topic.substr(11));
        calibrating = false;
        start_blinking(index);
        return;
    }
    if (match_exact && topic.starts_with("esp32/noblink") && topic.length() == 14
            && topic[13] >= '0' && topic[13] < '3') {
        uint8_t index = std::stoi(topic.substr(13));
        stop_blinking(index);
        return;
    }
}

/**
 * Callback function gets called, when time has been synced via NTP
 */
static void ntp_sync_cb(struct timeval *tv) {
    if (SNTP_SYNC_STATUS_COMPLETED == sntp_get_sync_status()) {
        xEventGroupSetBits(appState, NTP_SYNCED);
        struct tm _tm;
        time_t now;
        time(&now);
        localtime_r(&now, &_tm);
        char tbuf[50];
        strftime(tbuf, sizeof(tbuf), "%c %Z", &_tm);
        ESP_LOGI(TAG, "Time synchronized to: %s", tbuf);
        syslog(LOG_DEBUG, "Time synchronized to: %s", tbuf);
    } else {
        xEventGroupClearBits(appState, NTP_SYNCED);
    }
}

static void check_ntpserver() {
    const ip_addr_t* ntpserver = esp_sntp_getserver(0);
    if (nullptr != ntpserver) {
        ESP_LOGI(TAG, "NTP:  " IPSTR, IP2STR(ntpserver));
        esp_netif_sntp_start();
    } else {
        ESP_LOGW(TAG, "NTP:  NONE");
    }
}

static void sntp_setup() {
    esp_sntp_config_t cfg = {
        .smooth_sync = false,
        .server_from_dhcp = true,
        .wait_for_sync = true,
        .start = false,
        .sync_cb = ntp_sync_cb,
        .renew_servers_after_new_IP = true,
        .ip_event_to_renew = IP_EVENT_STA_GOT_IP,
        .index_of_first_server = 0,
        .num_of_servers = 0,
    };
    esp_netif_sntp_init(&cfg);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Connected to broker %s", CONFIG_MQTTS_URI);
            syslog(LOG_INFO, "Connected to broker %s", CONFIG_MQTTS_URI);
            msg_id = esp_mqtt_client_subscribe(client, "esp32/#", 0);
            ESP_LOGD(TAG_MQTT, "sent subscribe successful, msg_id=%d", msg_id);
            msg_id = esp_mqtt_client_publish(client, "esp32/start", identity.c_str(), 0, 0, 0);
            ESP_LOGD(TAG_MQTT, "sent publish successful, msg_id=%d", msg_id);
            publish_version();
            xEventGroupSetBits(appState, MQTT_CONNECTED);
            break;
        case MQTT_EVENT_DISCONNECTED:
            xEventGroupClearBits(appState, MQTT_CONNECTED);
            ESP_LOGD(TAG_MQTT, "MQTT_EVENT_DISCONNECTED");
            break;
        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGD(TAG_MQTT, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGD(TAG_MQTT, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGD(TAG_MQTT, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGD(TAG_MQTT, "MQTT_EVENT_DATA");
            ESP_LOGD(TAG_MQTT, "TOPIC=%.*s", event->topic_len, event->topic);
            ESP_LOGD(TAG_MQTT, "DATA=%.*s", event->data_len, event->data);
            if (0 < event->topic_len) {
                std::string topic(event->topic, event->topic_len);
                std::string data(event->data, event->data_len);
                mqtt_action(topic, data);
            }
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGW(TAG_MQTT, "MQTT_EVENT_ERROR");
            if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                ESP_LOGW(TAG_MQTT, "Last error code reported from esp-tls: 0x%x", event->error_handle->esp_tls_last_esp_err);
                ESP_LOGI(TAG_MQTT, "Last tls stack error number: 0x%x", event->error_handle->esp_tls_stack_err);
                ESP_LOGI(TAG_MQTT, "Last captured errno : %d (%s)",  event->error_handle->esp_transport_sock_errno,
                        strerror(event->error_handle->esp_transport_sock_errno));
            } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                ESP_LOGW(TAG_MQTT, "Connection refused error: 0x%x", event->error_handle->connect_return_code);
            } else {
                ESP_LOGW(TAG_MQTT, "Unknown error type: 0x%x", event->error_handle->error_type);
            }
            break;
        case MQTT_EVENT_BEFORE_CONNECT:
            ESP_LOGD(TAG_MQTT, "MQTT_EVENT_BEFORE_CONNECT");
            break;
        default:
            ESP_LOGW(TAG, "Other event id:%d", event->event_id);
            break;
    }
    ESP_LOGD(TAG_MEM, "Free memory: %lu bytes", esp_get_free_heap_size());
}

static void mqtt_setup(void)
{
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, mac));
    char idbuf[100];
    snprintf(idbuf, sizeof(idbuf), "esp32-%02x%02x%02x%02x%02x%02x", MAC2STR(mac));
    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker {
            .address = {
                .uri = CONFIG_MQTTS_URI,
            },
            .verification = {
                .certificate = (const char *)ca_crt_start,
                .certificate_len = ca_crt_bytes,
            }
        },
        .credentials = {
            .client_id = idbuf,
            .authentication = {
                .certificate = (const char *)client_crt_start,
                .certificate_len = client_crt_bytes,
                .key = (const char *)client_key_start,
                .key_len = client_key_bytes,
            },
        },
        .session = {
            .last_will = {
                .topic = "esp32/dead",
                .msg = identity.c_str(),
            },
        },
    };
    client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, mqtt_event_handler, NULL);
}


static ota_params_t ota_params = {
    .done_event = OTA_DONE,
    .cacert = (const char *)ca_crt_start,
};

/**
 * Check, if update is requested. If yes, terminate MQTT connection
 * and start OTA task.
 */
static void update_check_task(void * pvParameter) {
    while (true) {
        EventBits_t bits = xEventGroupWaitBits(appState, OTA_REQUIRED,
                pdTRUE, pdFALSE, portMAX_DELAY);
        if (bits & OTA_REQUIRED) {
            ESP_LOGI(TAG, "Firmware update requested, shutting down MQTT");
            syslog(LOG_NOTICE, "Firmware update requested, shutting down MQTT");
            syslog_flush();
            ESP_ERROR_CHECK(esp_mqtt_client_stop(client));
            ESP_LOGD(TAG_MEM, "Free memory: %lu bytes", esp_get_free_heap_size());
            ota_params.event_group = appState;
            xTaskCreate(&ota_task, "ota_task", 9216, &ota_params, 5, nullptr);
            while (true) {
                bits = xEventGroupWaitBits(appState, OTA_DONE, pdTRUE, pdFALSE, portMAX_DELAY);
                if (bits & OTA_DONE) {
                    // If we arrive here, OTA has failed prematurely (e.g. 404 or somethin similar)
                    ESP_LOGD(TAG_MEM, "Free memory: %lu bytes", esp_get_free_heap_size());

                    ESP_LOGI(TAG, "Restarting MQTT");
                    ESP_ERROR_CHECK(esp_mqtt_client_start(client));
                    break;
                }
            }
        }
    }
}

extern "C" {
    void app_main();
}

static syslog_config_t syslog_config = {
    .wifi_connected = WIFI_CONNECTED,
    .ntp_synced = NTP_SYNCED,
    .msg_queued = SYSLOG_QUEUED,
};

void app_main()
{
    ESP_ERROR_CHECK(nvs_flash_init());
    setenv("TZ", CONFIG_TZ, 1);
    tzset();
    esp_log_level_set("*", ESP_LOG_INFO);
    enable_debug(false);
    esp_log_level_set("phy_init", ESP_LOG_WARN);
    esp_log_level_set("esp_netif_handlers", ESP_LOG_WARN);
    esp_log_level_set("main_task", ESP_LOG_WARN);
    esp_log_level_set("wifi", ESP_LOG_WARN);
    ad = esp_app_get_description();
    ESP_LOGI(TAG, "Free memory: %lu bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "APP version: %s", ad->version);
    ESP_LOGI(TAG, "APP build:   %s %s", ad->date, ad->time);
    ESP_LOGI(TAG, "IDF version: %s", ad->idf_ver);
    appState = xEventGroupCreate();
    syslog_config.event_group = appState;
    init_syslog(&syslog_config);
    init_identity();
    set_syslog_hostname(identity.c_str());
    ESP_LOGI(TAG, "My CN:       %s", identity.c_str());
    sntp_setup();
    openlog(CONFIG_LWIP_LOCAL_HOSTNAME, 0, LOG_USER);
    wifi_setup();
    mqtt_setup();
    esp_netif_ip_info_t ip;
    memset(&ip, 0, sizeof(esp_netif_ip_info_t));
    io_calibration(true);
    while (true) {
        EventBits_t bits = xEventGroupWaitBits(appState, WIFI_CONNECTED,
                pdFALSE, pdFALSE, 2000 / portTICK_PERIOD_MS);
        if (bits & WIFI_CONNECTED) {
            if (ESP_OK == esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip)) {
                ESP_LOGI(TAG, "IP:   " IPSTR, IP2STR(&ip.ip));
                ESP_LOGI(TAG, "MASK: " IPSTR, IP2STR(&ip.netmask));
                ESP_LOGI(TAG, "GW:   " IPSTR, IP2STR(&ip.gw));
                check_ntpserver();
            }
            if (ESP_OK == esp_mqtt_client_start(client)) {
                xTaskCreate(&update_check_task, "update_check_task", 2048, nullptr, 2, nullptr);
                setup_tonneda();
                break;
            }
            vTaskDelay(2000 / portTICK_PERIOD_MS);
        }
    }
}
