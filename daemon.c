/**
 * @file daemon.c
 */

#define _GNU_SOURCE
#include <linux/limits.h>
#include "daemon_logger.h"
#include "daemon.h"
#include "module.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <syslog.h>
#include <libgen.h> 
#include <fcntl.h>  
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/timerfd.h>
#include <sys/signalfd.h>
#include <signal.h>

struct daemon_var {
    int n_modules_running;
    volatile sig_atomic_t alive;
    char prj_root_folder[PATH_MAX];
    char prj_config_dir[PATH_MAX];
    char prj_config_path[PATH_MAX];

	int hearbeat_sig;
    int epoll_fd;
    int inotify_fd;
    int global_timer_fd;
    int signal_fd;
   
    module_t *modules; 
};

static void daemon_set_system_config(daemon_var_t *self);
static void daemon_resolve_paths(daemon_var_t *self);
static void daemon_daemonize(void);
static void daemon_epoll_setup(daemon_var_t *self);
static void daemon_write_pidfile(const char *prj_root_folder);
static void daemon_reap_zombies(daemon_var_t *self);
static void daemon_handle_heartbeat(daemon_var_t *self, uint32_t sender_pid);

void daemon_init(daemon_var_t **self_ptr)
{
    if (!self_ptr) exit(EXIT_FAILURE);    
    daemon_var_t *self = (daemon_var_t*)calloc(1, sizeof(struct daemon_var));
    if (!self) exit(EXIT_FAILURE);    
    self->alive = 1;
	/** We stay away from the first rt signals due to glibc */
	self->hearbeat_sig = HEARTBEAT_SIG + HEARTBEAT_OFFSET;
    
    daemon_resolve_paths(self);	
    daemon_daemonize();    

	openlog("SUNSPOTS_DAEMON", LOG_PID, LOG_DAEMON);
	daemon_epoll_setup(self);
    syslog(LOG_NOTICE, "Sunspots daemon started. Detached and darkened.");

    module_init(&self->modules);
	
    self->n_modules_running = module_load(&self->modules, self->n_modules_running,
										  self->prj_config_path, self->prj_root_folder,
										  self->epoll_fd, self->hearbeat_sig);
    daemon_write_pidfile(self->prj_root_folder);
	daemon_set_system_config(self);	
	
    *self_ptr = self;
}

void daemon_deinit(daemon_var_t **self_ptr)
{
    if (!self_ptr || !*self_ptr) return;
    
    daemon_var_t *self = *self_ptr;
    syslog(LOG_NOTICE, "Shutting down daemon and cleaning up...");
	daemon_logger_send("DAEMON", "Shutting down daemon and cleaning up...");
    
    module_deinit(&self->modules, self->n_modules_running, self->epoll_fd);
    if (self->global_timer_fd > 0)
	{
	    close(self->global_timer_fd);	
	}
    if (self->inotify_fd > 0)
	{
	    close(self->inotify_fd);	
	}
    if (self->epoll_fd > 0)
	{
        close(self->epoll_fd);		
	}
    if (self->signal_fd > 0)
	{
    	close(self->signal_fd);
	}
    free(self);
    *self_ptr = NULL;
	
    syslog(LOG_NOTICE, "Daemon vanished. All children reaped.");
	daemon_logger_send("DAEMON", "Daemon vanished. All children reaped.");
	
    closelog();
}

