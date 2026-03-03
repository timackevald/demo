# Demo daemon
Daemon written for school-project. 

**Overview**: The daemon process is responsible for spawning "modules" defined in the `sunspots.json` configuration file. It also performs health checks on child processes and restarts unresponsive or hung children. 

## Operations
The daemon can spawn three types of processes:
1. **Heartbeat Process**: The child process checks in with the daemon on a predetermined interval (in seconds). A successful check-in sets an `alive` flag, which the daemon verifies during its health check.
2. **Relative Timer Process**: The child process is spawned whenever a predetermined time interval (in seconds) expires. If the previous process is still running, a second instance will start, and so on.
3. **Absolute Timer Process**: The child process is spawned at a specific absolute time, such as `"14:50"`. After spawning the process, the daemon resets the timer. This logic accounts for leap years and daylight savings time to ensure the timer remains absolute.

### Child Processes
All children spawned by the daemon use `Sunspots/` as their current working directory. Keep this in mind when adding your own modules to the system. This pathing is reflected in the `"bin_path"` field of the `sunspots.json` configuration file, allowing the daemon to locate the correct binaries during runtime.

Furthermore, children of the daemon inherit their specific configuration via the `"SUNSPOTS_CONFIG"` environment variable. This means the configuration block defined in the module's JSON is available to the process after it starts. If the module uses a heartbeat, it will also receive the `"SUNSPOTS_SIGNAL"` variable, which contains the correct `SIGRTMIN` plus offset to use when signaling the daemon. Finally, the `"SUNSPOTS_SYSTEM"` configuration contains paths required for the SDK and the **Daemon Logger** program.

Children also inherit closed `stdin`, `stdout`, and `stderr` streams for security and background operation.

### Hot-reload
The daemon performs a hot-reload if the `sunspots.json` configuration file is modified. It compares the stored configuration with the new file to determine which modules need to be updated or restarted.

## Configuration 
The configuration file `sunspots.json` is located in the `Sunspots/config` folder. It consists of two blocks:
* **"system"**: Contains general settings for all modules, such as the database path and the Unix socket path used for communicating with the logger. Settings added here are available to all modules via `getenv("SUNSPOTS_SYSTEM")`.
* **"modules"**: An array of objects representing the binaries managed by the daemon. Several mandatory fields must be populated for the daemon to successfully run a binary.

```JSON
{
    "system": {
        "version": "1.1",
        "name": "Sunspots Daemon",
        "log_path": "./logs/daemon.log",
        "socket_path": "./logs/sunspots.sock"
    },
    "modules": [
        {
            "name": "Task_Heartbeat_mode",
            "bin_path": "./path/to/binary/from/root",
            "Timer-type": 0,
            "heartbeat_interval": 3
        },
        {
            "name": "Task_Absolute_Mode",
            "bin_path": "./path/to/binary/from/root",
            "Timer-type": 1,
            "Abs-time": "16:52"
        },
        {
            "name": "Task_Relative_Mode",
            "bin_path": "./path/to/binary/from/root",
            "Timer-type": 1,
            "Rel-time": 30,
            "start_immediately": true
        },
        {
            "name": "Logger",
            "bin_path": "./src/core/logger",
            "Timer-type": 0,
            "heartbeat_interval": 3,
            "log_path": "./logs/daemon.log",
            "socket_path": "./logs/sunspots.sock"
        },
        {
            "name": "Frontend",
            "bin_path": "./build/debug/src/frontend/sunspots_frontend",
            "Timer-type": 0,
            "heartbeat_interval": 5,
            "server_threads": 8,
            "server_port_http": 10480,
            "server_port_https": null,
            "server_listen_queue": 16,
            "client_queue_size": 32,
            "allow_file_search": true,
            "file_search_dir": "./htdocs"
        }
    ]
}
#### Timer Types
The "Timer-type" field is set to either 0 or 1:
- Type 0 (Heartbeat Mode): Requires a heartbeat_interval to be defined.
- Type 1 (Timer Mode): Used for either Absolute or Relative timing. If "Abs-time" is provided, the process runs at a specific time (24h format, HH:mm). If "Rel-time" is provided, the process runs at a repeating interval.

All intervals for heartbeats and relative timers are defined in seconds.

Optional field for timer modules:
- `start_immediately`: if `true`, daemon spawns the module once during daemon startup (or reload) in addition to the configured timer schedule.

## Running and stopping
After building the project the binary can be found in `Sunspots/build/debug/src/core`. Use `./sunspots_daemon` to run the daemon. The PID of the daemon will be written to `Sunspots/logs/sunspots.pid`, to kill the daemon use `kill $(cat sunspots.pid)` (seen from `Sunspots/logs`).

### Debug
To build in debug mode uncomments `# target_compile_definitions(sunspots_daemon PRIVATE DEBUG)` in daemons `CMakeLists.txt` at `Sunspots/src/core/`. This skips over the detaching steps of the daemonizing process, in this mode the daemon can talk.

### Journald entries
All `journald` entries related to this system starts with `"SUNSPOTS"`.
