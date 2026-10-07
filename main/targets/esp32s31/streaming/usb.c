/* Vendor bulk interface: JSON requests on 01, JSON replies on 81, IQ on 82.
 * No TX endpoint and no class FIFO copies in the sample path. */
#include "device/usbd_pvt.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "stream.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include <stdio.h>
#include <string.h>
static uint8_t port;
static uint32_t opened;
static SemaphoreHandle_t complete, response_submitted;
static QueueHandle_t control_jobs;
static uint32_t generation;
static size_t response_bytes;
typedef struct { uint32_t generation, bytes; uint8_t data[512]; } control_job_t;
static uint8_t *samples;
static unsigned next_buffer;
CFG_TUSB_MEM_SECTION static uint8_t request_data[512] __attribute__((aligned(64)));
CFG_TUSB_MEM_SECTION static uint8_t response_data[1024] __attribute__((aligned(64)));
static const tusb_desc_device_t device = {.bLength = sizeof(tusb_desc_device_t),
                                          .bDescriptorType = TUSB_DESC_DEVICE,
                                          .bcdUSB = 0x0200,
                                          .bMaxPacketSize0 = 64,
                                          .idVendor = STREAM_USB_VID,
                                          .idProduct = STREAM_USB_PID,
                                          .bcdDevice = 0x0200,
                                          .iManufacturer = 1,
                                          .iProduct = 2,
                                          .iSerialNumber = 3,
                                          .bNumConfigurations = 1};
#define CONFIG_DESCRIPTOR(mps)                                                                     \
    9, TUSB_DESC_CONFIGURATION, 39, 0, 1, 1, 0, 0x80, 250, 9, TUSB_DESC_INTERFACE, 0, 0, 3, 0xff,  \
        0x53, 2, 0, 7, TUSB_DESC_ENDPOINT, 0x01, TUSB_XFER_BULK, (mps) & 255, (mps) >> 8, 0, 7,    \
        TUSB_DESC_ENDPOINT, 0x81, TUSB_XFER_BULK, (mps) & 255, (mps) >> 8, 0, 7,                   \
        TUSB_DESC_ENDPOINT, 0x82, TUSB_XFER_BULK, (mps) & 255, (mps) >> 8, 0
static const uint8_t hs[] = {CONFIG_DESCRIPTOR(512)}, fs[] = {CONFIG_DESCRIPTOR(64)};
static char serial[13];
static const char *strings[] = {(const char[]){0x09, 0x04}, "ESPARGOS", "ESP-SDR RX", serial};
static void init(void) {}
static bool deinit(void) { return true; }
static void reset(uint8_t rhport) {
    (void)rhport;
    stream_store(&opened, 0);
    stream_store(&generation, stream_load(&generation) + 1);
    if (complete)
        xSemaphoreGive(complete);
    receiver_transport_lost(STREAM_USB);
}
static uint16_t open_driver(uint8_t rhport, const tusb_desc_interface_t *itf, uint16_t maxlen) {
    if (itf->bInterfaceClass != 0xff || itf->bInterfaceSubClass != 0x53 ||
        itf->bInterfaceProtocol != 2 || maxlen < 30)
        return 0;
    const uint8_t *ptr = tu_desc_next(itf);
    for (unsigned i = 0; i < 3; i++, ptr = tu_desc_next(ptr))
        if (!usbd_edpt_open(rhport, (const tusb_desc_endpoint_t *)ptr))
            return 0;
    port = rhport;
    stream_store(&opened, 1);
    usbd_edpt_xfer(port, 0x01, request_data, sizeof(request_data), false);
    return 30;
}
static bool control_cb(uint8_t p, uint8_t stage, const tusb_control_request_t *r) {
    (void)p;
    (void)stage;
    (void)r;
    return false;
}
/* Only endpoint work runs on TinyUSB's task. Calibration can take hundreds
 * of milliseconds and must not block IQ completion callbacks/semaphore handoff. */