void daemon_run(daemon_var_t *self)	
{
    struct epoll_event events[MAX_EVENTS];    
    /** * fdsi is populated by the kernel when reading from signal_fd.
     * It structures the signal data, allowing us to safely check the 
     * signal number (ssi_signo) and the sender's PID (ssi_pid).
     */
    struct signalfd_siginfo fdsi;    
    syslog(LOG_NOTICE, "SUNSPOTS daemon setup complete! Waiting for events...");
	daemon_logger_send("DAEMON", "SUNSPOTS daemon setup complete! Waiting for events...");
    while (self->alive)
    {
        int nfds = epoll_wait(self->epoll_fd, events, MAX_EVENTS, -1);
        if (nfds == -1)
        {
            if (errno == EINTR) continue; // Interrupted by an unblocked signal
            syslog(LOG_ERR, "epoll_wait error: %m");
            break;
        }
        
        for (int i = 0; i < nfds; i++)
        {
            /** Event 1: Synchronous Signals */
            if (events[i].data.fd == self->signal_fd)
            {
                while (read(self->signal_fd, &fdsi, sizeof(fdsi)) == sizeof(fdsi))
                {
                    if (fdsi.ssi_signo == SIGINT || fdsi.ssi_signo == SIGTERM)
					{						
						self->alive = 0;
					} 
					else if (fdsi.ssi_signo == SIGCHLD)
					{
						daemon_reap_zombies(self);
					} 					
					else if (fdsi.ssi_signo == (uint32_t)self->hearbeat_sig)
					{
						daemon_handle_heartbeat(self, fdsi.ssi_pid);
					}
                }
            }
            /** Event 2: Health check-up */
            else if (events[i].data.fd == self->global_timer_fd)
            {
                uint64_t exp;
                if (read(self->global_timer_fd, &exp, sizeof(exp)) > 0)
                {
                    module_health_check_all(self->modules, self->n_modules_running, self->prj_root_folder, self->hearbeat_sig);
                }
            }
            /** Event 3: Config change (Hot Reload) */
            else if (events[i].data.fd == self->inotify_fd)
            {
                char buf[4096] __attribute__ ((aligned(__alignof__(struct inotify_event))));
                ssize_t len;
                static time_t last_reload = 0;                
                while ((len = read(self->inotify_fd, buf, sizeof(buf))) > 0)
                {
                    const struct inotify_event *event;
                    for (char *ptr = buf; ptr < buf + len; ptr += sizeof(struct inotify_event) + event->len)
                    {
                        event = (const struct inotify_event*)ptr;
                        if (event->len && strcmp(event->name, CONFIG_FILENAME) == 0)
                        {
                            time_t now = time(NULL);
                            if (now - last_reload >= 1)
                            {
                                syslog(LOG_NOTICE, "Config change detected. Performing hot-reload.");
								daemon_logger_send("DEAEMON", "Config change detected. Performing hot-reload.");
                                self->n_modules_running = module_load(&self->modules, self->n_modules_running,
																	  self->prj_config_path,
																	  self->prj_root_folder,
																	  self->epoll_fd, self->hearbeat_sig);
                                last_reload = now;
                            }
                        }
                    }
                }
            }
            /** Event 4: Abs/ Rel-timer triggered */
            else
            {
                module_handle_timer_event(self->modules, self->n_modules_running, events[i].data.fd, self->prj_root_folder, self->epoll_fd, self->hearbeat_sig);
            }
        }
    }
}

static void daemon_set_system_config(daemon_var_t *self)
{
	const char *system_config = module_get_system_config(self->modules);
	if (system_config)
	{
		cJSON *sys = cJSON_Parse(system_config);
		if (sys)
		{
			cJSON *sp = cJSON_GetObjectItemCaseSensitive(sys, "socket_path");
			if (cJSON_IsString(sp))
			{
				char abs_sp[PATH_MAX];
				snprintf(abs_sp, sizeof(abs_sp), "%s/%s", self->prj_root_folder, sp->valuestring);
				cJSON_SetValuestring(sp, abs_sp);
			}
			char *resolved_abs_sp = cJSON_PrintUnformatted(sys);
			if (resolved_abs_sp)
			{
				setenv("SUNSPOTS_SYSTEM", resolved_abs_sp, 1);
				free(resolved_abs_sp);
				cJSON_Delete(sys);
			}
		}
	}
}

static void daemon_reap_zombies(daemon_var_t *self)
{
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
    {
        module_t *m = module_find_by_pid(self->modules, self->n_modules_running, pid);
        if (m)
		{
            module_set_pid(m, 0);
            module_set_alive(m, 0);
        }
    }
}

static void daemon_handle_heartbeat(daemon_var_t *self, uint32_t sender_pid)
{
    module_t *m = module_find_by_pid(self->modules, self->n_modules_running, sender_pid);
    if (m)
	{
        module_set_alive(m, 1);
    }
}

static void daemon_resolve_paths(daemon_var_t *self)
{
    char exe_path[PATH_MAX];
    char temp_path[PATH_MAX];
    
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path));
    if (len == -1)
	{
        perror("FATAL: Cannot read /proc/self/exe");
        exit(EXIT_FAILURE);
    }
    exe_path[len] = '\0';
    strcpy(temp_path, exe_path);
    char *curr_dir = dirname(temp_path);    
    while (strcmp(curr_dir, "/") != 0)
    {
        char check_path[PATH_MAX];
        snprintf(check_path, sizeof(check_path), "%s/config/%s", curr_dir, CONFIG_FILENAME);
        if (access(check_path, R_OK) == 0)
        {
            strncpy(self->prj_root_folder, curr_dir, PATH_MAX);
            strncpy(self->prj_config_path, check_path, PATH_MAX);
            snprintf(self->prj_config_dir, sizeof(self->prj_config_dir), "%s/config", curr_dir);
            return;
        }
        curr_dir = dirname(curr_dir);
    }
    fprintf(stderr, "FATAL: Could not find anchor (./config/%s)!\n", CONFIG_FILENAME);
    exit(EXIT_FAILURE);
}

