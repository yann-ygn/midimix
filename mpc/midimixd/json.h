// json: a small JSON reader for the native API's messages (watch frames up to 1 MiB, receipts).
// One document is parsed in place: strings are unescaped inside the caller's buffer, which must
// end with a NUL. The result is a flat array of nodes; node 0 is the root.

#ifndef JSON_H
#define JSON_H

#include <stddef.h>

enum json_type { JSON_NULL, JSON_FALSE, JSON_TRUE, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT };

struct json_node {
    unsigned char type;
    int next;         // next member / element of the parent, or -1
    int child;        // first member / element of an object or array, or -1
    const char *key;  // member name when the parent is an object
    const char *str;  // a string's value
    double num;       // a number's value
};

struct json {
    struct json_node *n;
    int count, cap;
};

// Parses text[0..len) (text[len] must be NUL). 0, or -1 if it isn't one valid JSON document.
// Reuses j's node array between calls; free it with json_free.
int json_parse(struct json *j, char *text, size_t len);
void json_free(struct json *j);

// The member called key of object node obj, or -1 (no such member, obj not an object, obj -1).
int json_get(const struct json *j, int obj, const char *key);
// Follows a dotted path of member names ("muteContext.sequence.sequenceId"), or -1.
int json_path(const struct json *j, int obj, const char *path);
// A string node's value, or NULL for any other node (or -1).
const char *json_str(const struct json *j, int i);
// 1 true, 0 false, -1 anything else (null, missing, not a boolean).
int json_bool(const struct json *j, int i);
// A number node's value, or dflt.
double json_num(const struct json *j, int i, double dflt);

#endif
