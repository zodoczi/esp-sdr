#include "esp_eth.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "lwip/etharp.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/tcpip.h"
#include "stream.h"
#include <stdio.h>
#include <string.h>
static esp_eth_handle_t ethernet;
static esp_netif_t *net;
uint32_t stream_link_mbps;
static uint8_t stream_header[42];
static portMUX_TYPE header_mux = portMUX_INITIALIZER_UNLOCKED;
typedef struct {
    ip4_addr_t peer;
    uint8_t header[42];
    bool valid;
    SemaphoreHandle_t done;
} destination_t;
static void resolve_destination(void *arg) {
    destination_t *d = arg;
    struct netif *n = esp_netif_get_netif_impl(net);
    const ip4_addr_t *next = &d->peer;
    if (!ip4_addr_netcmp(next, netif_ip4_addr(n), netif_ip4_netmask(n))) {
        next = netif_ip4_gw(n);
    }
    struct eth_addr *mac;
    const ip4_addr_t *cached;
    if (etharp_find_addr(n, next, &mac, &cached) < 0) {
        etharp_request(n, next);
        xSemaphoreGive(d->done);
        return;
    }
    memcpy(d->header, mac->addr, 6);
    memcpy(d->header + 6, n->hwaddr, 6);
    memcpy(d->header + 26, netif_ip4_addr(n), 4);
    d->valid = true;
    xSemaphoreGive(d->done);
}
char stream_ip[16] = "0.0.0.0";
extern const uint8_t index_start[] asm("_binary_index_html_start");
extern const uint8_t index_end[] asm("_binary_index_html_end");
extern const uint8_t logo_start[] asm("_binary_logo_svg_start");
extern const uint8_t logo_end[] asm("_binary_logo_svg_end");
static esp_err_t logo(httpd_req_t *req) {
    httpd_resp_set_type(req, "image/svg+xml");
    return httpd_resp_send(req, (const char *)logo_start, logo_end - logo_start - 1);
}
static esp_err_t page(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)index_start, index_end - index_start - 1);
}
static esp_err_t api(httpd_req_t *req) {
    char body[512];
    if (req->content_len <= 0 || req->content_len >= sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request size");
        return ESP_FAIL; /* Close rather than leave an unread request body. */
    }
    size_t n = 0;
    while (n < req->content_len) {
        int k = httpd_req_recv(req, body + n, req->content_len - n);
        if (k <= 0)
            return ESP_FAIL;
        n += k;
    }
    body[n] = 0;
    cJSON *input = cJSON_ParseWithOpts(body, NULL, true);
    struct sockaddr_storage peer = {0};
    socklen_t len = sizeof(peer);
    char ip[16] = {0};
    if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&peer, &len) == 0) {
        if (peer.ss_family == AF_INET) {
            inet_ntop(AF_INET, &((struct sockaddr_in *)&peer)->sin_addr, ip, sizeof(ip));
        } else if (peer.ss_family == AF_INET6) {
            /* The HTTP listener is dual stack: IPv4 peers arrive mapped into
             * sockaddr_in6. Never reinterpret its flow-info as an IPv4 address. */
            const struct in6_addr *address = &((struct sockaddr_in6 *)&peer)->sin6_addr;
            if (IN6_IS_ADDR_V4MAPPED(address))
                inet_ntop(AF_INET, &address->s6_addr[12], ip, sizeof(ip));
        }
    }
    cJSON *out = control_request(input, STREAM_ETHERNET, ip);
    cJSON_Delete(input);
    char *text = cJSON_PrintUnformatted(out);
    bool bad = cJSON_HasObjectItem(out, "error");
    cJSON_Delete(out);
    if (!text)
        return ESP_ERR_NO_MEM;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (bad)
        httpd_resp_set_status(req, "400 Bad Request");
    esp_err_t e = httpd_resp_sendstr(req, text);
    free(text);
    return e;
}
static void got_ip(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    (void)id;
    ip_event_got_ip_t *e = data;
    snprintf(stream_ip, sizeof(stream_ip), IPSTR, IP2STR(&e->ip_info.ip));
    ESP_LOGI("stream", "Web interface: http://%s", stream_ip);
}
static void link_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    (void)data;
    if (id == ETHERNET_EVENT_CONNECTED) {
        eth_speed_t speed;
        if (esp_eth_ioctl(ethernet, ETH_CMD_G_SPEED, &speed) == ESP_OK)
            stream_store(&stream_link_mbps, speed == ETH_SPEED_1000M  ? 1000
                                            : speed == ETH_SPEED_100M ? 100
                                                                      : 10);
    } else if (id == ETHERNET_EVENT_DISCONNECTED || id == ETHERNET_EVENT_STOP) {
        stream_store(&stream_link_mbps, 0);
        receiver_transport_lost(STREAM_ETHERNET);
    }
}
bool network_destination(const char *address, unsigned port) {
    destination_t d = {0};
    if (!address || !ip4addr_aton(address, &d.peer))
        return false;
    d.done = xSemaphoreCreateBinary();
    if (!d.done)
        return false;
    if (tcpip_callback(resolve_destination, &d) == ERR_OK)
        xSemaphoreTake(d.done, portMAX_DELAY);
    vSemaphoreDelete(d.done);
    if (!d.valid)
        return false;
    uint8_t *h = d.header;
    h[12] = 8;
    h[14] = 0x45;
    unsigned ip_length = 28 + sizeof(stream_packet_t);
    h[16] = ip_length >> 8;
    h[17] = ip_length;
    h[20] = 0x40;
    h[22] = 64;
    h[23] = 17;
    memcpy(h + 30, &d.peer, 4);
    unsigned checksum = 0;
    for (unsigned i = 14; i < 34; i += 2)
        checksum += (h[i] << 8) | h[i + 1];
    while (checksum >> 16)
        checksum = (checksum & 65535) + (checksum >> 16);
    checksum = ~checksum;
    h[24] = checksum >> 8;
    h[25] = checksum;
    h[34] = 0xd5;
    h[35] = 0x51;
    h[36] = port >> 8;
    h[37] = port;
    unsigned udp_length = 8 + sizeof(stream_packet_t);
    h[38] = udp_length >> 8;
    h[39] = udp_length;
    /* IPv4 permits a zero UDP checksum. Ethernet supplies the link FCS;
     * sample positions detect loss. Avoid a second payload pass. */
    taskENTER_CRITICAL(&header_mux);
    memcpy(stream_header, h, sizeof(stream_header));
    taskEXIT_CRITICAL(&header_mux);
    return true;
}
bool network_send(const stream_packet_t *p) {
    uint8_t header[42];
    taskENTER_CRITICAL(&header_mux);
    memcpy(header, stream_header, sizeof(header));
    taskEXIT_CRITICAL(&header_mux);
    const esp_eth_buf_desc_t buffers[] = {{header, sizeof(header)}, {(uint8_t *)p, sizeof(*p)}};
    int64_t deadline = esp_timer_get_time() + 1000;
    for (;;) {
        esp_err_t error = esp_eth_transmit_ctrl_bufs(ethernet, NULL, buffers, 2);
        if (error == ESP_OK)
            return true;
        if (error != ESP_ERR_NO_MEM || esp_timer_get_time() >= deadline ||
            stream_load(&stream_owner) != STREAM_ETHERNET)
            return false;
        esp_rom_delay_us(2);
    }
}
void network_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    eth_mac_config_t mc = ETH_MAC_DEFAULT_CONFIG();
    mc.rx_task_prio = 22;
    mc.flags |= ETH_MAC_FLAG_PIN_TO_CORE;
    eth_esp32_emac_config_t ec = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    ec.smi_gpio.mdc_num = 5;
    ec.smi_gpio.mdio_num = 6;
    eth_phy_config_t pc = ETH_PHY_DEFAULT_CONFIG();
    pc.phy_addr = -1;
    pc.reset_gpio_num = 7;
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&ec, &mc);
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&pc);
    assert(mac && phy);
    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_ERROR_CHECK(esp_eth_driver_install(&config, &ethernet));
    bool autoneg = true;
    ESP_ERROR_CHECK(esp_eth_ioctl(ethernet, ETH_CMD_S_AUTONEGO, &autoneg));
    uint32_t v = 0xa001;
    esp_eth_phy_reg_rw_data_t reg = {.reg_addr = 0x1e, .reg_value_p = &v};
    ESP_ERROR_CHECK(esp_eth_ioctl(ethernet, ETH_CMD_WRITE_PHY_REG, &reg));
    reg.reg_addr = 0x1f;
    ESP_ERROR_CHECK(esp_eth_ioctl(ethernet, ETH_CMD_READ_PHY_REG, &reg));
    v |= 1u << 8;
    ESP_ERROR_CHECK(esp_eth_ioctl(ethernet, ETH_CMD_WRITE_PHY_REG, &reg));
    v = 0xa003;
    reg.reg_addr = 0x1e;
    ESP_ERROR_CHECK(esp_eth_ioctl(ethernet, ETH_CMD_WRITE_PHY_REG, &reg));
    reg.reg_addr = 0x1f;
    ESP_ERROR_CHECK(esp_eth_ioctl(ethernet, ETH_CMD_READ_PHY_REG, &reg));
    v = (v & ~255u) | (13u << 4) | 13u;
    ESP_ERROR_CHECK(esp_eth_ioctl(ethernet, ETH_CMD_WRITE_PHY_REG, &reg));
    esp_netif_config_t nc = ESP_NETIF_DEFAULT_ETH();
    net = esp_netif_new(&nc);
    assert(net);
    ESP_ERROR_CHECK(esp_netif_attach(net, esp_eth_new_netif_glue(ethernet)));
    ESP_ERROR_CHECK(esp_netif_set_hostname(net, "esp-sdr"));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, got_ip, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, link_event, NULL));
    ESP_ERROR_CHECK(esp_eth_start(ethernet));
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.stack_size = 6144;
    hc.task_priority = 18;
    hc.core_id = 0;
    httpd_handle_t server;
    ESP_ERROR_CHECK(httpd_start(&server, &hc));
    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = page};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));
    httpd_uri_t brand = {.uri = "/logo.svg", .method = HTTP_GET, .handler = logo};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &brand));
    httpd_uri_t control = {.uri = "/api/rx", .method = HTTP_POST, .handler = api};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &control));
}