static void submit_response(void *context) {
    uint32_t request_generation = (uint32_t)(uintptr_t)context;
    if (request_generation == stream_load(&generation) && stream_load(&opened))
        usbd_edpt_xfer(port, 0x81, response_data, response_bytes, false);
    xSemaphoreGive(response_submitted);
}
static void control_worker(void *arg) {
    (void)arg;
    control_job_t job;
    for (;;) {
        xQueueReceive(control_jobs, &job, portMAX_DELAY);
        if (job.generation != stream_load(&generation)) continue;
        cJSON *in = NULL;
        if (job.bytes < sizeof(job.data)) {
            job.data[job.bytes] = 0;
            in = cJSON_ParseWithOpts((char *)job.data, NULL, true);
        }
        cJSON *out = control_request(in, STREAM_USB, NULL);
        cJSON_Delete(in);
        char *text = cJSON_PrintUnformatted(out);
        cJSON_Delete(out);
        size_t n = text ? strlen(text) : 0;
        if (!n || n >= sizeof(response_data)) {
            static const char error[] = "{\"error\":\"Control response unavailable\"}";
            memcpy(response_data, error, sizeof(error)-1);
            n = sizeof(error)-1;
        } else memcpy(response_data, text, n);
        free(text);
        response_bytes = n;
        if (job.generation != stream_load(&generation))
            receiver_transport_lost(STREAM_USB);
        /* Check generation and submit in USB task context so bus reset cannot
         * interleave the check with endpoint submission. Wait before reusing
         * the shared response buffer for a command from a new USB session. */
        usbd_defer_func(submit_response, (void *)(uintptr_t)job.generation, false);
        xSemaphoreTake(response_submitted, portMAX_DELAY);
    }
}
static bool transfer(uint8_t p, uint8_t ep, xfer_result_t result, uint32_t bytes) {
    if (ep == 0x82) {
        xSemaphoreGive(complete);
        return true;
    }
    if (result != XFER_RESULT_SUCCESS) return false;
    if (ep == 0x01) {
        control_job_t job = {.generation = stream_load(&generation), .bytes = bytes};
        if (bytes <= sizeof(job.data)) memcpy(job.data, request_data, bytes);
        /* OUT is rearmed only after its reply completes. On reset, a newer
         * session may replace a queued stale request while calibration ends. */
        xQueueOverwrite(control_jobs, &job);
    } else if (ep == 0x81)
        usbd_edpt_xfer(p, 0x01, request_data, sizeof(request_data), false);
    return true;
}
static const usbd_class_driver_t driver = {.name = "ESPRX",
                                           .init = init,
                                           .deinit = deinit,
                                           .reset = reset,
                                           .open = open_driver,
                                           .control_xfer_cb = control_cb,
                                           .xfer_cb = transfer};
const usbd_class_driver_t *usbd_app_driver_get_cb(uint8_t *count) {
    *count = 1;
    return &driver;
}
bool usb_ready(void) {
    return stream_load(&opened) && tud_mounted() && tud_speed_get() == TUSB_SPEED_HIGH;
}
bool usb_send_and_retire(const void *data, size_t bytes, uint32_t tail_after_copy) {
    if (!usb_ready() || bytes > STREAM_USB_BATCH * sizeof(stream_packet_t))
        return false;
    /* Fill the other internal DMA buffer while the previous transfer runs. */
    uint8_t *buffer = samples + next_buffer * STREAM_USB_BATCH * sizeof(stream_packet_t);
    memcpy(buffer, data, bytes);
    /* Capture may reuse these slots now: the staged batch is independent of
     * the ring even if the host delays completion of the preceding transfer. */
    stream_store(&stream_tail, tail_after_copy);
    /* The semaphore is the sole endpoint ownership token. A separate busy
     * flag races when this task preempts the USB completion callback. */
    if (xSemaphoreTake(complete, pdMS_TO_TICKS(100)) != pdTRUE)
        return false;
    if (!usb_ready() || !usbd_edpt_claim(port, 0x82)) {
        xSemaphoreGive(complete);
        return false;
    }
    if (!usbd_edpt_xfer(port, 0x82, buffer, bytes, false)) {
        usbd_edpt_release(port, 0x82);
        xSemaphoreGive(complete);
        return false;
    }
    next_buffer ^= 1;
    return true;
}
void usb_init(void) {
    complete = xSemaphoreCreateBinary();
    response_submitted = xSemaphoreCreateBinary();
    control_jobs = xQueueCreate(1, sizeof(control_job_t));
    assert(complete && response_submitted && control_jobs);
    BaseType_t created = xTaskCreatePinnedToCore(control_worker, "usb_control", 4096, NULL, 18, NULL, 0);
    ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    xSemaphoreGive(complete);
    samples = heap_caps_aligned_alloc(64, 2 * STREAM_USB_BATCH * sizeof(stream_packet_t),
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    assert(samples);
    ESP_LOGI("stream_usb", "Internal DMA sample buffer; remaining internal heap %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_BASE));
    snprintf(serial, sizeof(serial), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5]);
    tud_configure_dwc2_t dwc = CFG_TUD_CONFIGURE_DWC2_DEFAULT;
    dwc.bm_double_buffered = 1u << 2;
    tud_configure(0, TUD_CFGID_DWC2, &dwc);
    tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG();
    cfg.descriptor.device = &device;
    cfg.descriptor.full_speed_config = fs;
    cfg.descriptor.high_speed_config = hs;
    cfg.descriptor.string = strings;
    cfg.descriptor.string_count = 4;
    cfg.task.xCoreID = 0;
    /* IQ completion must outrank the Wi-Fi task (normally priority 23).
     * A full RTOS time slice at equal priority can exhaust the sample ring.
     * JSON/calibration already runs separately at priority 18, so this task
     * only does bounded endpoint work and can share the sender's priority. */
    cfg.task.priority = 24;
    cfg.task.size = 6144;
    TaskHandle_t wifi = xTaskGetHandle("wifi");
    ESP_LOGI("stream_usb", "USB event priority %u, Wi-Fi priority %u",
             (unsigned)cfg.task.priority, wifi ? (unsigned)uxTaskPriorityGet(wifi) : 0);
    ESP_ERROR_CHECK(tinyusb_driver_install(&cfg));
}
