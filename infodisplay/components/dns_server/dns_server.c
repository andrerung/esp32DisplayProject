#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_log.h"
#include "dns_server.h"

static const char *TAG = "dns_server";

#define DNS_PORT     53
#define DNS_BUF_SIZE 512

static volatile bool s_running = false;
static TaskHandle_t  s_task    = NULL;

/* Respond to every DNS query with an A record pointing to the AP IP 192.168.4.1 */
static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in saddr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
        ESP_LOGE(TAG, "bind() failed on port %d", DNS_PORT);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    /* 2-second receive timeout so we can check s_running */
    struct timeval tv = {.tv_sec = 2, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ESP_LOGI(TAG, "Listening on UDP port %d", DNS_PORT);

    uint8_t buf[DNS_BUF_SIZE];
    struct sockaddr_in caddr;
    socklen_t clen = sizeof(caddr);

    while (s_running) {
        /* Leave 16 bytes of headroom for appended answer record */
        int n = recvfrom(sock, buf, sizeof(buf) - 16, 0,
                         (struct sockaddr *)&caddr, &clen);
        if (n < 12) continue; /* too short or timeout */

        /*
         * Build a minimal DNS response in-place:
         *   - Copy the full query (header + question) into buf
         *   - Modify flags to mark it as a response
         *   - Set ANCOUNT = 1
         *   - Append one A record answer (compression pointer back to question name)
         */
        buf[2] = 0x81; /* QR=1 Response, RD=1 */
        buf[3] = 0x80; /* RA=1, RCODE=0 No Error */
        buf[6] = 0x00; /* ANCOUNT MSB */
        buf[7] = 0x01; /* ANCOUNT LSB = 1 */

        int pos = n;
        /* Name: pointer to byte offset 12 (start of question name) */
        buf[pos++] = 0xC0; buf[pos++] = 0x0C;
        /* TYPE A */
        buf[pos++] = 0x00; buf[pos++] = 0x01;
        /* CLASS IN */
        buf[pos++] = 0x00; buf[pos++] = 0x01;
        /* TTL: 10 seconds */
        buf[pos++] = 0x00; buf[pos++] = 0x00;
        buf[pos++] = 0x00; buf[pos++] = 0x0A;
        /* RDLENGTH: 4 */
        buf[pos++] = 0x00; buf[pos++] = 0x04;
        /* RDATA: 192.168.4.1 */
        buf[pos++] = 192; buf[pos++] = 168; buf[pos++] = 4; buf[pos++] = 1;

        sendto(sock, buf, pos, 0, (struct sockaddr *)&caddr, clen);
    }

    close(sock);
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t dns_server_start(void)
{
    if (s_running) return ESP_OK;
    s_running = true;
    if (xTaskCreate(dns_task, "dns_srv", 4096, NULL, 5, &s_task) != pdPASS) {
        s_running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void dns_server_stop(void)
{
    s_running = false;
}
