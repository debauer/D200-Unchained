#include "drawdyn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "config.h"
#include "draw.h"
#include "esp_log.h"
#include "placeholders.h"
#include "render.h"
#include "wallclock.h"

static const char *TAG = "drawdyn";
#define PREFIX 7 /* SET_KEY_IMAGE: u16 index, u8 format, u16 w, u16 h */
#define CYAN ((rgb_t){0, 240, 255})
#define MAGENTA ((rgb_t){255, 43, 214})

static const char *text_of(const cJSON *key, const char *field)
{
    const cJSON *v = key ? cJSON_GetObjectItem(key, field) : NULL;
    return cJSON_IsString(v) ? v->valuestring : "";
}

static bool blank(const char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    return *s == 0;
}

static bool shows_clock(const cJSON *key, const device_key_t *w)
{
    return w->width > w->height * 3 / 2 && !text_of(key, "image")[0] && blank(text_of(key, "label"));
}

bool drawdyn_is_dynamic(const cJSON *key, const device_key_t *w)
{
    return shows_clock(key, w) || placeholders_present(text_of(key, "label"));
}

/* The unlabelled icon of a key (RGB565, exactly the window size), or NULL. Free with free(). */
static uint8_t *load_icon(const char *page_id, const device_key_t *w)
{
    char name[96], path[sizeof IMAGE_DIR + 128];
    config_image_name(name, sizeof name, page_id, w->index, "-icon");
    snprintf(path, sizeof path, IMAGE_DIR "/%s", name);
    size_t size = (size_t)w->width * w->height * 2;
    struct stat st;
    FILE *f = stat(path, &st) == 0 && (size_t)st.st_size == size ? fopen(path, "rb") : NULL;
    uint8_t *icon = f ? malloc(size) : NULL;
    if (icon && fread(icon, 1, size, f) != size) {
        free(icon);
        icon = NULL;
    }
    if (f) fclose(f);
    return icon;
}

esp_err_t drawdyn_key(deckd_t *d, const cJSON *config, const cJSON *page, const char *page_id, const cJSON *key,
                      const device_key_t *w)
{
    static bool fonts_ready;
    extern const uint8_t font_bold_start[], font_medium_start[];
    if (!fonts_ready) {
        if (!render_init(font_bold_start, font_medium_start)) {
            ESP_LOGE(TAG, "cannot read the embedded font");
            return ESP_FAIL;
        }
        fonts_ready = true;
    }
    const size_t pixels = (size_t)w->width * w->height * 2;
    uint8_t *buf = malloc(PREFIX + pixels);
    if (!buf) return ESP_ERR_NO_MEM;
    buf[0] = w->index;
    buf[1] = w->index >> 8;
    buf[2] = 0; /* RGB565 */
    buf[3] = w->width;
    buf[4] = w->width >> 8;
    buf[5] = w->height;
    buf[6] = w->height >> 8;
    rgb_t color = draw_color(config, page, key, "color", (rgb_t){0, 0, 0});
    bool ok;
    struct tm t;
    if (shows_clock(key, w)) {
        bool have = wallclock_now(&t);
        rgb_t time_color = key && cJSON_IsString(cJSON_GetObjectItem(key, "label_color"))
                               ? draw_color(NULL, NULL, key, "label_color", CYAN)
                               : draw_color(NULL, page, NULL, "label_color", CYAN);
        rgb_t date_color = draw_color(NULL, page, NULL, "accent_color", MAGENTA);
        ok = render_clock(buf + PREFIX, w->width, w->height, color, have ? t.tm_hour : -1, t.tm_min, (t.tm_wday + 6) % 7,
                          t.tm_mday, t.tm_mon + 1, time_color, date_color);
    } else {
        char label[256];
        placeholders_fill(text_of(key, "label"), cJSON_GetObjectItem(config, "variables"), label, sizeof label);
        const char *position = text_of(key, "label_pos");
        const cJSON *size = key ? cJSON_GetObjectItem(key, "label_size") : NULL;
        uint8_t *icon = load_icon(page_id, w);
        ok = render_key(buf + PREFIX, w->width, w->height, color, icon, label,
                        draw_color(NULL, page, key, "label_color", (rgb_t){255, 255, 255}), position[0] ? position : "bottom",
                        cJSON_IsNumber(size) ? size->valueint : 100);
        free(icon);
    }
    esp_err_t err = ESP_ERR_NO_MEM;
    if (ok) {
        uint8_t type, reply[DECKD_REPLY_MAX];
        size_t n;
        err = deckd_request(d, DECKD_SET_KEY_IMAGE, buf, PREFIX + pixels, &type, reply, sizeof reply, &n);
        if (err == ESP_OK && type != DECKD_OK) ESP_LOGE(TAG, "deckd refused key %u (0x%02x)", w->index, type);
    }
    free(buf);
    return err;
}

void drawdyn_request_all(void)
{
    device_state_t state;
    device_snapshot(&state);
    cJSON *config = config_parse();
    const cJSON *page = config ? config_find_page(config, state.page) : NULL;
    for (int i = 0; page && i < state.key_count; i++) {
        if (drawdyn_is_dynamic(config_find_key(page, state.keys[i].index), &state.keys[i])) {
            device_request_redraw((int)state.keys[i].index);
        }
    }
    cJSON_Delete(config);
}
