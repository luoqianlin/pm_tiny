![](images/shot.png)
# PM_Tiny

简体中文 | [English](README_en.md)

## PM_Tiny进程管理工具

PM_Tiny(process manager tiny)是Linux环境下进程管理工具。
能够按照设置的依赖关系启动程序，在进程异常退出时重新启动应用程序，保障服务的可用性。
pm_tiny同时会将应用程序的输出内容重定向到日志文件中，日志文件实现循环覆盖，确保不会因为输出日志太多消耗完存储空间。

**编译后可执行文件极小(总大小不超过1MB),非常适合嵌入式Linux运行。**

### 编译

代码使用c++实现(编译器需支持C++14)。

项目使用cmake构建，请确保系统已安装cmake。

#### Ubuntu编译安装
```shell
$ make build #编译
$ make install_ubuntu #安装
$ systemctl start pm_tiny #启动
```

#### Linux通用平台编译

```shell
$ mkdir build && cd build
$ cmake ..
$ cmake --build . --target pm_tiny --target pm --target pm_sdk
$ cmake --install .
```

#### Hi3559A V100 平台编译

已安装相应的SDK,交叉编译工具链应该位于目录`/opt/hisi-linux/x86-arm`
```shell
$ mkdir hisi_build && cd hisi_build
$ cmake -DCMAKE_TOOLCHAIN_FILE=../toolchains/himix100.toolchain.cmake ..
$ cmake --build . --target pm_tiny --target pm --target pm_sdk
$ cmake --install .
```
#### AX620A平台编译

已安装相应交叉编译工具链,将命令`arm-linux-gnueabihf-gcc`、`arm-linux-gnueabihf-g++`配置到环境变量
```shell
BUILD_DIR=".ax_build"
rm -rf ${BUILD_DIR}
mkdir ${BUILD_DIR} && cd ${BUILD_DIR}
cmake -DCMAKE_TOOLCHAIN_FILE=../toolchains/ax620a.toolchain.cmake ..
cmake --build . --target pm_tiny --target pm --target pm_sdk
cmake --install .
```

