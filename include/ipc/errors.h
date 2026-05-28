/*
 * axctl - IPC error codes and helpers
 */
#ifndef AXCTL_IPC_ERRORS_H
#define AXCTL_IPC_ERRORS_H

/* Standard error codes returned by compositor operations */
#define AXCTL_OK                        0
#define AXCTL_ERR_NOT_SUPPORTED        -1
#define AXCTL_ERR_WINDOW_NOT_FOUND     -2
#define AXCTL_ERR_WORKSPACE_NOT_FOUND  -3
#define AXCTL_ERR_COMPOSITOR_UNAVAIL   -4
#define AXCTL_ERR_SUBSCRIPTION_FAILED  -5
#define AXCTL_ERR_OPERATION_FAILED     -6
#define AXCTL_ERR_INVALID_PARAMS       -7
#define AXCTL_ERR_CONNECT              -8
#define AXCTL_ERR_IO                   -9
#define AXCTL_ERR_PARSE                -10
#define AXCTL_ERR_OOM                  -11

/* Thread-local error message buffer */
void axctl_set_error(const char *fmt, ...);
const char *axctl_get_error(void);
void axctl_clear_error(void);

/* Get human-readable string for an error code */
const char *axctl_error_code_str(int code);

#endif /* AXCTL_IPC_ERRORS_H */
