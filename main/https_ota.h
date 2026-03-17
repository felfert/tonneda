#pragma once

#include "syslog.h"

#ifdef __cplusplus
extern "C" {
#endif

extern void ota_task(void * pvParameter);
typedef struct {
    EventGroupHandle_t event_group;
    EventBits_t done_event;
    const char *cacert;
} ota_params_t;


#ifdef __cplusplus
}
#endif
