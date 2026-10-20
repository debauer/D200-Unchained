#include "oidc.h"

#include <string.h>

static bool utf8(const unsigned char *s, size_t length)
{
    for (size_t i = 0; i < length;) {
        unsigned c = s[i++];
        if (c < 128) continue;
        unsigned count = c >= 0xC2 && c <= 0xDF ? 1 : c >= 0xE0 && c <= 0xEF ? 2 : c >= 0xF0 && c <= 0xF4 ? 3 : 0;
        if (!count || length - i < count) return false;
        unsigned value = c & (count == 1 ? 31 : count == 2 ? 15 : 7);
        for (unsigned j = 0; j < count; j++) {
            if ((s[i] & 0xC0) != 0x80) return false;
            value = value << 6 | (s[i++] & 63);
        }
        if ((count == 1 && value < 128) || (count == 2 && value < 2048) || (count == 3 && value < 65536) ||
            (value >= 0xD800 && value <= 0xDFFF) || value > 0x10FFFF) return false;
    }
    return true;
}

static bool unique(const cJSON *node)
{
    const cJSON *child;
    cJSON_ArrayForEach(child, node) {
        if (cJSON_IsObject(node)) {
            for (const cJSON *previous = node->child; previous != child; previous = previous->next)
                if (!strcmp(previous->string, child->string)) return false;
        }
        if ((cJSON_IsArray(child) || cJSON_IsObject(child)) && !unique(child)) return false;
    }
    return true;
}

cJSON *oidc_json(const char *text, size_t length)
{
    if (!length || length > 65536 || memchr(text, 0, length) || !utf8((const unsigned char *)text, length)) return NULL;
    bool quoted = false;
    unsigned depth = 0;
    for (size_t i = 0; i < length; i++) {
        char c = text[i];
        if (quoted && c == '\\') {
            if (i + 5 < length && text[i + 1] == 'u' && !memcmp(text + i + 2, "0000", 4)) return NULL;
            i++;
            continue;
        }
        if (c == '"') quoted = !quoted;
        if (quoted) continue;
        if ((c == '{' || c == '[') && ++depth > 12) return NULL;
        if (c == '}' || c == ']') { if (!depth) return NULL; depth--; }
    }
    const char *end;
    cJSON *json = cJSON_ParseWithLengthOpts(text, length, &end, false);
    while (json && end < text + length && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) end++;
    if (!cJSON_IsObject(json) || end != text + length || !unique(json)) { cJSON_Delete(json); return NULL; }
    return json;
}
