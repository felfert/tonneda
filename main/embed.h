#pragma once
#include "stdint.h"
#include "stddef.h"

extern uint8_t *broker_crt_der_start;
extern uint8_t *broker_crt_der_end;
extern uint8_t *client_crt_der_start;
extern uint8_t *client_crt_der_end;
extern uint8_t *client_key_der_start;
extern uint8_t *client_key_der_end;
extern uint8_t *ca_crt_der_start;
extern uint8_t *ca_crt_der_end;

#define broker_crt_der_bytes (size_t)(broker_crt_der_end - broker_crt_der_start)
#define client_crt_der_bytes (size_t)(client_crt_der_end - client_crt_der_start)
#define client_key_der_bytes (size_t)(client_key_der_end - client_key_der_start)
#define ca_crt_der_bytes     (size_t)(ca_crt_der_end     - ca_crt_der_start)
