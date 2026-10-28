// json: see json.h. A recursive-descent parser with a depth limit; strings are decoded in place
// (an escape never decodes to more bytes than it takes), numbers with strtod.

#include "json.h"

#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 64

struct parser {
    struct json *j;
    char *p;
    int depth;
};

static int new_node(struct json *j, int type) {
    if (j->count == j->cap) {
        int cap = j->cap ? j->cap * 2 : 1024;
        struct json_node *n = realloc(j->n, (size_t)cap * sizeof *n);
        if (!n) return -1;
        j->n = n;
        j->cap = cap;
    }
    struct json_node *n = &j->n[j->count];
    memset(n, 0, sizeof *n);
    n->type = (unsigned char)type;
    n->next = n->child = -1;
    return j->count++;
}

static void skip_space(struct parser *ps) {
    while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r') ps->p++;
}

static int hex4(const char *s) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1;
    }
    return v;
}

// At the opening quote. Decodes the string in place and returns it, NUL-terminated, or NULL.
static char *parse_string(struct parser *ps) {
    char *r = ps->p + 1, *w = r, *start = r;
    for (;;) {
        unsigned char c = (unsigned char)*r;
        if (c == '"') break;
        if (c < 0x20) return NULL;  // includes the final NUL: unterminated
        if (c != '\\') {
            *w++ = *r++;
            continue;
        }
        r++;
        switch (*r++) {
        case '"': *w++ = '"'; break;
        case '\\': *w++ = '\\'; break;
        case '/': *w++ = '/'; break;
        case 'b': *w++ = '\b'; break;
        case 'f': *w++ = '\f'; break;
        case 'n': *w++ = '\n'; break;
        case 'r': *w++ = '\r'; break;
        case 't': *w++ = '\t'; break;
        case 'u': {
            int u = hex4(r);
            if (u < 0) return NULL;
            r += 4;
            int lo = u >= 0xD800 && u <= 0xDBFF && r[0] == '\\' && r[1] == 'u' ? hex4(r + 2) : -1;
            if (lo >= 0xDC00 && lo <= 0xDFFF) {  // a surrogate pair: 12 bytes in, 4 out
                r += 6;
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
            } else if (u >= 0xD800 && u <= 0xDFFF) {
                u = 0xFFFD;  // a lone surrogate: valid JSON, not a character
            }
            if (u < 0x80) {
                *w++ = (char)u;
            } else if (u < 0x800) {
                *w++ = (char)(0xC0 | u >> 6);
                *w++ = (char)(0x80 | (u & 0x3F));
            } else if (u < 0x10000) {
                *w++ = (char)(0xE0 | u >> 12);
                *w++ = (char)(0x80 | (u >> 6 & 0x3F));
                *w++ = (char)(0x80 | (u & 0x3F));
            } else {
                *w++ = (char)(0xF0 | u >> 18);
                *w++ = (char)(0x80 | (u >> 12 & 0x3F));
                *w++ = (char)(0x80 | (u >> 6 & 0x3F));
                *w++ = (char)(0x80 | (u & 0x3F));
            }
            break;
        }
        default: return NULL;
        }
    }
    *w = '\0';  // w <= r: at most overwrites the closing quote
    ps->p = r + 1;
    return start;
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }

// The end of a number as JSON writes them: -?(0|[1-9][0-9]*)(.[0-9]+)?([eE][+-]?[0-9]+)?, or NULL.
static char *number_end(char *p) {
    if (*p == '-') p++;
    if (*p == '0') p++;
    else if (is_digit(*p)) while (is_digit(*p)) p++;
    else return NULL;
    if (*p == '.') {
        if (!is_digit(*++p)) return NULL;
        while (is_digit(*p)) p++;
    }
    if (*p == 'e' || *p == 'E') {
        if (*++p == '+' || *p == '-') p++;
        if (!is_digit(*p)) return NULL;
        while (is_digit(*p)) p++;
    }
    return p;
}

static int parse_value(struct parser *ps);

