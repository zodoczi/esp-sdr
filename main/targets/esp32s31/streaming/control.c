#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "stream.h"
#include <math.h>
#include <string.h>
static SemaphoreHandle_t control_mutex;
static TaskHandle_t sender_task;
static portMUX_TYPE stats_mux = portMUX_INITIALIZER_UNLOCKED;
static uint64_t dropped[3];
void stream_record_drop(uint64_t samples, stream_drop_t reason) {
    taskENTER_CRITICAL(&stats_mux);
    dropped[reason] += samples;
    taskEXIT_CRITICAL(&stats_mux);
}
void stream_wake_sender(void) {
    TaskHandle_t task = __atomic_load_n(&sender_task, __ATOMIC_ACQUIRE);
    if (task)
        xTaskNotifyGive(task);
}
static cJSON *error(const char *message) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "error", message);
    return r;
}
static cJSON *status(void) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "product", "ESP-SDR RX");
    cJSON_AddNumberToObject(r, "protocol", STREAM_VERSION);
    cJSON_AddStringToObject(r, "transport",
                            stream_load(&stream_owner) == STREAM_USB        ? "usb"
                            : stream_load(&stream_owner) == STREAM_ETHERNET ? "ethernet"
                                                                            : "idle");
    cJSON_AddNumberToObject(r, "epoch", stream_load(&stream_epoch));
    cJSON_AddNumberToObject(r, "frequency", receiver_config.frequency_hz);
    cJSON_AddNumberToObject(r, "rate", receiver_config.rate);
    cJSON_AddNumberToObject(r, "dc_correction", receiver_config.dc_correction);
    cJSON_AddNumberToObject(r, "dc_before_i", receiver_dc_before[0]);
    cJSON_AddNumberToObject(r, "dc_before_q", receiver_dc_before[1]);
    cJSON_AddNumberToObject(r, "dc_after_i", receiver_dc_after[0]);
    cJSON_AddNumberToObject(r, "dc_after_q", receiver_dc_after[1]);
    cJSON_AddNumberToObject(r, "dc_steps", receiver_dc_steps);
    cJSON_AddNumberToObject(r, "dc_j00", receiver_dc_jacobian[0]);
    cJSON_AddNumberToObject(r, "dc_j01", receiver_dc_jacobian[1]);
    cJSON_AddNumberToObject(r, "dc_j10", receiver_dc_jacobian[2]);
    cJSON_AddNumberToObject(r, "dc_j11", receiver_dc_jacobian[3]);
    cJSON_AddNumberToObject(r, "gain", receiver_config.gain);
    cJSON_AddNumberToObject(r, "agc", receiver_config.agc);
    cJSON_AddNumberToObject(r, "gain_max", receiver_gain_max);
    cJSON_AddNumberToObject(r, "bandwidth", receiver_config.bandwidth);
    cJSON_AddNumberToObject(r, "effective_bandwidth", receiver_bandwidth(&receiver_config));
    uint64_t snapshot[3];
    taskENTER_CRITICAL(&stats_mux);
    memcpy(snapshot, dropped, sizeof(snapshot));
    taskEXIT_CRITICAL(&stats_mux);
    cJSON_AddNumberToObject(r, "capture_drops", snapshot[0] + snapshot[1]);
    cJSON_AddNumberToObject(r, "capture_overruns", snapshot[0]);
    cJSON_AddNumberToObject(r, "ring_drops", snapshot[1]);
    cJSON_AddNumberToObject(r, "transport_drops", snapshot[2]);
    cJSON_AddNumberToObject(r, "time_us", esp_timer_get_time());
    cJSON_AddBoolToObject(r, "usb_connected", usb_ready());
    cJSON_AddStringToObject(r, "ip", stream_ip);
    cJSON_AddNumberToObject(r, "ethernet_mbps", stream_load(&stream_link_mbps));
    cJSON_AddNumberToObject(r, "dma_free", heap_caps_get_free_size(MALLOC_CAP_DMA));
    return r;
}
static bool number(const cJSON *obj, const char *key, uint32_t *value, uint32_t low, uint32_t high,
                   uint32_t step) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!v)
        return true;
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < low ||
        v->valuedouble > high)
        return false;
    uint32_t n = (uint32_t)v->valuedouble;
    if (n != v->valuedouble || (n - low) % step)
        return false;
    *value = n;
    return true;
}
void control_init(void) {
    control_mutex = xSemaphoreCreateMutex();
    assert(control_mutex);
}
cJSON *control_request(const cJSON *request, stream_owner_t caller, const char *peer) {
    if (!cJSON_IsObject(request))
        return error("Expected a JSON object");
    const cJSON *op = cJSON_GetObjectItemCaseSensitive(request, "op");
    if (!cJSON_IsString(op))
        return error("Missing operation");
    xSemaphoreTake(control_mutex, portMAX_DELAY);
    cJSON *r = NULL;
    receiver_config_t cfg = receiver_config;
    stream_owner_t owner = stream_load(&stream_owner);
    if (!strcmp(op->valuestring, "status"))
        r = status();
    else if (strcmp(op->valuestring, "configure") && strcmp(op->valuestring, "start") &&
             strcmp(op->valuestring, "stop"))
        r = error("Unknown operation");
    else if (!number(request, "frequency", &cfg.frequency_hz, 2150000000u, 2800000000u, 1000000) ||
             !number(request, "dc_correction", &cfg.dc_correction, 0, 1, 1) ||
             !number(request, "agc", &cfg.agc, 0, 1, 1) ||
             !number(request, "gain", &cfg.gain, 0, receiver_gain_max, 1) ||
             !number(request, "rate", &cfg.rate, 1000000, 40000000, 1) ||
             !number(request, "bandwidth", &cfg.bandwidth, 0, 54000000, 1000000) ||
             (cfg.bandwidth && cfg.bandwidth < 13000000) ||
             (cfg.rate != 4000000 && cfg.rate != 8000000 && cfg.rate != 16000000 &&
              cfg.rate != 20000000 && cfg.rate != 40000000))
        r = error("Unsupported receiver setting");
    else {
        if (!strcmp(op->valuestring, "start")) {
            if (owner != STREAM_IDLE && owner != caller)
                r = error("Receiver is owned by the other transport");
            else if (caller == STREAM_ETHERNET) {
                uint32_t port = 0;
                if (!number(request, "port", &port, 1024, 65535, 1) || !port ||
                    !network_destination(peer, port))
                    r = error("Invalid UDP destination");
            } else if (!usb_ready())
                r = error("USB is not connected at high speed");
            if (!r)
                owner = caller;
        } else if (!strcmp(op->valuestring, "stop")) {
            if (owner != STREAM_IDLE && owner != caller)
                r = error("Receiver is owned by the other transport");
            else
                owner = STREAM_IDLE;
        }
        if (!r && owner == STREAM_USB && cfg.rate > 20000000)
            r = error("USB supports at most 20 MS/s; use Ethernet for 40 MS/s");
        if (!r) {
            if (receiver_apply(&cfg, owner) != ESP_OK)
                r = error("Receiver configuration failed");
            else
                r = status();
        }
    }
    xSemaphoreGive(control_mutex);
    return r;
}
void stream_sender(void *arg) {
    (void)arg;
    __atomic_store_n(&sender_task, xTaskGetCurrentTaskHandle(), __ATOMIC_RELEASE);
    for (;;) {
        uint32_t t = stream_load(&stream_tail), h = stream_load(&stream_head);
        if (t == h) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        stream_packet_t *p = &stream_ring[t % STREAM_RING];
        unsigned count = 1;
        bool ok = true;
        unsigned owner = stream_load(&stream_owner);
        if (p->epoch == stream_load(&stream_epoch) && owner != STREAM_IDLE) {
            if (owner == STREAM_ETHERNET)
                ok = network_send(p);
            else {
                unsigned batch = STREAM_USB_BATCH;
                if (batch > STREAM_RING - (t % STREAM_RING))
                    batch = STREAM_RING - (t % STREAM_RING);
                if (h - t < batch && stream_ring[(h - 1) % STREAM_RING].epoch == p->epoch) {
                    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2));
                    continue;
                }
                while (count < STREAM_USB_BATCH && count < h - t &&
                       (t % STREAM_RING) + count < STREAM_RING && p[count].epoch == p->epoch)
                    count++;
                ok = usb_send_and_retire(p, count * sizeof(*p), t + count);
            }
            if (!ok)
                stream_record_drop(count * STREAM_SAMPLES, STREAM_DROP_TRANSPORT);
        }
        /* USB already releases after copying; this also covers failures
         * before the copy, discarded epochs, and the Ethernet path. */
        stream_store(&stream_tail, t + count);
        if (!ok)
            vTaskDelay(1);
    }
}