static void daemon_epoll_setup(daemon_var_t *self)
{
    self->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (self->epoll_fd == -1) exit(EXIT_FAILURE);
    /** * SIGSET MASKING:
     * 1. We create an empty sigset and add the signals we want to manage.
     * 2. sigprocmask blocks the CPU from interrupting execution when these arrive.
     * 3. signalfd creates a file descriptor that reads these blocked signals as data.
     */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, self->hearbeat_sig);

    if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1)
	{
    	exit(EXIT_FAILURE);	
	}
    self->signal_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (self->signal_fd == -1)
	{
		exit(EXIT_FAILURE);
	}
    self->inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (self->inotify_fd == -1)
	{
		exit(EXIT_FAILURE);
	}		
    inotify_add_watch(self->inotify_fd, self->prj_config_dir, IN_CLOSE_WRITE | IN_MOVED_TO);    
    self->global_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (self->global_timer_fd == -1)
	{
	    exit(EXIT_FAILURE);	
	}    
    struct itimerspec ts =
	{
        .it_interval = { HEALTH_CHECKUP_INTERVAL, 0 },
        .it_value    = { HEALTH_CHECKUP_INTERVAL, 0 }
    };
    timerfd_settime(self->global_timer_fd, 0, &ts, NULL);
    struct epoll_event ev;
    ev.events = EPOLLIN;    
    int fd_to_add[] =
	{
		self->signal_fd,
		self->global_timer_fd,
		self->inotify_fd
	};
    for (int i = 0; i < 3; i++)
	{
        ev.data.fd = fd_to_add[i];
        epoll_ctl(self->epoll_fd, EPOLL_CTL_ADD, fd_to_add[i], &ev);
    }
}

static void daemon_daemonize(void)
{
#ifndef DEBUG
    pid_t init_pid = fork();
    if (init_pid < 0)
	{
    	exit(EXIT_FAILURE);	
	}
    if (init_pid > 0)
	{
	    exit(EXIT_SUCCESS);	
	}
    if (setsid() < 0)
	{
		exit(EXIT_FAILURE);
	}
    pid_t second_pid = fork(); 
    if (second_pid < 0)
	{
    	exit(EXIT_FAILURE);	
	}
    if (second_pid > 0)
	{
	    exit(EXIT_SUCCESS);	
	}
    umask(0); 
    if (chdir("/") != 0)
	{
	    exit(EXIT_FAILURE);	
	}
    close(STDIN_FILENO);
	close(STDOUT_FILENO);
	close(STDERR_FILENO); 
    int fd = open("/dev/null", O_RDWR); 
    if (fd != -1)
	{
        dup2(fd, STDIN_FILENO);
		dup2(fd, STDOUT_FILENO);
		dup2(fd, STDERR_FILENO);
        if (fd > 2)
		{
    		close(fd);	
		}
    }
#else
	printf("DEBUG MODE\n");
	printf("Daemon lives! Use 'kill $(cat <path/to>/Sunspots/logs/sunspots.pid)' to kill it\n");
#endif	
}

static void daemon_write_pidfile(const char *prj_root_folder)
{
    char pid_path[PATH_MAX];
    char log_path[PATH_MAX];
    snprintf(pid_path, sizeof(pid_path), "%s/logs/sunspots.pid", prj_root_folder);
    FILE *fptr = fopen(pid_path, "w");
    if (fptr)
	{
        fprintf(fptr, "%d\n", getpid());
        fclose(fptr);
    }
    snprintf(log_path, sizeof(log_path), "%s/logs/sunspots.pid.log", prj_root_folder);
    fptr = fopen(log_path, "a");
    if (fptr)
	{
        time_t now = time(NULL);
        char ts[32];
        struct tm tm_buf;
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime_r(&now, &tm_buf));
        fprintf(fptr, "[%s] PID %d\n", ts, getpid());
        fclose(fptr);
    }
}