// Links each new child after the previous one. Returns 0 or -1.
static int parse_container(struct parser *ps, int node, int object) {
    char close = object ? '}' : ']';
    int last = -1;
    ps->p++;
    skip_space(ps);
    if (*ps->p == close) {
        ps->p++;
        return 0;
    }
    if (++ps->depth > MAX_DEPTH) return -1;
    for (;;) {
        char *key = NULL;
        if (object) {
            if (*ps->p != '"' || !(key = parse_string(ps))) return -1;
            skip_space(ps);
            if (*ps->p++ != ':') return -1;
            skip_space(ps);
        }
        int child = parse_value(ps);
        if (child < 0) return -1;
        ps->j->n[child].key = key;
        if (last < 0) ps->j->n[node].child = child;
        else ps->j->n[last].next = child;
        last = child;
        skip_space(ps);
        if (*ps->p == ',') {
            ps->p++;
            skip_space(ps);
            continue;
        }
        if (*ps->p++ != close) return -1;
        ps->depth--;
        return 0;
    }
}

static int parse_value(struct parser *ps) {
    char *p = ps->p;
    int node;
    switch (*p) {
    case '{':
    case '[':
        node = new_node(ps->j, *p == '{' ? JSON_OBJECT : JSON_ARRAY);
        if (node < 0 || parse_container(ps, node, *p == '{') < 0) return -1;
        return node;
    case '"': {
        char *s = parse_string(ps);
        if (!s || (node = new_node(ps->j, JSON_STRING)) < 0) return -1;
        ps->j->n[node].str = s;
        return node;
    }
    case 't':
    case 'f':
    case 'n': {
        static const char *const words[] = {"null", "false", "true"};
        for (int type = JSON_NULL; type <= JSON_TRUE; type++) {
            size_t len = strlen(words[type]);
            if (strncmp(p, words[type], len)) continue;
            ps->p += len;
            return new_node(ps->j, type);
        }
        return -1;
    }
    default: {
        char *end = number_end(p), *strtod_end;  // strtod alone would take "inf", hex, "01"
        if (!end) return -1;
        double v = strtod(p, &strtod_end);
        if (strtod_end != end) return -1;
        ps->p = end;
        if ((node = new_node(ps->j, JSON_NUMBER)) < 0) return -1;
        ps->j->n[node].num = v;
        return node;
    }
    }
}

int json_parse(struct json *j, char *text, size_t len) {
    struct parser ps = {.j = j, .p = text};
    j->count = 0;
    if (text[len] != '\0') return -1;
    skip_space(&ps);
    if (parse_value(&ps) != 0) return -1;
    skip_space(&ps);
    return ps.p == text + len ? 0 : -1;
}

void json_free(struct json *j) {
    free(j->n);
    j->n = NULL;
    j->count = j->cap = 0;
}

int json_get(const struct json *j, int obj, const char *key) {
    if (obj < 0 || j->n[obj].type != JSON_OBJECT) return -1;
    for (int i = j->n[obj].child; i >= 0; i = j->n[i].next)
        if (!strcmp(j->n[i].key, key)) return i;
    return -1;
}

int json_path(const struct json *j, int obj, const char *path) {
    char key[64];
    while (obj >= 0 && *path) {
        size_t len = strcspn(path, ".");
        if (len >= sizeof key) return -1;
        memcpy(key, path, len);
        key[len] = '\0';
        obj = json_get(j, obj, key);
        path += len + (path[len] == '.');
    }
    return obj;
}

const char *json_str(const struct json *j, int i) {
    return i >= 0 && j->n[i].type == JSON_STRING ? j->n[i].str : NULL;
}

int json_bool(const struct json *j, int i) {
    if (i < 0) return -1;
    if (j->n[i].type == JSON_TRUE) return 1;
    if (j->n[i].type == JSON_FALSE) return 0;
    return -1;
}

double json_num(const struct json *j, int i, double dflt) {
    return i >= 0 && j->n[i].type == JSON_NUMBER ? j->n[i].num : dflt;
}
