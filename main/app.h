
extern EventGroupHandle_t appState;

extern const EventBits_t WIFI_CONNECTED;
extern const EventBits_t MQTT_CONNECTED;
extern const EventBits_t OTA_REQUIRED;
extern const EventBits_t OTA_DONE;
extern const EventBits_t NTP_SYNCED;
extern const EventBits_t SYSLOG_QUEUED;

extern const char* TAG;
extern esp_mqtt_client_handle_t client;
extern std::string identity;
