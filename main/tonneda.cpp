#include <string>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <deque>

#ifdef __cplusplus
extern "C" {
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "driver/gpio.h"
#include "rom/ets_sys.h"

#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
#include "esp_log.h"

#ifdef __cplusplus
}
#endif

#include "app.h"
#include "tonneda.h"
#include "syslog.h"

static QueueHandle_t gpio_evt_queue = nullptr;

static bool last_tonneda[3] = {false,};

static void publish_tonne(uint8_t index, bool tonneda, bool force = false) {
    if (force || last_tonneda[index] != tonneda) {
        char topic[50];
        snprintf(topic, sizeof(topic), "esp32/tonne%d/%s", index, tonneda ? "true" : "false");
        esp_mqtt_client_publish(client, topic, identity.c_str(), 0, 0, 0);
        last_tonneda[index] = tonneda;
    }
}

static void publish_dist(uint8_t index, float distance, float average) {
    char topic[50];
    snprintf(topic, sizeof(topic), "esp32/dist%d/%.2f %.2f", index, distance, average);
    esp_mqtt_client_publish(client, topic, identity.c_str(), 0, 0, 0);
}

bool calibrating = false;

typedef struct {
    uint16_t min_good;   // Minimal good distance
    uint16_t max_good;   // Maximal good distance
    uint16_t led_enable; // Distance less than this enables LEDs for 5min
} calibration_t;

static calibration_t calibration[3] = {0, };
;
static gpio_num_t *triggers = new gpio_num_t[3] {
    GPIO_NUM_32, GPIO_NUM_17, GPIO_NUM_18,
};

typedef struct {
    gpio_num_t red;
    gpio_num_t green;
} led_t;

static led_t *leds = new led_t[3] {
    {.red = GPIO_NUM_25, .green = GPIO_NUM_26},
    {.red = GPIO_NUM_22, .green = GPIO_NUM_23},
    {.red = GPIO_NUM_19, .green = GPIO_NUM_21},
};


typedef struct {
    gpio_num_t port;
    uint8_t index;
    uint64_t start; // usecs rising edge
} isr_echo_t;

static isr_echo_t *echos = new isr_echo_t[3] {
    {.port = GPIO_NUM_35, .index = 0, .start = 0 },
    {.port = GPIO_NUM_34, .index = 1, .start = 0 },
    {.port = GPIO_NUM_33, .index = 2, .start = 0 },
};

static int nrSensors = 3;

/**
 * Switches LEDS on/off
 */
static void switchleds(uint8_t index, uint8_t onoff) {
    if (index < nrSensors) {
        gpio_set_level(leds[index].red, onoff&1 ? 1 : 0);
        gpio_set_level(leds[index].green, onoff&2 ? 1 : 0);
    }
}

/**
 * Trigger task
 * Triggers all US-Sensors in a loop
 */
static void trigger_task(void * pvParameter) {
    while (true) {
        vTaskDelay(500 / portTICK_PERIOD_MS);
        for (int i = 0; i < nrSensors; i++) {
            gpio_set_level(triggers[i], 1);
            ets_delay_us(10);
            gpio_set_level(triggers[i], 0);
        }
    }
}

// Time to switch off LEDs
static time_t led_offtime[3] = {0, 0, 0};
// Flag: LEDs show distance
static bool led_active[3] = {false, false, false};
// Flag: LEDs are blinking
static bool led_blinking[3] = {false, false, false};

/**
 * LED off task
 * Disables LEDs when led_offtime has been reached.
 */
static void ledoff_task(void * pvParameter) {
    while (true) {
        vTaskDelay(5000 / portTICK_PERIOD_MS);
        time_t now;
        time(&now);
        for (uint8_t i = 0; i <  nrSensors; i++) {
            if (led_offtime[i] != 0 && now > led_offtime[i]) {
                ESP_LOGI(TAG, "switch off leds %d", i);
                led_offtime[i] = 0;
                led_active[i] = false;
                switchleds(i, 0);
            }
        }
    }
}

static bool blink_on = false;

static void blink_task(void * pvParameter) {
    while (true) {
        vTaskDelay(200 / portTICK_PERIOD_MS);
        for (uint8_t i = 0; i < nrSensors; i++) {
            if (led_blinking[i]) {
                switchleds(i, blink_on ? 3 : 0);
            }
        }
        blink_on = !blink_on;
    }
}

bool is_blinking(uint8_t index) {
    if (index < nrSensors) {
        return led_blinking[index];
    }
    return false;
}

void start_blinking(uint8_t index) {
    if (index < nrSensors) {
        led_blinking[index] = true;
    }
}

void stop_blinking(uint8_t index) {
    if (index < nrSensors) {
        led_blinking[index] = false;
        switchleds(index, 0);
    }
}

static std::deque<float> history;

/**
 * Echo task
 * Publishes changes queued by echo_isr to MQTT.
 */
