/**
 * module.c
 */

#define _GNU_SOURCE

#include "module.h"
#include "daemon.h"
#include "daemon_logger.h"
#include "../../src/libs/json/cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

#include <syslog.h>
#include <fcntl.h>
#include <sys/wait.h>     
#include <sys/resource.h> 
#include <sys/timerfd.h>  
#include <sys/epoll.h>    
#include <signal.h>    

typedef enum {
    MODE_HEARTBEAT = 0,
    MODE_RELTIME,
    MODE_ABSTIME
} timer_mode_t;

struct module {
    volatile sig_atomic_t module_alive;
    pid_t module_pid;             
    char *module_name;      
    char *module_binary_path;
    char *module_config;
	char *system_config;
    
    timer_mode_t module_timertype; 
    char *module_absolut_time;     
    long module_relative_time;     
    int module_timer_fd;           
    int module_heartbeat;
    int module_start_immediately;
};

static char *module_read_conf_file(const char *filepath);

void module_init(module_t **self)
{
    if (!self) exit(EXIT_FAILURE);
    *self = NULL; 
}

void module_deinit(module_t **self_ptr, int count, int epoll_fd)
{
    if (!self_ptr || !*self_ptr) return;
    module_t *table = *self_ptr;

    for (int i = 0; i < count; i++)
    {
        if (table[i].module_pid > 0)
        {
            syslog(LOG_NOTICE, "Killing process: %s PID %d", table[i].module_name, table[i].module_pid);			
            kill(table[i].module_pid, SIGTERM);
            
            struct rusage usage;
            int status;
            if (wait4(table[i].module_pid, &status, 0, &usage) != -1)
			{
                syslog(LOG_INFO, "%s RAM Peak: %ld KB", table[i].module_name, usage.ru_maxrss);
            }
        }
        
        if (table[i].module_timer_fd > 0)
		{
            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, table[i].module_timer_fd, NULL);
            close(table[i].module_timer_fd);
        }

        free(table[i].module_name);
        free(table[i].module_binary_path);
        free(table[i].module_config);
		free(table[i].system_config);
        free(table[i].module_absolut_time);
    }
    
    free(table);
    *self_ptr = NULL;
}