### 安装
> Ubuntu环境参照[Ubuntu编译安装](#ubuntu编译安装)说明，无需进行此步骤

拷贝编译产物`build/pm_tiny`,`build/pm`到环境变量路径 ，运行pm_tiny服务(正式环境中应配置pm_tiny为开机自启动)：
```shell
$ pm_tiny -d #-d参数指定以daemon方式运行
```

### 使用

就将程序交给pm_tiny管理:

1、添加程序

```shell
$ pm start "node test.js arg0 arg1" --name app_name #运行app.js并取名为app_name,名称用于后面管理使用，不能重复
Success
Total:1
 ------------------------------------------------------------------------------------------
| pid   | name     | cwd                      | command                 | restart | state  |
 ------------------------------------------------------------------------------------------
| 21864 | app_name | /home/xx/pm_tiny/default |  node test.js arg0 arg1 | 0       | online |
 ------------------------------------------------------------------------------------------

```

可用选项:

`--kill_timeout <seconds>` 停止应用时的超时时间，单位秒，默认值为3；`pm stop`命令执行时，PM_Tiny会首先向应用发送`SIGTERM`信号，如果超过该时间应用还存在将向其发送`SIGKILL`信号。

`--user <user>` 应用程序运行时的用户名，默认为空，空表示与运行PM_Tiny服务的用户一样。

`--depends_on <other_apps>` 依赖的应用程序，必须等依赖的程序启动后，该程序才会启动。不同程序名称使用`,`分隔。

`--start_timeout <seconds>` 应用启动超时时间，单位秒，默认值0；如果为-1表示必须接收到应用程序的ready消息后才会标记为在线状态； 如果大于零，在该值范围内收到应用程序ready消息，则标记为在线，否则将执行`failure_action`指定的动作。

`--failure_action <skip|restart|reboot>`   启动超时和心跳超时执行的动作，其值可以为`skip`、`restart`、`reboot`，默认值为`skip`； 当`start_timeout`>0时使用`skip`可以实现应用启动延时`start_timeout`秒后将其标记为在线；`restart`表示重启应用；`reboot`表示重启系统。

`--heartbeat_timeout <seconds>` 单位秒，默认值-1；小于等于0表示不开启应用程序到PM_Tiny的心跳监测；大于0时，在该时间内如果未收到应用程序的心跳将执行`failure_action`指定的动作。

`--no_daemon` 带有该选项表示不以`daemon`运行，程序退出后不会重启

`--log` 表示运行程序并显示其输出信息


2、查看程序

```shell
$ pm ls 
Total:1
 ------------------------------------------------------------------------------------------
| pid   | name     | cwd                      | command                 | restart | state  |
 ------------------------------------------------------------------------------------------
| 21864 | app_name | /home/xx/pm_tiny/default |  node test.js arg0 arg1 | 0       | online |
 ------------------------------------------------------------------------------------------

```

3、停止程序

```shell
$ pm stop app_name
Success
Total:1
 ----------------------------------------------------------------------------------------
| pid | name     | cwd                      | command                 | restart | state  |
 ----------------------------------------------------------------------------------------
| -1  | app_name | /home/xx/pm_tiny/default |  node test.js arg0 arg1 | 0       | stoped |
 ----------------------------------------------------------------------------------------

```

4、运行程序

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

可以选择添加`--log`选项,表示运行程序并显示其输出信息

5、查看程序输出

```shell
$ pm log app_name
```

6、查看程序配置

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


7、删除程序

```shell
$ pm delete app_name
Success
Total:0
 ----------------------------------------------
| pid | name | cwd | command | restart | state |
 ----------------------------------------------
```

8、持久化启动配置，pm_tiny服务启动后运行在配置中的程序(配置默认保存在`~/.pm_tiny/prog.cfg`文件中)

```shell
$ pm save
Success
```

### 配置

pm_tiny默认家目录(PM_TINY_HOME)为`~/.pm_tiny`,也可以通过修改环境变量`PM_TINY_HOME`指定家目录。

应用程序的输出日志默认在`${PM_TINY_HOME}/logs`目录下

配置文件位于`${PM_TINY_HOME}/prog.yaml`

一个配置文件例子：

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

- name: web_service             # 应用名称，必须唯一。
  cwd: /opt/app                 # 工作目录。
  command: ./server             # 启动命令。
  kill_timeout_s: 3             # 停止应用时的超时时间，单位秒，默认值为3；stop命令执行时，PM_Tiny会首先向应用发送SIGTERM信号，
                                # 如果超过该时间应用还存在将向其发送SIGKILL信号。
  
  user: ""                      # 应用程序运行时的用户名，默认为空，空表示与运行PM_Tiny服务的用户一样。
  depends_on:                   # 依赖的应用程序，必须等依赖的程序启动后，该程序才会启动。
   - hardware_service
   - ai_service 
  start_timeout: 1              # 应用启动超时时间，单位秒，默认值0；如果为-1表示必须接收到应用程序的ready消息后才会标记为在线状态；
                                # 如果大于零，在该值范围内收到应用程序ready消息，则标记为在线，否则将执行failure_action指定的动作。

  failure_action: skip          # 启动超时和心跳超时执行的动作，其值可以为skip、restart、reboot，默认值为skip；
                                # 当start_timeout>0时使用skip可以实现应用启动延时start_timeout秒
                                # 后将其标记为在线；restart表示重启应用；reboot表示重启系统。
  
  daemon: true                  # 其中可以为true、false，默认值true；当为true时，程序退出后自动将其重启（不再需要退出码为非0的要求）；
                                # 为false将不再重启程序。
  
  heartbeat_timeout: -1         # 单位秒，默认值-1；小于等于0表示不开启应用程序到PM_Tiny的心跳监测；
                                # 大于0时，在该时间内如果未收到应用程序的心跳将执行failure_action指定的动作。
```

配置文件配置了三个服务hardware_service、ai_service、web_service。其中ai_service依赖于hardware_service，也就是说要等hardware_service启动后ai_service才能启动。
web_service启动依赖于hardware_service和ai_service。

启动依赖只在pm_tiny服务启动过程中有效（如系统重启后，pm_tiny读取配置文件启动）。手动启停应用启动依赖无效。如果在运行过程中修改了配置文件可以使用`pm reload`重新载入配置，`pm_tiny`将停止所有应用，然后按照依赖配置启动程序。



### 应用重启策略

应用程序配置为daemon模式，运行时长超过100ms，满足此条件的程序退出后，pm_tiny才会重启程序。