static void echo_task(void * pvParameter) {
    uint64_t msg;
    while (true) {
        if (xQueueReceive(gpio_evt_queue, &msg, portMAX_DELAY)) {
            uint64_t duration = msg & 0xfffffffffffffffc;
            uint8_t index = msg & 3;
            float distance = duration / 58.00; // (340m/s * 1us) / 2

            // calculate average
            history.push_front(distance);
            if (history.size() > 5) {
                history.pop_back();
            }
            float average = 0.0;
            for (float f : history) {
                average += f;
            }
            average /= history.size();
            ESP_LOGD(TAG, "%d %8.2f %8.2f", index, distance, average);
            calibration_t cal = calibration[index];
            if (distance < cal.led_enable && led_offtime[index] == 0) {
                ESP_LOGI(TAG, "switch on leds %d", index);
                led_offtime[index] = time(nullptr) + 30;
                led_active[index] = true;
            }
            if (calibrating) {
                publish_dist(index, distance, average);
            }
            bool tonneda = average >  cal.min_good && average < cal.max_good;
            if (led_active[index]) {
                switchleds(index, tonneda ? 2 : 1);
            }
            // publish to MQTT only if MQTT is actually connected.
            if (xEventGroupWaitBits(appState, MQTT_CONNECTED, pdFALSE, pdFALSE, 0) & MQTT_CONNECTED) {
                publish_tonne(index, tonneda);
            }
        }
    }
}

/**
 * The gpio ISR
 * Measures echo impulse duration and enqueues an event with the result.
 */
static void IRAM_ATTR echo_isr(void *arg) {
    uint64_t now = esp_timer_get_time();
    isr_echo_t *echo = (isr_echo_t *)arg;

    // rising edge: just store ticks
    if (gpio_get_level(echo->port)) {
        echo->start = now;
        return;
    }

    // falling edge: calculate duration im usecs an enqueue result
    uint64_t duration = now - echo->start; // this ignores overflows
    if (duration > 100) {
        // Lowest 3bits are the index
        duration = (duration & 0xfffffffffffffffc) | (echo->index & 3);
        xQueueSendFromISR(gpio_evt_queue, &duration, nullptr);
    }
}

void setup_tonneda() {

    // configure input pins (US-Sensor's Echo pins)
    gpio_config_t echo_conf = {
        .pin_bit_mask = 0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    for (int i = 0; i < nrSensors; i++) {
        echo_conf.pin_bit_mask |= (1ULL << echos[i].port);
    }
    ESP_ERROR_CHECK(gpio_config(&echo_conf));

    // configure output pins (US-Sensor's Trigger)
    gpio_config_t trigger_conf = {
        .pin_bit_mask = 0,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    for (int i = 0; i < nrSensors; i++) {
        trigger_conf.pin_bit_mask |= (1ULL << triggers[i]);
    }
    ESP_ERROR_CHECK(gpio_config(&trigger_conf));

    // configure output pins (LEDS)
    gpio_config_t led_conf = {
        .pin_bit_mask = 0,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    for (int i = 0; i < nrSensors; i++) {
        led_conf.pin_bit_mask |= (1ULL << leds[i].green);
        led_conf.pin_bit_mask |= (1ULL << leds[i].red);
    }
    ESP_ERROR_CHECK(gpio_config(&led_conf));

    // Create queue for echo events and task for delivering event to mqtt
    gpio_evt_queue = xQueueCreate(30, sizeof(uint64_t));
    xTaskCreate(&echo_task, "echo_task", 2048, nullptr, 3, nullptr);

    /// install gpio isr service
    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_IRAM));

    // hook isr handlers for specific gpio pin
    for (int i = 0; i < nrSensors; i++) {
        ESP_ERROR_CHECK(gpio_isr_handler_add(echos[i].port, echo_isr, (void *)&(echos[i])));
    }

    // Start trigger_task which pulls all triggers in a loop
    xTaskCreate(&trigger_task, "trigger_task", 2048, nullptr, 3 | portPRIVILEGE_BIT, nullptr);
    xTaskCreate(&ledoff_task, "ledoff_task", 2048, nullptr, tskIDLE_PRIORITY | portPRIVILEGE_BIT, nullptr);
    xTaskCreate(&blink_task, "blink_task", 2048, nullptr, tskIDLE_PRIORITY | portPRIVILEGE_BIT, nullptr);
}

static void set_calibration_defaults() {
    ESP_LOGI(TAG, "Setting calibration defaults");
    for (uint8_t i = 0; i < 3; i++) {
        calibration[i].led_enable = 80;
        calibration[i].min_good = 120;
        calibration[i].max_good = 150;
    }
}

/*
 * Attempt to read calibration from NVS
 * Called from app_main() during startup. If no values are in NVS,
 * Initialize values in set_calibration_defaults() instead.
 */
void io_calibration(bool readmode) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("tonneda", NVS_READWRITE, &nvs_handle);
    if (ESP_OK == err) {
        size_t sz = sizeof(calibration);
        if (readmode) {
            err = nvs_get_blob(nvs_handle, "cal", &calibration, &sz);
        } else {
            err = nvs_set_blob(nvs_handle, "cal", &calibration, sizeof(calibration));
        }
        switch (err) {
            case ESP_OK:
                break;
            case ESP_ERR_NVS_NOT_FOUND:
                if (readmode) {
                    set_calibration_defaults();
                }
                break;
            default:
                ESP_LOGE(TAG, "Unable to read NVS: %s", esp_err_to_name(err));
                syslog(LOG_ERR, "Unable to read NVS: %s", esp_err_to_name(err));
                if (readmode) {
                    set_calibration_defaults();
                }
                break;
        }
        nvs_close(nvs_handle);
    } else {
        ESP_LOGE(TAG, "Unable to open NVS: %s", esp_err_to_name(err));
        syslog(LOG_ERR, "Unable to open NVS: %s", esp_err_to_name(err));
        if (readmode) {
            set_calibration_defaults();
        }
    }
}
