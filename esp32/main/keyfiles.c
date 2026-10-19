#include "keyfiles.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "config.h"
#include "device.h"
#include "esp_log.h"
#include "web.h"

static const char *TAG = "keyfiles";
#define CHUNK 2048

typedef struct {
    char page[33];
    unsigned index;
    char what[10]; /* image, icon, alticon, press */
} target_t;

void keyfiles_init(void)
{
    mkdir(IMAGE_DIR, 0775);
}

/* /api/keys/<n>/<what> or /api/pages/<page>/keys/<n>/<what> */
static bool parse_target(const char *uri, target_t *t)
{
    strcpy(t->page, "root");
    const char *p = uri;
    if (strncmp(p, "/api/pages/", 11) == 0) {
        p += 11;
        const char *slash = strchr(p, '/');
        if (!slash || slash == p || (size_t)(slash - p) >= sizeof t->page) return false;
        memcpy(t->page, p, (size_t)(slash - p));
        t->page[slash - p] = 0;
        p = slash;
        if (strncmp(p, "/keys/", 6) != 0) return false;
        p += 6;
    } else if (strncmp(p, "/api/keys/", 10) == 0) {
        p += 10;
    } else {
        return false;
    }
    char *end;
    unsigned long index = strtoul(p, &end, 10);
    if (end == p || *end != '/') return false;
    t->index = (unsigned)index;
    const char *what = end + 1;
    if (strlen(what) >= sizeof t->what) return false;
    strcpy(t->what, what);
    return true;
}

static esp_err_t send_json_tree(httpd_req_t *req, const cJSON *tree)
{
    char *text = cJSON_PrintUnformatted(tree);
    if (!text) return web_json_error(req, "500 Internal Server Error", "out of memory");
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, text);
    free(text);
    return err;
}

static void file_path(char *out, size_t size, const target_t *t, const char *kind)
{
    char name[96];
    config_image_name(name, size < sizeof name ? size : sizeof name, t->page, t->index, kind);
    snprintf(out, size, IMAGE_DIR "/%s", name);
}

static const char *kind_of(const char *what)
{
    if (strcmp(what, "image") == 0) return "";
    if (strcmp(what, "icon") == 0) return "-icon";
    if (strcmp(what, "alticon") == 0) return "-alt";
    return NULL;
}

static esp_err_t get_file(httpd_req_t *req, const target_t *t, const char *kind)
{
    char path[sizeof IMAGE_DIR + 128];
    file_path(path, sizeof path, t, kind);
    FILE *f = fopen(path, "rb");
    if (!f) {
        char message[48];
        snprintf(message, sizeof message, "key %u has no %s", t->index, t->what);
        return web_json_error(req, "404 Not Found", message);
    }
    httpd_resp_set_type(req, "application/octet-stream");
    char *buf = malloc(CHUNK);
    esp_err_t err = buf ? ESP_OK : ESP_ERR_NO_MEM;
    size_t n;
    while (err == ESP_OK && (n = fread(buf, 1, CHUNK, f)) > 0) err = httpd_resp_send_chunk(req, buf, n);
    if (err == ESP_OK) httpd_resp_send_chunk(req, NULL, 0);
    free(buf);
    fclose(f);
    return err;
}

/* Receives the body into a temporary file and moves it to `path`; `expected` bytes exactly. */
/* True when stored; otherwise the error answer is already sent. */
static bool put_file(httpd_req_t *req, const char *path, size_t expected, const device_key_t *w)
{
    if (req->content_len != expected) {
        char message[96];
        snprintf(message, sizeof message, "key %u needs %u bytes (%ux%u RGB565), got %u", w->index, (unsigned)expected,
                 w->width, w->height, (unsigned)req->content_len);
        web_json_error(req, "400 Bad Request", message);
        return false;
    }
    char tmp[] = IMAGE_DIR "/.upload";
    FILE *f = fopen(tmp, "wb");
    char *buf = malloc(CHUNK);
    if (!f || !buf) {
        if (f) fclose(f);
        free(buf);
        web_json_error(req, "500 Internal Server Error", "cannot store the image");
        return false;
    }
    size_t got = 0;
    bool ok = true;
    while (ok && got < expected) {
        int n = httpd_req_recv(req, buf, expected - got < CHUNK ? expected - got : CHUNK);
        if (n <= 0) ok = false;
        else if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) ok = false;
        else got += (size_t)n;
    }
    free(buf);
    ok = (fclose(f) == 0) && ok && rename(tmp, path) == 0;
    if (!ok) {
        unlink(tmp);
        ESP_LOGE(TAG, "upload of %s failed after %u bytes", path, (unsigned)got);
        web_json_error(req, "500 Internal Server Error", "cannot store the image");
        return false;
    }
    return true;
}

/* Sets or removes "image" of the key in the configuration; answers with the key entry. */
static esp_err_t update_key(httpd_req_t *req, const target_t *t, const char *image_name)
{
    cJSON *config = config_parse();
    cJSON *page = config ? config_find_page(config, t->page) : NULL;
    cJSON *key = page ? config_ensure_key(page, t->index) : NULL;
    if (!key) {
        cJSON_Delete(config);
        return web_json_error(req, "500 Internal Server Error", "cannot update the configuration");
    }
    cJSON_DeleteItemFromObject(key, "image");
    if (image_name) cJSON_AddStringToObject(key, "image", image_name);
    bool saved = config_save(config);
    esp_err_t err = saved ? send_json_tree(req, key) : web_json_error(req, "500 Internal Server Error", "cannot store the config");
    cJSON_Delete(config);
    if (saved) device_request_redraw((int)t->index);
    return err;
}

esp_err_t keyfiles_handler(httpd_req_t *req)
{
    target_t t;
    if (!parse_target(req->uri, &t)) return web_json_error(req, "404 Not Found", "not found");
    device_key_t window;
    if (!device_key_window(t.index, &window)) return web_json_error(req, "404 Not Found", "unknown key");
    cJSON *config = config_parse();
    bool page_known = config && config_find_page(config, t.page);
    cJSON_Delete(config);
    if (!page_known) return web_json_error(req, "404 Not Found", "unknown page");
    const char *kind = kind_of(t.what);
    if (!kind) return web_json_error(req, "404 Not Found", "not found");

    if (req->method == HTTP_GET) return get_file(req, &t, kind);
    if (!web_mutation_allowed(req)) return web_json_error(req, "403 Forbidden", "missing X-Deck-Request header");

    char path[sizeof IMAGE_DIR + 128];
    file_path(path, sizeof path, &t, kind);
    bool is_image = kind[0] == 0;
    if (req->method == HTTP_PUT) {
        if (!put_file(req, path, (size_t)window.width * window.height * 2, &window)) return ESP_OK;
        if (!is_image) {
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_sendstr(req, "{\"stored\":true}");
        }
        char name[96];
        config_image_name(name, sizeof name, t.page, t.index, "");
        return update_key(req, &t, name);
    }
    if (req->method == HTTP_DELETE) {
        unlink(path);
        if (is_image) return update_key(req, &t, NULL);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"stored\":false}");
    }
    return web_json_error(req, "405 Method Not Allowed", "method not allowed");
}
