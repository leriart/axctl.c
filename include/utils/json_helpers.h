/*
 * axctl - JSON helper utilities wrapping json-c
 */
#ifndef AXCTL_JSON_HELPERS_H
#define AXCTL_JSON_HELPERS_H

#include <json-c/json.h>
#include <stdbool.h>

/* Safely get a string from a JSON object, returns "" if missing */
const char *json_get_string(struct json_object *obj, const char *key);

/* Safely get an int from a JSON object, returns def if missing */
int json_get_int(struct json_object *obj, const char *key, int def);

/* Safely get a double from a JSON object, returns def if missing */
double json_get_double(struct json_object *obj, const char *key, double def);

/* Safely get a bool from a JSON object, returns def if missing */
bool json_get_bool(struct json_object *obj, const char *key, bool def);

/* Get a nested object, returns NULL if missing */
struct json_object *json_get_object(struct json_object *obj, const char *key);

/* Get a nested array, returns NULL if missing */
struct json_object *json_get_array(struct json_object *obj, const char *key);

/* Create a metadata JSON object from a map-like structure */
struct json_object *json_create_metadata(void);

/* Add a string entry to metadata */
void json_metadata_add_string(struct json_object *meta, const char *key, const char *val);

/* Add an int entry to metadata */
void json_metadata_add_int(struct json_object *meta, const char *key, int val);

#endif /* AXCTL_JSON_HELPERS_H */
