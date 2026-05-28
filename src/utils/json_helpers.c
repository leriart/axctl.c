/*
 * axctl - JSON helper implementations
 */
#include "utils/json_helpers.h"
#include <string.h>

const char *json_get_string(struct json_object *obj, const char *key) {
    struct json_object *val = NULL;
    if (!json_object_object_get_ex(obj, key, &val)) return "";
    const char *s = json_object_get_string(val);
    return s ? s : "";
}

int json_get_int(struct json_object *obj, const char *key, int def) {
    struct json_object *val = NULL;
    if (!json_object_object_get_ex(obj, key, &val)) return def;
    return json_object_get_int(val);
}

double json_get_double(struct json_object *obj, const char *key, double def) {
    struct json_object *val = NULL;
    if (!json_object_object_get_ex(obj, key, &val)) return def;
    return json_object_get_double(val);
}

bool json_get_bool(struct json_object *obj, const char *key, bool def) {
    struct json_object *val = NULL;
    if (!json_object_object_get_ex(obj, key, &val)) return def;
    return json_object_get_boolean(val);
}

struct json_object *json_get_object(struct json_object *obj, const char *key) {
    struct json_object *val = NULL;
    if (!json_object_object_get_ex(obj, key, &val)) return NULL;
    if (json_object_get_type(val) != json_type_object) return NULL;
    return val;
}

struct json_object *json_get_array(struct json_object *obj, const char *key) {
    struct json_object *val = NULL;
    if (!json_object_object_get_ex(obj, key, &val)) return NULL;
    if (json_object_get_type(val) != json_type_array) return NULL;
    return val;
}

struct json_object *json_create_metadata(void) {
    return json_object_new_object();
}

void json_metadata_add_string(struct json_object *meta, const char *key, const char *val) {
    if (!meta || !key) return;
    json_object_object_add(meta, key, json_object_new_string(val ? val : ""));
}

void json_metadata_add_int(struct json_object *meta, const char *key, int val) {
    if (!meta || !key) return;
    json_object_object_add(meta, key, json_object_new_int(val));
}
