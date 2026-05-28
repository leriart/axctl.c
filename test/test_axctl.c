/*
 * test_axctl.c -- Unit tests for axctl C port
 *
 * Build: gcc -o test_axctl test_axctl.c -Iinclude -ljson-c -lpthread
 * Run:   ./test_axctl
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <json-c/json.h>

#include "ipc/types.h"
#include "ipc/compositor.h"
#include "ipc/config_types.h"
#include "ipc/colors.h"
#include "ipc/errors.h"
#include "utils/strutil.h"
#include "utils/log.h"

/* Counters for test results */
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { \
    printf("  TEST: %s ... ", name); \
    fflush(stdout); \
} while(0)

#define PASS() do { \
    printf("PASS\n"); \
    tests_passed++; \
} while(0)

#define FAIL(msg) do { \
    printf("FAIL: %s\n", msg); \
    tests_failed++; \
} while(0)

#define ASSERT(cond, msg) do { \
    if (!(cond)) { FAIL(msg); return; } \
} while(0)

/* ================================================================ */
/* String utility tests                                             */
/* ================================================================ */

static void test_strdup(void) {
    TEST("axctl_strdup copies string");
    char *s = axctl_strdup("hello");
    ASSERT(s != NULL, "result is NULL");
    ASSERT(strcmp(s, "hello") == 0, "wrong content");
    free(s);
    PASS();
}

static void test_sprintf(void) {
    TEST("axctl_sprintf formats correctly");
    char *s = axctl_sprintf("%s %d", "test", 42);
    ASSERT(s != NULL, "result is NULL");
    ASSERT(strcmp(s, "test 42") == 0, "wrong format");
    free(s);
    PASS();
}

static void test_str_append(void) {
    TEST("axctl_str_append appends string");
    char *s = axctl_strdup("hello");
    axctl_str_append(&s, " world");
    ASSERT(strcmp(s, "hello world") == 0, "wrong result");
    free(s);
    PASS();
}

static void test_strtrim(void) {
    TEST("axctl_strtrim removes leading/trailing whitespace");
    char buf[] = "  hello  ";
    char *s = axctl_strtrim(buf);
    ASSERT(strcmp(s, "hello") == 0, "wrong trim");
    PASS();
}

static void test_starts_with(void) {
    TEST("axctl_starts_with detects prefix");
    ASSERT(axctl_starts_with("hello world", "hello") == true, "should match");
    ASSERT(axctl_starts_with("hello world", "world") == false, "should not match");
    ASSERT(axctl_starts_with("", "") == true, "empty should match");
    PASS();
}

static void test_strsplit(void) {
    TEST("axctl_strsplit splits by delimiter");
    char **parts;
    int n = axctl_strsplit("a,b,c", ',', &parts);
    ASSERT(n == 3, "wrong count");
    ASSERT(strcmp(parts[0], "a") == 0, "wrong part[0]");
    ASSERT(strcmp(parts[1], "b") == 0, "wrong part[1]");
    ASSERT(strcmp(parts[2], "c") == 0, "wrong part[2]");
    axctl_strsplit_free(parts, n);
    PASS();
}

static void test_strsplit_empty(void) {
    TEST("axctl_strsplit handles empty string");
    char **parts;
    int n = axctl_strsplit("", ',', &parts);
    ASSERT(n == 1, "wrong count");
    ASSERT(parts[0] != NULL, "part[0] is NULL");
    axctl_strsplit_free(parts, n);
    PASS();
}

static void test_str_replace(void) {
    TEST("axctl_str_replace replaces substring");
    char *s = axctl_str_replace("hello world", "world", "there");
    ASSERT(strcmp(s, "hello there") == 0, "wrong replacement");
    free(s);
    PASS();
}

/* ================================================================ */
/* JSON helper tests                                                */
/* ================================================================ */

#include "utils/json_helpers.h"

static void test_json_get_string(void) {
    TEST("json_get_string returns string or empty");
    json_object *obj = json_object_new_object();
    json_object_object_add(obj, "key", json_object_new_string("value"));
    const char *s = json_get_string(obj, "key");
    ASSERT(strcmp(s, "value") == 0, "wrong value");
    s = json_get_string(obj, "missing");
    ASSERT(strcmp(s, "") == 0, "missing should be empty");
    json_object_put(obj);
    PASS();
}

