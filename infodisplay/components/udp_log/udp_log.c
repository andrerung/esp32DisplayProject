#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "esp_log.h"
#include "udp_log.h"

static int s_sock = -1;
static struct sockaddr_in s_dest;
static vprintf_like_t s_orig_vprintf;

static int udp_vprintf(const char *fmt, va_list args)
{
    char buf[256];
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    if (s_sock >= 0 && n > 0) {
        sendto(s_sock, buf, (size_t)n, 0,
               (struct sockaddr *)&s_dest, sizeof(s_dest));
    }
    /* Always return the formatted length so esp_log accounting stays correct */
    return n;
}

esp_err_t udp_log_init(const char *host, uint16_t port)
{
    if (!host || host[0] == '\0' || port == 0) return ESP_ERR_INVALID_ARG;

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) return ESP_FAIL;

    memset(&s_dest, 0, sizeof(s_dest));
    s_dest.sin_family = AF_INET;
    s_dest.sin_port   = htons(port);
    if (inet_pton(AF_INET, host, &s_dest.sin_addr) != 1) {
        close(s_sock);
        s_sock = -1;
        return ESP_ERR_INVALID_ARG;
    }

    s_orig_vprintf = esp_log_set_vprintf(udp_vprintf);
    return ESP_OK;
}

void udp_log_stop(void)
{
    if (s_orig_vprintf) {
        esp_log_set_vprintf(s_orig_vprintf);
        s_orig_vprintf = NULL;
    }
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
}
