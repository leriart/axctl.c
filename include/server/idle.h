/*
 * axctl - Idle management
 *
 * Manages Wayland idle notifications and inhibitors:
 *   - ext-idle-notify-v1 for idle monitoring
 *   - zwp-idle-inhibit-v1 for idle prevention
 *   - systemd-inhibit for system-level idle control
 *   - Application and media inhibitor checks
 */
#ifndef AXCTL_SERVER_IDLE_H
#define AXCTL_SERVER_IDLE_H

#include <stdint.h>
#include <json-c/json.h>

typedef struct axctl_idle_manager axctl_idle_manager_t;

/* State snapshots for JSON serialisation */
typedef struct {
    uint32_t id;
    int enabled;
    uint32_t timeout_ms;
    int respect_inhibitors;
    int is_idle;
} axctl_idle_monitor_state_t;

typedef struct {
    uint32_t id;
    int enabled;
} axctl_idle_inhibitor_state_t;

/* Callback for idle monitor state changes */
typedef void (*axctl_idle_monitor_cb_t)(uint32_t id, int is_idle, void *userdata);

/* Create a new idle manager. Returns NULL on failure (non-fatal). */
axctl_idle_manager_t *axctl_idle_manager_create(void);

/* Free the idle manager */
void axctl_idle_manager_destroy(axctl_idle_manager_t *mgr);

/* Set callback for idle monitor state changes */
void axctl_idle_manager_set_callback(axctl_idle_manager_t *mgr,
                                      axctl_idle_monitor_cb_t cb, void *userdata);

/* Legacy inhibit (simple on/off) */
int axctl_idle_inhibit(axctl_idle_manager_t *mgr, int on);
int axctl_idle_is_inhibited(axctl_idle_manager_t *mgr);

/* Blocking wait operations */
int axctl_idle_wait(axctl_idle_manager_t *mgr, uint32_t timeout_ms);
int axctl_idle_wait_resume(axctl_idle_manager_t *mgr, uint32_t timeout_ms);
int axctl_idle_wait_input(axctl_idle_manager_t *mgr, uint32_t timeout_ms);
int axctl_idle_wait_input_resume(axctl_idle_manager_t *mgr, uint32_t timeout_ms);

/* Polling operations */
int axctl_idle_is_idle(axctl_idle_manager_t *mgr, uint32_t timeout_ms, int *is_idle);
int axctl_idle_is_input_idle(axctl_idle_manager_t *mgr, uint32_t timeout_ms, int *is_idle);

/* Idle monitor CRUD */
int axctl_idle_monitor_create(axctl_idle_manager_t *mgr, uint32_t timeout_ms,
                               int respect_inhibitors, int enabled,
                               axctl_idle_monitor_state_t *out);
int axctl_idle_monitor_update(axctl_idle_manager_t *mgr, uint32_t id,
                               uint32_t timeout_ms, int respect_inhibitors,
                               int enabled, axctl_idle_monitor_state_t *out);
int axctl_idle_monitor_get(axctl_idle_manager_t *mgr, uint32_t id,
                            axctl_idle_monitor_state_t *out);
int axctl_idle_monitor_destroy(axctl_idle_manager_t *mgr, uint32_t id);

/* Idle inhibitor CRUD */
int axctl_idle_inhibitor_create(axctl_idle_manager_t *mgr, int enabled,
                                 axctl_idle_inhibitor_state_t *out);
int axctl_idle_inhibitor_set(axctl_idle_manager_t *mgr, uint32_t id, int enabled,
                              axctl_idle_inhibitor_state_t *out);
int axctl_idle_inhibitor_get(axctl_idle_manager_t *mgr, uint32_t id,
                              axctl_idle_inhibitor_state_t *out);
int axctl_idle_inhibitor_destroy(axctl_idle_manager_t *mgr, uint32_t id);

/* System-level inhibition */
int axctl_idle_inhibit_system(axctl_idle_manager_t *mgr, int on);
int axctl_idle_is_system_inhibited(axctl_idle_manager_t *mgr);

/* Application/media checks */
int axctl_idle_app_check(axctl_idle_manager_t *mgr,
                          char **patterns, int pattern_count,
                          struct json_object **out);
int axctl_idle_media_check(axctl_idle_manager_t *mgr,
                            struct json_object **out);

/* JSON serialisation helpers */
struct json_object *axctl_idle_monitor_state_to_json(const axctl_idle_monitor_state_t *s);
struct json_object *axctl_idle_inhibitor_state_to_json(const axctl_idle_inhibitor_state_t *s);

#endif /* AXCTL_SERVER_IDLE_H */