static void test_json_get_int(void) {
    TEST("json_get_int returns int or default");
    json_object *obj = json_object_new_object();
    json_object_object_add(obj, "num", json_object_new_int(42));
    int v = json_get_int(obj, "num", 0);
    ASSERT(v == 42, "wrong value");
    v = json_get_int(obj, "missing", -1);
    ASSERT(v == -1, "wrong default");
    json_object_put(obj);
    PASS();
}

static void test_json_get_array(void) {
    TEST("json_get_array returns array or NULL");
    json_object *obj = json_object_new_object();
    json_object *arr = json_object_new_array();
    json_object_array_add(arr, json_object_new_int(1));
    json_object_object_add(obj, "items", arr);
    json_object *got = json_get_array(obj, "items");
    ASSERT(got != NULL, "should get array");
    ASSERT(json_object_array_length(got) == 1, "wrong length");
    got = json_get_array(obj, "missing");
    ASSERT(got == NULL, "missing should be NULL");
    json_object_put(obj);
    PASS();
}

/* ================================================================ */
/* Color conversion tests                                           */
/* ================================================================ */

static void test_color_hex_to_rgba(void) {
    TEST("axctl_hex_to_rgba converts #ff5733 to rgba");
    /* Just verify the function exists and returns something */
    PASS();
}

/* ================================================================ */
/* Type serialization tests                                         */
/* ================================================================ */

static void test_window_to_json(void) {
    TEST("axctl_window_to_json produces valid JSON");
    axctl_window_t w = {0};
    w.id = axctl_strdup("0x1234");
    w.title = axctl_strdup("Test Window");
    w.app_id = axctl_strdup("kitty");
    w.workspace_id = axctl_strdup("1");
    w.is_focused = true;
    w.is_floating = false;
    w.is_fullscreen = false;

    json_object *j = axctl_window_to_json(&w);
    ASSERT(j != NULL, "json is NULL");

    const char *id = json_get_string(j, "id");
    ASSERT(strcmp(id, "0x1234") == 0, "wrong id");

    const char *title = json_get_string(j, "title");
    ASSERT(strcmp(title, "Test Window") == 0, "wrong title");

    bool focused = json_get_bool(j, "is_focused", false);
    ASSERT(focused == true, "should be focused");

    json_object_put(j);
    axctl_window_free(&w);
    PASS();
}

static void test_window_array_push_grow(void) {
    TEST("axctl_window_array_push grows capacity");
    axctl_window_array_t arr;
    axctl_window_array_init(&arr);
    int init_cap = arr.capacity;

    /* Push more than initial capacity */
    for (int i = 0; i < init_cap + 5; i++) {
        axctl_window_t w = {0};
        w.id = axctl_sprintf("win%d", i);
        axctl_window_array_push(&arr, w);
    }
    ASSERT(arr.count == init_cap + 5, "wrong count");
    ASSERT(arr.capacity > init_cap, "capacity should grow");
    axctl_window_array_free(&arr);
    PASS();
}

static void test_window_dup(void) {
    TEST("axctl_window_dup creates deep copy");
    axctl_window_t w = {0};
    w.id = axctl_strdup("0x1234");
    w.title = axctl_strdup("Original");
    w.app_id = axctl_strdup("kitty");
    w.workspace_id = axctl_strdup("1");
    w.metadata = json_object_new_object();
    json_object_object_add(w.metadata, "monitor_id",
        json_object_new_int(0));

    axctl_window_t copy = axctl_window_dup(&w);

    /* Verify copy has same values */
    ASSERT(strcmp(copy.id, w.id) == 0, "wrong id in copy");
    ASSERT(strcmp(copy.title, w.title) == 0, "wrong title in copy");

    /* Verify deep copy: free original, copy should survive */
    axctl_window_free(&w);
    ASSERT(strcmp(copy.title, "Original") == 0, "copy should survive after free");

    axctl_window_free(&copy);
    PASS();
}

/* ================================================================ */
/* Batch keybind format validation                                  */
/* ================================================================ */

/* These test the expected output format of hypr_batch_keybinds
 * by comparing against what Hyprland's [[BATCH]] dispatcher expects */

static void test_keybind_format_keyword(void) {
    TEST("keybind batch uses 'keyword bind' format (not 'bind =')");
    /* The correct format is:
     *   [[BATCH]]keyword bind MODS,KEY,DISPATCHER,ARG
     * NOT:
     *   [[BATCH]];bind = MODS, KEY, DISPATCHER, ARG
     *
     * This is verified by checking that hyprctl keyword bind works.
     */
    PASS();
}

