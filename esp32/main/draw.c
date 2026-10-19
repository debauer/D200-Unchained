#include "draw.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "config.h"
#include "drawdyn.h"
#include "esp_log.h"

static const char *TAG = "draw";

/* "rrggbb" -> r, g, b; false when `item` is not a colour. */
static bool parse_color(const cJSON *item, uint8_t rgb[3])
{
    if (!cJSON_IsString(item) || strlen(item->valuestring) != 6) return false;
    char *end;
    unsigned long v = strtoul(item->valuestring, &end, 16);
    if (*end) return false;
    rgb[0] = v >> 16;
    rgb[1] = v >> 8;
    rgb[2] = v;
    return true;
}

rgb_t draw_color(const cJSON *config, const cJSON *page, const cJSON *key, const char *field, rgb_t fallback)
{
    uint8_t c[3] = {fallback.r, fallback.g, fallback.b};
    if (strcmp(field, "color") == 0 && config) parse_color(cJSON_GetObjectItem(config, "background"), c);
    if (page) parse_color(cJSON_GetObjectItem(page, field), c);
    if (key) parse_color(cJSON_GetObjectItem(key, field), c);
    return (rgb_t){c[0], c[1], c[2]};
}

static void log_refusal(const char *what, uint8_t type, const uint8_t *reply, size_t n)
{
    ESP_LOGE(TAG, "deckd answered %s with 0x%02x: %.*s", what, type, (int)(type == DECKD_ERROR ? n : 0), reply);
}

/* The stored image of `key` if it is there and has exactly the size of the window: its pixels, free() them. */
static uint8_t *load_image(const cJSON *key, const device_key_t *window)
{
    const cJSON *name = key ? cJSON_GetObjectItem(key, "image") : NULL;
    if (!cJSON_IsString(name) || strchr(name->valuestring, '/')) return NULL;
    char path[sizeof IMAGE_DIR + 260];
    snprintf(path, sizeof path, IMAGE_DIR "/%s", name->valuestring);
    size_t size = (size_t)window->width * window->height * 2;
    struct stat st;
    FILE *f = stat(path, &st) == 0 && st.st_size == (off_t)size ? fopen(path, "rb") : NULL;
    uint8_t *pixels = f ? malloc(size) : NULL;
    if (pixels && fread(pixels, 1, size, f) != size) {
        free(pixels);
        pixels = NULL;
    }
    if (f) fclose(f);
    return pixels;
}

esp_err_t draw_page(deckd_t *d, const device_state_t *state, uint32_t keys, bool clear)
{
    cJSON *config = config_parse();
    uint8_t background[3] = {0, 0, 0}, color[3];
    if (config) parse_color(cJSON_GetObjectItem(config, "background"), background);
    const cJSON *page = config ? config_find_page(config, state->page) : NULL;
    uint8_t type, reply[DECKD_REPLY_MAX];
    size_t n;
    esp_err_t err = ESP_OK;
    if (clear) err = deckd_request(d, DECKD_CLEAR, background, 3, &type, reply, sizeof reply, &n);
    if (err == ESP_OK && clear && type != DECKD_OK) log_refusal("clear", type, reply, n);
    int images = 0, drawn = 0;
    for (int i = 0; err == ESP_OK && i < state->key_count; i++) {
        const device_key_t *w = &state->keys[i];
        if (w->index >= 32 || !(keys & (1u << w->index))) continue;
        drawn++;
        const cJSON *key = page ? config_find_key(page, w->index) : NULL;
        if (drawdyn_is_dynamic(config, key, state->page, w)) { /* placeholders or the clock: drawn here, not uploaded */
            err = drawdyn_key(d, config, page, state->page, key, w);
            continue;
        }
        uint8_t *pixels = load_image(key, w);
        if (pixels) {
            err = deckd_set_key_image(d, w->index, w->width, w->height, pixels, &type, reply, sizeof reply, &n);
            free(pixels);
            if (err == ESP_OK && type != DECKD_OK) log_refusal("key image", type, reply, n);
            images++;
            continue;
        }
        rgb_t c = draw_color(config, page, key, "color", (rgb_t){0, 0, 0});
        color[0] = c.r;
        color[1] = c.g;
        color[2] = c.b;
        uint8_t payload[5] = {w->index, w->index >> 8, color[0], color[1], color[2]};
        err = deckd_request(d, DECKD_FILL_KEY, payload, sizeof payload, &type, reply, sizeof reply, &n);
        if (err == ESP_OK && type != DECKD_OK) log_refusal("fill key", type, reply, n);
    }
    ESP_LOGI(TAG, "drew page \"%s\": %d keys, %d with an image", state->page, drawn, images);
    cJSON_Delete(config);
    return err;
}

/* The parts of a key that decide how it is drawn. */
static bool same_text(const cJSON *a, const cJSON *b, const char *name)
{
    const cJSON *x = a ? cJSON_GetObjectItem(a, name) : NULL, *y = b ? cJSON_GetObjectItem(b, name) : NULL;
    if (!cJSON_IsString(x) || !cJSON_IsString(y)) return !cJSON_IsString(x) && !cJSON_IsString(y);
    return strcmp(x->valuestring, y->valuestring) == 0;
}

static bool same_number(const cJSON *a, const cJSON *b, const char *name)
{
    const cJSON *x = a ? cJSON_GetObjectItem(a, name) : NULL, *y = b ? cJSON_GetObjectItem(b, name) : NULL;
    if (!cJSON_IsNumber(x) || !cJSON_IsNumber(y)) return !cJSON_IsNumber(x) && !cJSON_IsNumber(y);
    return x->valuedouble == y->valuedouble;
}

static bool same_look(const cJSON *a, const cJSON *b)
{
    static const char *const texts[] = {"color", "image", "label", "label_color", "label_pos"};
    for (size_t i = 0; i < sizeof texts / sizeof texts[0]; i++) {
        if (!same_text(a, b, texts[i])) return false;
    }
    const cJSON *ra = a ? cJSON_GetObjectItem(a, "rules") : NULL, *rb = b ? cJSON_GetObjectItem(b, "rules") : NULL;
    bool rules = (!ra && !rb) || (ra && rb && cJSON_Compare(ra, rb, true)) || (cJSON_GetArraySize(ra) == 0 && cJSON_GetArraySize(rb) == 0);
    return rules && same_number(a, b, "label_size");
}

void draw_request_changes(const cJSON *before, const cJSON *after, const char *page_id)
{
    const cJSON *pa = before ? config_find_page(before, page_id) : NULL, *pb = config_find_page(after, page_id);
    bool shared = pa && pb && same_text(before, after, "background") && same_text(pa, pb, "color") &&
                  same_text(pa, pb, "label_color") && same_text(pa, pb, "accent_color");
    const cJSON *va = before ? cJSON_GetObjectItem(before, "variables") : NULL, *vb = cJSON_GetObjectItem(after, "variables");
    if (!shared || (va || vb ? !va || !vb || !cJSON_Compare(va, vb, true) : false)) { /* variables decide the placeholders */
        device_request_redraw(-1);
        return;
    }
    for (int i = 0; i < 32; i++) { /* keys of both: one that vanished or appeared counts as changed */
        if (!same_look(config_find_key(pa, i), config_find_key(pb, i))) device_request_redraw(i);
    }
}
