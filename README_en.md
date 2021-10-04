![](images/shot.png)

# PM_Tiny

[简体中文](README.md) | English

## PM_Tiny Process Management Tool

PM_Tiny (process manager tiny) is a process management tool for the Linux environment. It can start programs based on set dependencies and will restart applications if they exit unexpectedly, ensuring service availability. pm_tiny also redirects the output content of applications to log files. The log files implement cyclic overwriting to ensure that storage space is not consumed by excessive log outputs.

**The compiled executable file is very small (total size does not exceed 1MB), making it highly suitable for embedded Linux operation.**

### Compilation

The code is implemented in C++ (compiler must support C++14).

The project uses cmake for building. Ensure cmake is installed on your system.

#### Compilation and Installation on Ubuntu
```shell
$ make build #Compile
$ make install_ubuntu #Install
$ systemctl start pm_tiny #Start
```

#### General Linux Platform Compilation

```shell
$ mkdir build && cd build
$ cmake ..
$ cmake --build . --target pm_tiny --target pm --target pm_sdk
$ cmake --install .
```

#### Compilation for Hi3559A V100 Platform

Ensure the corresponding SDK is installed. Cross-compilation toolchain should be located in the directory `/opt/hisi-linux/x86-arm`.
```shell
$ mkdir hisi_build && cd hisi_build
$ cmake -DCMAKE_TOOLCHAIN_FILE=../toolchains/himix100.toolchain.cmake ..
$ cmake --build . --target pm_tiny --target pm --target pm_sdk
$ cmake --install .
```

#### Compilation for AX620A Platform

Ensure the corresponding cross-compilation toolchain is installed. Add the commands `arm-linux-gnueabihf-gcc` and `arm-linux-gnueabihf-g++` to the environment variables.
```shell
BUILD_DIR=".ax_build"
rm -rf ${BUILD_DIR}
mkdir ${BUILD_DIR} && cd ${BUILD_DIR}
cmake -DCMAKE_TOOLCHAIN_FILE=../toolchains/ax620a.toolchain.cmake ..
cmake --build . --target pm_tiny --target pm --target pm_sdk
cmake --install .
```