int module_load(module_t **self_ptr, int old_count, const char *config_path, const char *prj_root_path, int epoll_fd, int heartbeat_sig)
{
    char *json_data = module_read_conf_file(config_path);
    if (!json_data) {
        syslog(LOG_ERR, "Failed to read config file.");
        return 0;
    }

    cJSON *root = cJSON_Parse(json_data);
    if (!root)
	{
        free(json_data);
        return 0;
    }

    cJSON *modules_arr = cJSON_GetObjectItemCaseSensitive(root, "modules");
    int new_count = cJSON_GetArraySize(modules_arr);

    module_t *new_table = calloc(new_count, sizeof(struct module));
    if (!new_table)
	{
        cJSON_Delete(root);
        free(json_data);
        return 0;
    }

	cJSON *system = cJSON_GetObjectItemCaseSensitive(root, "system");
    cJSON *mod = NULL;
    int i = 0;
    cJSON_ArrayForEach(mod, modules_arr)
    {
        cJSON *n = cJSON_GetObjectItemCaseSensitive(mod, "name");
        cJSON *p = cJSON_GetObjectItemCaseSensitive(mod, "bin_path");
        if (!cJSON_IsString(n) || !cJSON_IsString(p)) continue;

        new_table[i].module_name = strdup(n->valuestring);
        if (!new_table[i].module_name) continue;

        char raw_path[PATH_MAX];
        snprintf(raw_path, sizeof(raw_path), "%s/%s", prj_root_path, p->valuestring);
        new_table[i].module_binary_path = realpath(raw_path, NULL);
        if (!new_table[i].module_binary_path)
		{
            syslog(LOG_ERR, "Cannot resolve binary for module '%s': %s", new_table[i].module_name, strerror(errno));
            free(new_table[i].module_name);
            new_table[i].module_name = NULL;
            continue;
        }

        new_table[i].module_config   = cJSON_PrintUnformatted(mod);
		new_table[i].system_config   = cJSON_PrintUnformatted(system);
        new_table[i].module_timer_fd = -1;

        cJSON *t_flag = cJSON_GetObjectItemCaseSensitive(mod, "Timer-type");
        cJSON *start_now = cJSON_GetObjectItemCaseSensitive(mod, "start_immediately");
        new_table[i].module_start_immediately = cJSON_IsBool(start_now) && cJSON_IsTrue(start_now);
        if (cJSON_IsNumber(t_flag) && t_flag->valueint == 1)
		{
            cJSON *t_abs = cJSON_GetObjectItemCaseSensitive(mod, "Abs-time");
            if (cJSON_IsString(t_abs))
			{
                new_table[i].module_timertype    = MODE_ABSTIME;
                new_table[i].module_absolut_time = strdup(t_abs->valuestring);
            }
			else
			{
                cJSON *t_rel = cJSON_GetObjectItemCaseSensitive(mod, "Rel-time");
                new_table[i].module_timertype      = MODE_RELTIME;
                new_table[i].module_relative_time  = cJSON_IsNumber(t_rel) ? t_rel->valueint : ONE_MINUTE;
            }
        }
		else
		{
            new_table[i].module_timertype = MODE_HEARTBEAT;
            cJSON *h = cJSON_GetObjectItemCaseSensitive(mod, "heartbeat_interval");
            new_table[i].module_heartbeat = cJSON_IsNumber(h) ? h->valueint : HEARTBEAT_SPEED;
        }
        i++;
    }

    module_t *old_table = *self_ptr;
    if (old_table) {
        for (int j = 0; j < i; j++) {

            /** Skip any module that failed path resolution above */
            if (!new_table[j].module_name || !new_table[j].module_binary_path)
			{
                continue;				
			}
            for (int k = 0; k < old_count; k++)
			{
                if (!old_table[k].module_name || !old_table[k].module_binary_path)
				{
                    continue;					
				}
                if (strcmp(new_table[j].module_name, old_table[k].module_name) != 0 ||
                    strcmp(new_table[j].module_binary_path, old_table[k].module_binary_path) != 0)
				{
					continue;
				}
                /* Same binary and name found — now check if the config changed. */
                int config_changed = (new_table[j].module_config && old_table[k].module_config) ? strcmp(new_table[j].module_config, old_table[k].module_config) != 0 : 1;
                if (!config_changed)
				{
					new_table[j].module_pid      = old_table[k].module_pid;
					new_table[j].module_alive    = old_table[k].module_alive;
					new_table[j].module_timer_fd = old_table[k].module_timer_fd;
				}
				else
				{
					syslog(LOG_NOTICE, "Config changed for '%s' — scheduling restart.", new_table[j].module_name);

					if (old_table[k].module_pid > 0)
					{
						kill(old_table[k].module_pid, SIGTERM);
					}
					if (old_table[k].module_timer_fd > 0)
					{
						epoll_ctl(epoll_fd, EPOLL_CTL_DEL, old_table[k].module_timer_fd, NULL);
						close(old_table[k].module_timer_fd);
					}					
				}
                /** prevent module_deinit from double-killing.  */
                old_table[k].module_pid      = 0;
                old_table[k].module_timer_fd = -1;
                break;
            }
        }
        /** We deinit all the unused modules */
        module_deinit(&old_table, old_count, epoll_fd);
    }

    *self_ptr = new_table;
    /** Spawn / configure timers for entries that need it. */
    for (int j = 0; j < i; j++)
	{
        if (!new_table[j].module_name)
		{
		    continue;	
		}
        if (new_table[j].module_timertype != MODE_HEARTBEAT && new_table[j].module_timer_fd == -1)
        {
            module_timer_config(&new_table[j], epoll_fd);
            if (new_table[j].module_start_immediately && new_table[j].module_pid == 0)
            {
                module_spawn(&new_table[j], prj_root_path, heartbeat_sig);
            }
        }
        if (new_table[j].module_timertype == MODE_HEARTBEAT && new_table[j].module_pid == 0)
        {
            module_spawn(&new_table[j], prj_root_path, heartbeat_sig);
        }
    }
    cJSON_Delete(root);
    free(json_data);
    return i;
}

void module_spawn(module_t *self, const char *prj_root_path, int heartbeat_sig)
{
    if (!self)
	{
	    return;	
	}
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) == -1)
	{
        syslog(LOG_ERR, "pipe2 failed for %s: %m", self->module_name);
        return;
    }
    pid_t p = fork();
    if (p < 0)
	{
        syslog(LOG_ERR, "fork failed for %s: %m", self->module_name);
        close(pipefd[0]);
        close(pipefd[1]);
        return;
    }
    if (p == 0)
    {
        /** CHILD CONTEXT Unblock all signals the parent blocked for signalfd. */		
        sigset_t child_mask;
        sigemptyset(&child_mask);
        sigaddset(&child_mask, SIGINT);
        sigaddset(&child_mask, SIGTERM);
        sigaddset(&child_mask, SIGCHLD);
        sigaddset(&child_mask, heartbeat_sig);
        sigprocmask(SIG_UNBLOCK, &child_mask, NULL);
        close(pipefd[0]);

        if (chdir(prj_root_path) == -1)
		{
            int err = errno;
            write(pipefd[1], &err, sizeof(err));
            _exit(EXIT_FAILURE);
        }
        if (self->module_config && self->system_config)
		{
            setenv("SUNSPOTS_CONFIG", self->module_config, 1);
			setenv("SUNSPOTS_SYSTEM", self->system_config, 1);
        }
        char sig_str[16];
        snprintf(sig_str, sizeof(sig_str), "%d", heartbeat_sig);
        setenv("SUNSPOTS_SIGNAL", sig_str, 1);
        char *args[] = { self->module_binary_path, NULL };
        execvp(args[0], args);
        int err = errno;
        write(pipefd[1], &err, sizeof(err));
        _exit(EXIT_FAILURE);
    }

    /** PARENT CONTEXT */
    close(pipefd[1]);
    int err = 0;
    if (read(pipefd[0], &err, sizeof(err)) > 0)
	{
        syslog(LOG_ERR, "spawn failed for '%s': %s",
               self->module_name, strerror(err));
        self->module_pid = -1;
    }
	else
	{
        self->module_pid   = p;
        self->module_alive = 1;
        syslog(LOG_INFO, "Execvp on: '%s' [PID %d]", self->module_name, p);
		daemon_logger_send(self->module_name, "Started.");
    }
    close(pipefd[0]);
}

