/**
 * @file daemon.h
 */

#ifndef DAEMON_H
#define DAEMON_H

#include <signal.h>
#include <linux/limits.h>

#define CONFIG_FILENAME         "sunspots.json"
#define MAX_EVENTS              10
#define HEALTH_CHECKUP_INTERVAL 5
#define HEARTBEAT_SPEED         2
#define ONE_MINUTE              60
#define HEARTBEAT_SIG           SIGRTMIN
#define HEARTBEAT_OFFSET        4

typedef struct daemon_var daemon_var_t;

/**
 * @brief Fully initializes the daemon.
 * Handles path resolution, daemonization, signal setup, and initial module loading.
 * @param self_ptr Pointer to the handle to be allocated.
 */
void daemon_init(daemon_var_t **self_ptr);

/**
 * @brief Starts the primary epoll event loop.
 * Blocks until a termination signal (SIGTERM/SIGINT) is received.
 * @param self Pointer to the initialized daemon context.
 */
void daemon_run(daemon_var_t *self);

/**
 * @brief Performs a graceful shutdown.
 * Kills child processes, closes file descriptors, and frees all memory.
 * @param self_ptr Pointer to the handle to be destroyed and nulled.
 */
void daemon_deinit(daemon_var_t **self_ptr);

#endif /* DAEMON_H */
