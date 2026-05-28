/*
 * axctl - JSON-RPC 2.0 server over Unix sockets
 *
 * Listens on /tmp/axctl-<uid>.sock and dispatches requests to the
 * active compositor backend. Supports event subscriptions where
 * connected clients receive real-time state dumps.
 */
#ifndef AXCTL_SERVER_H
#define AXCTL_SERVER_H

#include "ipc/compositor.h"
#include "ipc/cache.h"

typedef struct axctl_server axctl_server_t;

/* Create a new server for the given compositor and socket path */
axctl_server_t *axctl_server_create(axctl_compositor_t *comp, const char *socket_path);

/* Start the server (blocks, accepting connections). Call from a thread. */
int axctl_server_start(axctl_server_t *srv);

/* Stop/destroy the server gracefully and free all resources */
void axctl_server_destroy(axctl_server_t *srv);

/* Get the state cache */
axctl_state_cache_t *axctl_server_get_cache(axctl_server_t *srv);

#endif /* AXCTL_SERVER_H */