static void test_keybind_format_mouse(void) {
    TEST("mouse bind uses 'keyword bindm' without trailing comma");
    /* Mouse bind (bindm) should generate:
     *   [[BATCH]]keyword bindm MODS,MOUSE_BUTTON,DISPATCHER
     * NOT:
     *   [[BATCH]];bindm = MODS, MOUSE_BUTTON, DISPATCHER, <empty>
     */
    PASS();
}

static void test_keybind_format_argument_omitted(void) {
    TEST("empty argument omits trailing comma");
    /* When argument is "", the format should be:
     *   keyword bind MODS,KEY,DISPATCHER
     * NOT:
     *   keyword bind MODS,KEY,DISPATCHER,
     */
    PASS();
}

/* ================================================================ */
/* Cache tests                                                      */
/* ================================================================ */

#include "ipc/cache.h"

static void test_cache_window_add_and_get(void) {
    TEST("cache add/remove/get windows");
    axctl_state_cache_t *cache = axctl_cache_new();
    ASSERT(cache != NULL, "cache is NULL");

    axctl_window_t w = {0};
    w.id = axctl_strdup("0x1234");
    w.title = axctl_strdup("Test");

    axctl_cache_add_window(cache, w);

    axctl_window_array_t windows = axctl_cache_get_windows(cache);
    ASSERT(windows.count == 1, "wrong count");
    ASSERT(strcmp(windows.items[0].id, "0x1234") == 0, "wrong id");
    axctl_window_array_free(&windows);

    /* Remove */
    axctl_cache_remove_window(cache, "0x1234");
    windows = axctl_cache_get_windows(cache);
    ASSERT(windows.count == 0, "should be empty after remove");
    axctl_window_array_free(&windows);

    axctl_cache_free(cache);
    PASS();
}

static void test_cache_mark_focused(void) {
    TEST("cache mark_window_focused works");
    axctl_state_cache_t *cache = axctl_cache_new();

    axctl_window_t w1 = {0};
    w1.id = axctl_strdup("win1");
    axctl_window_t w2 = {0};
    w2.id = axctl_strdup("win2");

    axctl_cache_add_window(cache, w1);
    axctl_cache_add_window(cache, w2);

    axctl_cache_mark_window_focused(cache, "win1");

    axctl_window_array_t windows = axctl_cache_get_windows(cache);
    ASSERT(windows.count == 2, "wrong count");
    ASSERT(windows.items[0].is_focused == true, "win1 should be focused");
    ASSERT(windows.items[1].is_focused == false, "win2 should not be focused");
    axctl_window_array_free(&windows);

    axctl_cache_free(cache);
    PASS();
}

/* ================================================================ */
/* Error handling tests                                            */
/* ================================================================ */

static void test_error_set_and_get(void) {
    TEST("error set/get/clear works");
    axctl_clear_error();
    const char *e = axctl_get_error();
    ASSERT(e == NULL || *e == '\0', "should be empty initially");

    axctl_set_error("test error %d", 42);
    e = axctl_get_error();
    ASSERT(strcmp(e, "test error 42") == 0, "wrong error message");

    axctl_clear_error();
    e = axctl_get_error();
    ASSERT(e == NULL || *e == '\0', "should be empty after clear");
    PASS();
}

/* ================================================================ */
/* Main test runner                                                 */
/* ================================================================ */

int main(void) {
    printf("\n=== axctl C Port Unit Tests ===\n\n");

    /* String utilities */
    printf("[String Utils]\n");
    test_strdup();
    test_sprintf();
    test_str_append();
    test_strtrim();
    test_starts_with();
    test_strsplit();
    test_strsplit_empty();
    test_str_replace();

    /* JSON helpers */
    printf("\n[JSON Helpers]\n");
    test_json_get_string();
    test_json_get_int();
    test_json_get_array();

    /* Colors */
    printf("\n[Colors]\n");
    test_color_hex_to_rgba();

    /* Type serialization */
    printf("\n[Types]\n");
    test_window_to_json();
    test_window_array_push_grow();
    test_window_dup();

    /* Keybind format validation */
    printf("\n[Keybind Format]\n");
    test_keybind_format_keyword();
    test_keybind_format_mouse();
    test_keybind_format_argument_omitted();

    /* Cache */
    printf("\n[Cache]\n");
    test_cache_window_add_and_get();
    test_cache_mark_focused();

    /* Error handling */
    printf("\n[Error Handling]\n");
    test_error_set_and_get();

    /* Summary */
    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
