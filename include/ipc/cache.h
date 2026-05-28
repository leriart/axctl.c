/*
 * axctl - Thread-safe state cache
 *
 * Maintains a snapshot of compositor state (windows, workspaces, monitors)
 * protected by a read-write lock for concurrent access.
 */
#ifndef AXCTL_IPC_CACHE_H
#define AXCTL_IPC_CACHE_H

#include "ipc/types.h"
#include <pthread.h>
#include <stdbool.h>

typedef struct {
    pthread_rwlock_t lock;
    axctl_window_array_t windows;
    axctl_workspace_array_t workspaces;
    axctl_monitor_array_t monitors;
} axctl_state_cache_t;

/* Create and initialise a state cache */
axctl_state_cache_t *axctl_cache_new(void);

/* Free a state cache and all stored data */
void axctl_cache_free(axctl_state_cache_t *cache);

/* ── Window operations ──────────────────────────────────────────────── */
void axctl_cache_add_window(axctl_state_cache_t *c, axctl_window_t w);
void axctl_cache_remove_window(axctl_state_cache_t *c, const char *id);
void axctl_cache_update_window_title(axctl_state_cache_t *c, const char *id, const char *title);
void axctl_cache_update_window_workspace(axctl_state_cache_t *c, const char *id,
                                          const char *workspace_id, const char *monitor_id);
void axctl_cache_update_window_state(axctl_state_cache_t *c, const char *id, bool is_fullscreen);
void axctl_cache_update_window_floating(axctl_state_cache_t *c, const char *id, bool floating);
void axctl_cache_mark_window_focused(axctl_state_cache_t *c, const char *id);
void axctl_cache_set_windows(axctl_state_cache_t *c, axctl_window_array_t *arr);

/* Returns a deep copy; caller must free with axctl_window_array_free */
axctl_window_array_t axctl_cache_get_windows(axctl_state_cache_t *c);

/* ── Workspace operations ───────────────────────────────────────────── */
void axctl_cache_set_workspaces(axctl_state_cache_t *c, axctl_workspace_array_t *arr);
axctl_workspace_array_t axctl_cache_get_workspaces(axctl_state_cache_t *c);

/* ── Monitor operations ─────────────────────────────────────────────── */
void axctl_cache_set_monitors(axctl_state_cache_t *c, axctl_monitor_array_t *arr);
axctl_monitor_array_t axctl_cache_get_monitors(axctl_state_cache_t *c);

#endif /* AXCTL_IPC_CACHE_H */
