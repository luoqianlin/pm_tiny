# PM_Tiny 2.x 到 3.0 配置迁移

3.0 不读取旧 `command` 字段，也不自动转换旧 `.cfg` 文件。daemon、CLI 和 SDK 必须同步升级。

旧配置：

```yaml
- name: api
  cwd: /opt/api
  command: /opt/api/server --listen "0.0.0.0:8080"
```

新配置：

```yaml
- name: api
  cwd: /opt/api
  executable: /opt/api/server
  args:
    - --listen
    - 0.0.0.0:8080
  pty: false
```

迁移时必须按原程序真实 argv 拆分，不能简单按空格切割。特别检查空参数、包含空格的路径、引号、反斜杠和以 `-` 开头的值。缺少 `pty` 时 3.0 默认关闭。
