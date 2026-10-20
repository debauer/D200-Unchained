#include "web_transport.h"

#include "esp_https_server.h"
#include "lwip/sockets.h"
#include "web_settings.h"

#if !WEB_TLS
static int send_nonblocking(httpd_handle_t server, int fd, const char *buf, size_t size, int flags)
{
    return send(fd, buf, size, flags | MSG_DONTWAIT);
}
#endif

esp_err_t web_transport_start(httpd_config_t *config, httpd_handle_t *server)
{
    config->server_port = WEB_PORT;
#if WEB_TLS
    extern const uint8_t web_cert_start[], web_cert_end[], web_key_start[], web_key_end[];
    httpd_ssl_config_t tls = HTTPD_SSL_CONFIG_DEFAULT();
    tls.httpd = *config;
    tls.httpd.stack_size = 10240;
    tls.httpd.max_open_sockets = 4;
    tls.port_secure = WEB_PORT;
    tls.servercert = web_cert_start;
    tls.servercert_len = web_cert_end - web_cert_start + 1;
    tls.prvtkey_pem = web_key_start;
    tls.prvtkey_len = web_key_end - web_key_start + 1;
    tls.tls_handshake_timeout_ms = 5000;
    return httpd_ssl_start(server, &tls);
#else
    return httpd_start(server, config);
#endif
}

esp_err_t web_stream_prepare(httpd_req_t *req)
{
#if WEB_TLS
    struct timeval timeout = {.tv_sec = 0, .tv_usec = 200000};
    return setsockopt(httpd_req_to_sockfd(req), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout) == 0 ? ESP_OK : ESP_FAIL;
#else
    return httpd_sess_set_send_override(req->handle, httpd_req_to_sockfd(req), send_nonblocking);
#endif
}
