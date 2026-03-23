/**
 * Client cert, taken from client.crt
 * Client key, taken from client.key
 * CA cert, taken from ca.crt
 *
 * To embed it in the app binary, the files are named
 * in the component.mk COMPONENT_EMBED_TXTFILES variable.
 * All embedded buffers are NULL terminated.
 */
#include "embed.h"

extern uint8_t d_broker_crt_der_start[] asm("_binary_broker_crt_der_start");
extern uint8_t d_broker_crt_der_end[]   asm("_binary_broker_crt_der_end");
extern uint8_t d_client_crt_der_start[] asm("_binary_client_crt_der_start");
extern uint8_t d_client_crt_der_end[]   asm("_binary_client_crt_der_end");
extern uint8_t d_client_key_der_start[] asm("_binary_client_key_der_start");
extern uint8_t d_client_key_der_end[]   asm("_binary_client_key_der_end");
extern uint8_t d_ca_crt_der_start[]     asm("_binary_ca_crt_der_start");
extern uint8_t d_ca_crt_der_end[]       asm("_binary_ca_crt_der_end");

uint8_t *broker_crt_der_start = d_broker_crt_der_start;
uint8_t *broker_crt_der_end   = d_broker_crt_der_end;
uint8_t *client_crt_der_start = d_client_crt_der_start;
uint8_t *client_crt_der_end   = d_client_crt_der_end;
uint8_t *client_key_der_start = d_client_key_der_start;
uint8_t *client_key_der_end   = d_client_key_der_end;
uint8_t *ca_crt_der_start     = d_ca_crt_der_start;
uint8_t *ca_crt_der_end     = d_ca_crt_der_end;
