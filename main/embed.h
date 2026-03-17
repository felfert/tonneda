#pragma once
#include "stdint.h"
#include "stddef.h"

extern uint8_t *broker_crt_start;
extern uint8_t *broker_crt_end;
extern uint8_t *client_crt_start;
extern uint8_t *client_crt_end;
extern uint8_t *client_key_start;
extern uint8_t *client_key_end;
extern uint8_t *ca_crt_start;
extern uint8_t *ca_crt_end;

#define broker_crt_bytes (size_t)(broker_crt_end - broker_crt_start)
#define client_crt_bytes (size_t)(client_crt_end - client_crt_start)
#define client_key_bytes (size_t)(client_key_end - client_key_start)
#define ca_crt_bytes     (size_t)(ca_crt_end     - ca_crt_start)