### Installation
> For the Ubuntu environment, refer to [Compilation and Installation on Ubuntu](#compilation-and-installation-on-ubuntu). This step is not required for it.

Copy the compiled files `build/pm_tiny` and `build/pm` to the environment variable path. Run the pm_tiny service (in a production environment, pm_tiny should be set to start on boot):
```shell
$ pm_tiny -d # -d option runs in daemon mode
```

### Usage

To manage a program with pm_tiny:

1. Add a program:

```shell
$ pm start "node test.js arg0 arg1" --name app_name #Run app.js, name it as app_name. This name is used for subsequent management and must be unique.
Success
Total:1
 ------------------------------------------------------------------------------------------
| pid   | name     | cwd                      | command                 | restart | state  |
 ------------------------------------------------------------------------------------------
| 21864 | app_name | /home/xx/pm_tiny/default |  node test.js arg0 arg1 | 0       | online |
 ------------------------------------------------------------------------------------------
```

Available options include:

`--kill_timeout <seconds>` Timeout for stopping the application, in seconds, default is 3. When the `pm stop` command is executed, PM_Tiny first sends a `SIGTERM` signal to the application. If the app remains running beyond this timeout, a `SIGKILL` signal is sent.

`--user <user>` The username under which the application runs. Default is empty, which means it runs as the same user as the PM_Tiny service.

`--depends_on <other_apps>` Applications that the current application depends on. This application will only start after its dependencies have started. Different program names are separated by commas.

`--start_timeout <seconds>` Application start timeout in seconds. Default is 0. If -1, the application must send a ready message before being marked online. If greater than zero, the application will be marked online within this time if a ready message is received; otherwise, the action specified by `failure_action` will be executed.

`--failure_action <skip|restart|reboot>` Actions to execute upon start timeout and heartbeat timeout. Valid values are `skip`, `restart`, and `reboot` with the default being `skip`. Using `skip` with `start_timeout`>0 means the application will be marked online after a delay of `start_timeout` seconds. `restart` will restart the application, while `reboot` will reboot the system.

`--heartbeat_timeout <seconds>` Default is -1 seconds. Values less than or equal to 0 mean no heartbeat monitoring from the application to PM_Tiny. If greater than 0, the specified action by `failure_action` will be executed if no heartbeat is received from the application within this time.

`--no_daemon` Running with this option means it won't run in `daemon` mode and won't restart after the program exits.

`--log` Indicates that the program should run and display its output.

2. View a program:

```shell
$ pm ls 
Total:1
 ------------------------------------------------------------------------------------------
| pid   | name     | cwd                      | command                 | restart | state  |
 ------------------------------------------------------------------------------------------
| 21864 | app_name | /home/xx/pm_tiny/default |  node test.js arg0 arg1 | 0       | online |
 ------------------------------------------------------------------------------------------
```

3. Stop a program:

```shell
$ pm stop app_name
Success
Total:1
 ----------------------------------------------------------------------------------------
| pid | name     | cwd                      | command                 | restart | state  |
 ----------------------------------------------------------------------------------------
| -1  | app_name | /home/xx/pm_tiny/default |  node test.js arg0 arg1 | 0       | stopped|
 ----------------------------------------------------------------------------------------
```

4. Run a program:

```shell
$ pm start app_name
Success
Total:1
 ------------------------------------------------------------------------------------------
| pid   | name     | cwd                      | command                 | restart | state  |
 ------------------------------------------------------------------------------------------
| 22047 | app_name | /home/xx/pm_tiny/default |  node test.js arg0 arg1 | 0       | online |
 ------------------------------------------------------------------------------------------
```

You can optionally add the `--log` option, which means the program will run and display its output.

5. View program output:

```shell
$ pm log app_name
```

6. View program configuration:

```shell
$ pm inspect app_name
+-------------------+----------------------------------+
| name              | app_name                         |
+-------------------+----------------------------------+
| cwd               | /home/xx/pm_tiny/default         |
+-------------------+----------------------------------+
| command           |  node test.js arg0 arg1          |  
+-------------------+----------------------------------+
| user              | root                             |
+-------------------+----------------------------------+
| daemon            | Y                                |
+-------------------+----------------------------------+
| depends_on        |                                  |
+-------------------+----------------------------------+
| start_timeout     | available immediately            |
+-------------------+----------------------------------+
| failure_action    | skip                             |
+-------------------+----------------------------------+
| heartbeat_timeout | disable                          |
+-------------------+----------------------------------+
| kill_timeout      | 3s                               |
+-------------------+----------------------------------+
```

7. Delete a program:

```shell
$ pm delete app_name
Success
Total:0
 ----------------------------------------------
| pid | name | cwd | command | restart | state |
 ----------------------------------------------
```

8. Persist startup configuration so that pm_tiny starts the programs listed in the configuration upon service startup (configuration is saved in the `~/.pm_tiny/prog.cfg` file by default):

```shell
$ pm save
Success
```

### Configuration

The default home directory for pm_tiny (PM_TINY_HOME) is `~/.pm_tiny`. You can also specify a different home directory by modifying the `PM_TINY_HOME` environment variable.

The application's output logs are saved in the `${PM_TINY_HOME}/logs` directory by default.

The configuration file is located at `${PM_TINY_HOME}/prog.yaml`.

Here's an example configuration file:

```yaml
- name: hardware_service
  cwd: /opt/hardware
  command: ./hardware_server
  kill_timeout_s: 3
  user: ""
  depends_on:
    []
  start_timeout: 2
  failure_action: skip
  daemon: true
  heartbeat_timeout: -1

- name: ai_service
  cwd: /opt/algo
  command: ./ai_server
  kill_timeout_s: 10
  user: ""
  depends_on:
    - hardware_service
  start_timeout: 5
  failure_action: skip
  daemon: true
  heartbeat_timeout: -1

- name: web_service   # Application name; must be unique.
  cwd: /opt/app       # Working directory.
  command: ./server   # Startup command.
  
  kill_timeout_s: 3   # Timeout for stopping the application, in seconds. 
                      # Default value is 3. When executing the 'stop' command,
                      # PM_Tiny will first send a SIGTERM signal to the application. 
                      # If the application remains running beyond this timeout, a SIGKILL signal will be sent to it.
  
  user: ""            # Username under which the application runs. 
                      # Default is empty, which means the application runs as the same user as the PM_Tiny service.
  
  depends_on:         # Applications on which this one depends.
                      # This application will start only after its dependencies have started.
    - hardware_service
    - ai_service
  start_timeout: 1    # Application start timeout in seconds. 
                      # Default value is 0. If set to -1, 
                      # it means the application must send a 'ready' message before being marked as online. 
                      # If greater than zero and a 'ready' message from the application is received within this time,
                      # it will be marked as online; otherwise, the action specified by 'failure_action' will be taken.
  
  failure_action: skip # Action to take upon startup timeout and heartbeat timeout. 
                      # Valid values are 'skip', 'restart', and 'reboot' with default being 'skip'.
                      # If 'start_timeout' > 0, using 'skip' means the application will be marked online 
                      # after a delay of 'start_timeout' seconds. 
                      # 'restart' means the application will be restarted; 'reboot' means the system will be rebooted.
  
  daemon: true        # Valid values are true or false, with default being true. 
                      # When set to true, the program will be automatically restarted 
                      # if it exits (no requirement for non-zero exit code). 
                      # If set to false, the program won't be restarted.
  
  heartbeat_timeout: -1 # Specified in seconds. Default value is -1. 
                        # Values less than or equal to 0 mean no heartbeat monitoring from the application to PM_Tiny. 
                        # If greater than 0 and no heartbeat from the application is received within this time, 
                        # the action specified by 'failure_action' will be executed.

```

The configuration file configures three services: hardware_service, ai_service, and web_service. ai_service depends on hardware_service, meaning hardware_service must start before ai_service can. web_service depends on both hardware_service and ai_service.

The startup dependency is only effective during the pm_tiny service startup process (for example, after a system reboot when pm_tiny reads the configuration file to start). Manual start/stop of applications doesn't consider startup dependencies. If the configuration file is modified during operation, you can use `pm reload` to reload the configuration. `pm_tiny` will stop all applications and then start programs based on the dependency configuration.

### Application Restart Strategy

When an application is configured in daemon mode and runs for more than 100ms, pm_tiny will restart the program after it exits if these conditions are met.