void module_health_check_all(module_t *array, int count, const char *prj_root_path, int heartbeat_sig)
{
    if (!array) return;
    for (int i = 0; i < count; i++)
    {
        if (array[i].module_timertype != MODE_HEARTBEAT) continue;
        if (array[i].module_pid <= 0)
		{
            syslog(LOG_ERR, "Process '%s' terminated. Restarting.", array[i].module_name);
            daemon_logger_send("DAEMON", "A process terminated, restarting.");
            module_spawn(&array[i], prj_root_path, heartbeat_sig);
        }
        else if (!array[i].module_alive)
		{
            syslog(LOG_ERR, "Process '%s' hung (no heartbeat). Restarting.", array[i].module_name);
			daemon_logger_send("DAEMON", "A process hung, restarting");
            kill(array[i].module_pid, SIGKILL);
            waitpid(array[i].module_pid, NULL, 0);
            array[i].module_pid = 0;
            module_spawn(&array[i], prj_root_path, heartbeat_sig);
        }
        else
		{
            array[i].module_alive = 0;
        }
    }
}

void module_handle_timer_event(module_t *array, int count, int timer_fd, const char *prj_root, int epoll_fd, int heartbeat_sig)
{
    for (int i = 0; i < count; i++)
	{
        if (array[i].module_timer_fd == timer_fd)
		{
            uint64_t exp;
            if (read(timer_fd, &exp, sizeof(exp)) > 0)
			{
                if (array[i].module_pid <= 0)
				{
                    module_spawn(&array[i], prj_root, heartbeat_sig);
                }
                if (array[i].module_timertype == MODE_ABSTIME)
				{
                    module_timer_config(&array[i], epoll_fd);
                }
            }
            break;
        }
    }
}

void module_timer_config(module_t *self, int epoll_fd)
{
    if (self->module_timertype == MODE_HEARTBEAT) return;    
    int clock_type = (self->module_timertype == MODE_ABSTIME) ? CLOCK_REALTIME : CLOCK_MONOTONIC;    
    if (self->module_timer_fd <= 0)
	{
        self->module_timer_fd = timerfd_create(clock_type, TFD_NONBLOCK | TFD_CLOEXEC);
        struct epoll_event ev = { .events = EPOLLIN, .data.fd = self->module_timer_fd };
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, self->module_timer_fd, &ev);
    }    
    struct itimerspec its = {0};
    int timerfd_flags = 0;
    if (self->module_timertype == MODE_ABSTIME)
	{
        int hr, min;
        sscanf(self->module_absolut_time, "%d:%d", &hr, &min);        
        struct timespec curr;
        clock_gettime(clock_type, &curr);        
        struct tm tm_buf;
        struct tm *lt = localtime_r(&curr.tv_sec, &tm_buf);
        lt->tm_hour = hr; lt->tm_min = min; lt->tm_sec = 0; lt->tm_isdst = -1;         
        time_t run_at = mktime(lt);
        if (run_at <= curr.tv_sec)
		{
            lt->tm_mday += 1;
            run_at = mktime(lt);
        }
        its.it_value.tv_sec = run_at;
        timerfd_flags = TFD_TIMER_ABSTIME;
    }
	else
	{
        its.it_value.tv_sec = self->module_relative_time;
        its.it_interval.tv_sec = self->module_relative_time;
    }    
    timerfd_settime(self->module_timer_fd, timerfd_flags, &its, NULL);
}
const char *module_get_system_config(module_t *self)
{
	return (self && self[0].system_config) ? self[0].system_config : NULL;
}

pid_t module_get_pid(module_t *self)
{
	return self ? self->module_pid : -1;
}

void module_set_pid(module_t *self, int value)
{
	if (self) self->module_pid = value;
}

void module_set_alive(module_t *self, int value)
{
	if (self) self->module_alive = value;
}

module_t *module_find_by_pid(module_t *array, int count, pid_t pid)
{
    if (!array) return NULL;
    for (int i = 0; i < count; i++)
	{
        if (array[i].module_pid == pid) return &array[i];
    }
    return NULL;
}

static char *module_read_conf_file(const char *filepath)
{
    FILE *fptr = fopen(filepath, "rb");
    if (!fptr) return NULL;     
    fseek(fptr, 0, SEEK_END);
    size_t len = ftell(fptr);
    rewind(fptr);
    char *buf = malloc(len + 1);
    if (!buf) { fclose(fptr); return NULL; }   
    size_t rb = fread(buf, 1, len, fptr);
    buf[(rb < len) ? rb : len] = '\0';
    fclose(fptr);
    return buf;
}
