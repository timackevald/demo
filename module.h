/**
 * @file module.h
 */

#ifndef MODULE_H
#define MODULE_H

#include <sys/types.h>
#include <signal.h>

typedef struct module module_t;

/**
 * @brief Prepares the module handle.
 * @param self Pointer to the module handle to initialize.
 */
void module_init(module_t **self);

/**
 * @brief Cleans up all modules, kills processes, and frees memory.
 * @param self_ptr Pointer to the module array handle.
 * @param count Number of modules currently in the array.
 * @param epoll_fd The epoll instance to remove timers from.
 */
void module_deinit(module_t **self_ptr, int count, int epoll_fd);

/**
 * @brief Parses the JSON config and populates/updates the module array.
 * @return The number of modules loaded.
 */
int module_load(module_t **self_ptr, int old_count,  const char *config_path, 
                const char *prj_root_path, int epoll_fd, int heartbeat_sig);

/**
 * @brief Executes fork/execvp for a specific module.
 */
void module_spawn(module_t *self, const char *prj_root_path, int heartbeat_sig);

/**
 * @brief Checks if heartbeat modules are alive; restarts them if hung or dead.
 */
void module_health_check_all(module_t *array, int count, const char *prj_root_path, int heartbeat_sig);

/**
 * @brief Identifies which module's timer triggered and reacts.
 */
void module_handle_timer_event(module_t *array, int count, int timer_fd, 
                               const char *prj_root, int epoll_fd, int heartbeat_sig);

/**
 * @brief Configures the timerfd for a specific module (Rel-time or Abs-time).
 */
void module_timer_config(module_t *self, int epoll_fd);

/**
 * @brief Finds a module in an array based on its Process ID.
 */
module_t *module_find_by_pid(module_t *array, int count, pid_t pid);

/**
 * @breif Returns system config
 */
const char *module_get_system_config(module_t *self);
pid_t module_get_pid(module_t *self);
void  module_set_pid(module_t *self, int value);
void  module_set_alive(module_t *self, int value);

#endif /* MODULE_H */
