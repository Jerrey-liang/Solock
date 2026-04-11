# Solock

`Solock` 是一个面向 Windows 的常驻控制程序，用于按时间段自动执行热点维护、音量控制、目标进程断网、锁屏和空闲关机等动作。

当前仓库包含两个主要可执行文件：

- `Solock.Agent.exe`
  常驻代理，负责读取 `config.cfg` 并执行实际控制逻辑。
- `Solock_Configurator.exe`
  WinUI 3 配置器，用于分页编辑配置、校验输入、管理 `Solock.Agent.exe` 进程，并提供主题/语言等界面设置。

## 当前结构

### Agent

`Solock.Agent.exe` 的输出路径固定为：

`D:\C++\Projects\Solock\x64\Release\Solock.Agent.exe`

代理负责：

- 根据时间段切换运行阶段
- 维持系统唤醒
- 维持或切换移动热点
- 按阶段调整系统主音量
- 对目标进程安装或移除 WFP 出站拦截规则
- 在指定空闲条件下执行锁屏、灭屏或关机
- 按需读取 `%LocalAppData%\Solock\config.cfg` 的覆盖配置

### Configurator

`Solock_Configurator.exe` 的默认输出路径是：

`D:\C++\Projects\Solock\x64\Debug\Solock.Configurator\Solock_Configurator.exe`

配置器当前使用分页界面，不再是单个长滚动页面。当前页面如下：

- `Overview`
  显示配置文件路径、原始热点 SSID、表单校验状态、agent 状态和活动消息。
- `Appearance`
  提供主题模式、语言模式、壁纸主题色刷新和窗口材质状态说明。
- `Schedule`
  编辑晚间热点、中午关机窗口和晚间关机时间。
- `Volume`
  编辑正常音量和降低音量目标。
- `Custom Blocks`
  编辑自定义断网时间块。
- `Agent`
  查看发现路径、运行中的进程路径，并执行 agent 管理动作。

`Agent` 页面提供三个操作：

- `Refresh agent status`
  刷新已发现的 agent 路径和当前运行状态。
- `Start Solock.Agent`
  启动 `D:\C++\Projects\Solock\x64\Release\Solock.Agent.exe`。
- `Kill Solock.Agent`
  终止当前运行中的 `Solock.Agent.exe` 进程。

## 配置器界面说明

### Appearance

`Appearance` 页提供：

- `Theme mode`
  支持 `Follow system`、`Light` 和 `Dark`。
- `Language`
  支持 `Follow system`、`English` 和 `Chinese (Simplified)`。
- `Wallpaper theme color`
  从当前桌面壁纸提取主题色，并在失败时回退到系统强调色。
- `Aero background`
  优先启用 Desktop Acrylic，失败时自动回退到 Mica，再失败时使用半透明主题画刷。

界面偏好会保存到：

`%LocalAppData%\Solock\configurator_ui.cfg`

示例：

```ini
theme=system
language=zh-CN
```

### Schedule

`Schedule` 页不再要求手动输入 `HH:MM` 文本。

- 每个时间项都使用 `TimePicker`
- 每个时间项都有 `Use agent default` 复选框
- 选择默认值时，对应配置字段会留空，继续由 agent 使用内建默认时间

### Volume

`Volume` 页不再要求手动输入音量百分比。

- `normal_percent` 和 `reduced_percent` 使用 `Slider`
- 每个滑块都有 `Use agent default` 复选框
- 选择默认值时，对应配置字段会留空

### Custom Blocks

`Custom Blocks` 页不再使用手动拼写的单行文本格式。

- 每个 block 为独立一行
- `start` 使用 `TimePicker`
- `duration_minutes`、`interval_minutes`、`repeat_count` 仍为可选正整数
- 支持增删 block 行
- 加载到非法旧值时，会提示用户重新选择或改回默认

## 构建说明

配置器项目会在构建前自动触发 `Release|x64` 的 agent 构建，因此即使你构建的是 `Debug|x64` 配置器，默认也会先生成：

`D:\C++\Projects\Solock\x64\Release\Solock.Agent.exe`

常用命令：

```powershell
msbuild .\Solock.sln /p:Configuration=Debug /p:Platform=x64 /m
```

如果只想直接构建 agent：

```powershell
msbuild .\Solock\Solock.vcxproj /p:Configuration=Release /p:Platform=x64
```

如果只想直接构建配置器：

```powershell
msbuild .\Solock.Configurator\Solock.Configurator.vcxproj /p:Configuration=Debug /p:Platform=x64 /m
```

## 配置文件

运行时配置文件路径：

`%LocalAppData%\Solock\config.cfg`

格式示例：

```ini
[state]
original_hotspot_ssid=

[schedule]
enable_evening_hotspot=false
midday_shutdown_start=12:10
midday_shutdown_end=12:50
evening_hotspot_start=17:40
evening_shutdown_start=17:50

[volume]
normal_percent=55
reduced_percent=20

[custom_block]
start=19:30
duration_minutes=15
interval_minutes=10
repeat_count=3
```

## 配置字段

### `[state]`

- `original_hotspot_ssid`
  运行态字段，用来记录晚间热点切换前的 SSID。通常不需要手动编辑。

### `[schedule]`

- `enable_evening_hotspot`
  可选，`true` 或 `false`。
- `midday_shutdown_start`
  中午关机窗口开始时间，格式 `HH:MM`。
- `midday_shutdown_end`
  中午关机窗口结束时间，格式 `HH:MM`。
- `evening_hotspot_start`
  启用晚间热点模式时的开始时间，格式 `HH:MM`。
- `evening_shutdown_start`
  禁用晚间热点模式时的晚间关机开始时间，格式 `HH:MM`。

### `[volume]`

- `normal_percent`
  正常阶段目标音量，范围 `0` 到 `100`。
- `reduced_percent`
  中午和晚间降低阶段的目标音量，范围 `0` 到 `100`。

### `[custom_block]`

每个 `[custom_block]` 表示一个自定义断网时间块：

- `start`
  开始时间，格式 `HH:MM`，必填。
- `duration_minutes`
  每段断网持续分钟数，可选。
- `interval_minutes`
  多段断网之间的间隔分钟数，可选。
- `repeat_count`
  总重复次数，可选。

## 运行行为摘要

代理会按当前时间进入不同阶段，并执行对应动作：

- 普通阶段
  维持热点可用，按计划时间窗口断网，音量保持正常值。
- 中午空闲关机阶段
  持续断网，热点保持开启，空闲达到阈值后关机。
- 晚间热点阶段
  切换到晚间热点名，持续断网，空闲达到阈值后锁屏并关闭显示器。
- 晚间空闲关机阶段
  当晚间热点模式关闭时启用，持续断网并在空闲后关机。

代理在心跳周期内会重新读取 `config.cfg`，所以多数配置变更在保存后无需重启 agent 即可生效。

## 权限与注意事项

- 涉及热点控制、WFP 断网、锁屏、关机、计划任务等动作时，需要足够的系统权限。
- `Kill Solock.Agent` 使用强制终止方式结束进程，只应用于当前 agent 管理场景。
- 配置器优先发现并启动 `D:\C++\Projects\Solock\x64\Release\Solock.Agent.exe`。
- 如果磁盘上还残留旧的 `Solock.exe`，配置器会忽略它。

## 相关文件

- `Solock\`
  agent 工程。
- `Solock.Configurator\`
  WinUI 3 配置器工程。
- `README.md`
  当前说明文档。
