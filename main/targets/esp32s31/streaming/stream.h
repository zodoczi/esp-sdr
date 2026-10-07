#pragma once
#include "cJSON.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Little-endian on the wire. One datagram is one complete packet; USB uses
 * the identical packets back-to-back. No transport-specific sample framing. */
#define STREAM_VERSION 2
#define STREAM_PAYLOAD 1344
#define STREAM_SAMPLES (STREAM_PAYLOAD / 2)
#define STREAM_RING 32
#define STREAM_USB_BATCH 16
#define STREAM_USB_VID 0x303a
#define STREAM_USB_PID 0x4531

typedef struct __attribute__((packed)) {
    char magic[4];
    uint16_t version, bits;
    uint32_t epoch, rate;
    uint64_t sample, time_us;
    int8_t iq[STREAM_PAYLOAD];
} stream_packet_t;
_Static_assert(sizeof(stream_packet_t) == 1376, "wire packet size");
typedef enum { STREAM_IDLE, STREAM_ETHERNET, STREAM_USB } stream_owner_t;
typedef struct {
    uint32_t frequency_hz, rate, gain, bandwidth, dc_correction, agc;
} receiver_config_t;
/* Zero bandwidth follows the sample rate, limited by the analog filter. */
static inline uint32_t receiver_bandwidth(const receiver_config_t *config) {
    return config->bandwidth ? config->bandwidth : config->rate < 13000000 ? 13000000 : config->rate;
}
extern stream_packet_t *stream_ring;
extern uint32_t stream_head, stream_tail;
extern uint32_t stream_epoch, stream_owner;
extern receiver_config_t receiver_config;
extern unsigned receiver_gain_max;
extern float receiver_dc_before[2], receiver_dc_after[2], receiver_dc_jacobian[4];
extern unsigned receiver_dc_steps;
typedef enum { STREAM_DROP_DMA, STREAM_DROP_RING, STREAM_DROP_TRANSPORT } stream_drop_t;
void stream_record_drop(uint64_t samples, stream_drop_t reason);
extern char stream_ip[16];
extern uint32_t stream_link_mbps;
void receiver_init(void);
void receiver_dc_calibrate(bool (*measure)(float mean[2]));
void receiver_transport_lost(stream_owner_t owner);
esp_err_t receiver_apply(const receiver_config_t *config, stream_owner_t owner);
void network_init(void);
bool network_send(const stream_packet_t *packet);
bool network_destination(const char *address, unsigned port);
void usb_init(void);
bool usb_ready(void);
/* Copies into a private DMA buffer, then releases the consumed ring slots
 * before waiting for the preceding USB transfer. Called by the sender only. */
bool usb_send_and_retire(const void *data, size_t bytes, uint32_t tail_after_copy);
void control_init(void);
/* Caller owns result. Shared by HTTP and USB; all mutations are serialized. */
cJSON *control_request(const cJSON *request, stream_owner_t caller, const char *peer);
void stream_sender(void *arg);
void stream_wake_sender(void);
static inline uint32_t stream_load(const uint32_t *p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
static inline void stream_store(uint32_t *p, uint32_t v) {
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}
