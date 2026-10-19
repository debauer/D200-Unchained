#include "placeholders.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "wallclock.h"

#define NAME_MAX_LEN 32

/* Length of `{name}` at s (including the braces), 0 if it is not a placeholder. */
static size_t token_length(const char *s, char *name)
{
    if (s[0] != '{' || !islower((unsigned char)s[1])) return 0;
    size_t n = 0;
    while (n < NAME_MAX_LEN && (islower((unsigned char)s[1 + n]) || isdigit((unsigned char)s[1 + n]) || s[1 + n] == '_')) {
        name[n] = s[1 + n];
        n++;
    }
    if (s[1 + n] != '}') return 0;
    name[n] = 0;
    return n + 2;
}

bool placeholders_present(const char *text)
{
    char name[NAME_MAX_LEN + 1];
    for (const char *p = text; *p; p++) {
        if ((p[0] == '{' && p[1] == '{') || (p[0] == '}' && p[1] == '}')) {
            p++;
            continue;
        }
        if (token_length(p, name)) return true;
    }
    return false;
}

static bool is_variable(const cJSON *variables, const char *name)
{
    const cJSON *v;
    cJSON_ArrayForEach(v, variables) {
        const cJSON *n = cJSON_GetObjectItem(v, "name");
        if (cJSON_IsString(n) && strcmp(n->valuestring, name) == 0) return true;
    }
    return false;
}

static void builtin(const char *name, char *out, size_t size)
{
    static const char *const days[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    struct tm t;
    bool have = wallclock_now(&t);
    if (strcmp(name, "time") == 0) {
        if (have) snprintf(out, size, "%02d:%02d", t.tm_hour, t.tm_min);
        else snprintf(out, size, "--:--");
    } else if (have) {
        snprintf(out, size, "%s %02d.%02d.", days[(t.tm_wday + 6) % 7], t.tm_mday, t.tm_mon + 1);
    } else {
        snprintf(out, size, "---");
    }
}

void placeholders_fill(const char *text, const cJSON *variables, char *out, size_t size)
{
    size_t used = 0;
    char name[NAME_MAX_LEN + 1], value[24];
    for (const char *p = text; *p && used + 1 < size;) {
        if ((p[0] == '{' && p[1] == '{') || (p[0] == '}' && p[1] == '}')) {
            out[used++] = p[0];
            p += 2;
            continue;
        }
        size_t n = token_length(p, name);
        if (n) {
            if (strcmp(name, "time") == 0 || strcmp(name, "date") == 0) builtin(name, value, sizeof value);
            else if (is_variable(variables, name)) snprintf(value, sizeof value, "?");
            else n = 0; /* unknown: stays as written, so a typo is visible */
            if (n) {
                size_t len = strlen(value);
                if (used + len >= size) break;
                memcpy(out + used, value, len);
                used += len;
                p += n;
                continue;
            }
        }
        out[used++] = *p++;
    }
    out[used] = 0;
}
