#include "logbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#define LINES 256
#define LOG_LINE_MAX 192

static struct {
    uint32_t seq;
    char text[LOG_LINE_MAX];
} s_lines[LINES];
static uint32_t s_next = 1; /* number of the next line */
static char s_partial[LOG_LINE_MAX];
static size_t s_partial_len;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_uart;

/* Called with s_lock held. */
static void push_line(void)
{
    s_partial[s_partial_len] = 0;
    uint32_t slot = s_next % LINES;
    s_lines[slot].seq = s_next;
    memcpy(s_lines[slot].text, s_partial, s_partial_len + 1);
    s_next++;
    s_partial_len = 0;
}

static void store(const char *text, size_t len)
{
    portENTER_CRITICAL(&s_lock);
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        if (c == 0x1b) { /* skip a colour sequence: ESC [ ... letter */
            while (i + 1 < len && !((text[i + 1] >= 'A' && text[i + 1] <= 'Z') || (text[i + 1] >= 'a' && text[i + 1] <= 'z')))
                i++;
            i++;
        } else if (c == '\n') {
            push_line();
        } else if (c != '\r' && s_partial_len < LOG_LINE_MAX - 1) {
            s_partial[s_partial_len++] = c;
        }
    }
    portEXIT_CRITICAL(&s_lock);
}

static int hook(const char *fmt, va_list args)
{
    char text[256];
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(text, sizeof text, fmt, copy);
    va_end(copy);
    if (n > 0) store(text, (size_t)n < sizeof text ? (size_t)n : sizeof text - 1);
    return s_uart ? s_uart(fmt, args) : n;
}

void logbuf_init(void)
{
    s_uart = esp_log_set_vprintf(hook);
}

size_t logbuf_read(uint32_t since, char *out, size_t size, uint32_t *next)
{
    size_t used = 0;
    portENTER_CRITICAL(&s_lock);
    uint32_t oldest = s_next > LINES ? s_next - LINES : 1;
    uint32_t seq = since < oldest || since > s_next ? oldest : since;
    for (; seq < s_next; seq++) {
        const char *text = s_lines[seq % LINES].text;
        size_t len = strlen(text);
        if (used + len + 1 > size) break;
        memcpy(out + used, text, len);
        out[used + len] = '\n';
        used += len + 1;
    }
    *next = seq;
    portEXIT_CRITICAL(&s_lock);
    return used;
}
