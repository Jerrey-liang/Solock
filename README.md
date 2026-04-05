## 本项目使用 Vibe Coding

作者接受在自用项目中高比例使用 AI 生成代码、配置与文档。   
仅适用于自用场景、维护成本与风险由个人承担，不应被表述为通用工程标准。

# Solock

`Solock` 是一个面向 Windows 的自动化常驻控制程序。它会根据一天中的不同时间段，持续执行以下策略：

- 阻止系统因空闲而休眠
- 监听默认音频输出设备变化，并把系统音量拉回目标值
- 维持 Windows 移动热点可用
- 在指定时间段内对目标进程安装 WFP 出站拦截规则
- 支持从本地配置文件覆盖调度、晚间热点开关、多个自定义断网时间窗和音量
- 在午间空闲达到阈值时立即关机
- 在晚间可按配置选择“空闲锁屏并关闭显示器”或“空闲立即关机”
- 启动时为当前用户注册“登录后自动启动”的计划任务

## 功能概览

### 常驻循环

进入常驻后，程序会按 `heartbeatSeconds` 的节奏循环执行：

- 重新计算当前时间阶段
- 调用 `SetThreadExecutionState` 保持系统唤醒
- 把系统主音量调整到该阶段目标值
- 根据阶段执行热点、断网、关机、锁屏等动作
- 等待下一次心跳

如果默认音频输出设备发生变化，循环会被提前唤醒，不必等完整心跳周期才重新设置音量。

## 时间阶段

程序有四种运行阶段，其中两种晚间阶段会根据 `enable_evening_hotspot` 二选一：

| 阶段 | 默认时间 | 行为 |
| --- | --- | --- |
| `ScheduledBlocks` | 除午间和晚间外的其他时段 | 保持热点可用；按预设时间窗对目标进程断网；额外检查自定义断网时间窗；音量保持正常值 |
| `MiddayIdleShutdown` | `12:10` - `12:50` | 保持热点可用；持续对目标进程断网；空闲达到阈值后立即关机；音量降到较低值 |
| `EveningIdleShutdown` | `17:50` 之后，且 `enable_evening_hotspot=false` | 行为与午间空闲关机相同：保持热点可用；持续对目标进程断网；空闲达到阈值后立即关机；音量降到较低值 |
| `EveningPostAction` | `17:40` 之后，且 `enable_evening_hotspot=true` | 持续对目标进程断网；将热点切换到随机化晚间别名；空闲达到阈值后锁屏并关闭显示器；锁屏未回到桌面期间静音，解锁回到桌面后恢复晚间目标音量 |

阶段判断优先级如下：

1. 如果 `enable_evening_hotspot=true` 且当前时间大于等于晚间热点开始时间，进入 `EveningPostAction`。
2. 如果 `enable_evening_hotspot=false` 且当前时间大于等于晚间空闲关机开始时间，进入 `EveningIdleShutdown`。
3. 否则，如果当前时间位于午间窗口内，进入 `MiddayIdleShutdown`。
4. 其余时间进入 `ScheduledBlocks`。

## 默认配置

默认值定义在 `SolockController::Options` 中；`%LocalAppData%\\Solock\\config.cfg` 还可以在运行时覆盖其中一部分调度和行为。

### 调度与节奏

| 配置项 | 默认值 | 说明 |
| --- | --- | --- |
| `startupStableSeconds` | `8` | 启动时要求网络连续稳定的秒数 |
| `startupMaxWaitSeconds` | `45` | 启动阶段等待网络稳定的最长秒数 |
| `scheduledBlockStartMinutesOfDay` | `08:40, 09:30, 10:20, 11:10, 15:10, 16:00, 16:50` | 内置定时断网开始时间点 |
| `scheduledBlockDurationMinutes` | `10` | 每个内置定时断网窗口持续时间 |
| `middayShutdownStartHour:Minute` | `12:10` | 午间空闲关机窗口开始时间 |
| `middayShutdownEndHour:Minute` | `12:50` | 午间空闲关机窗口结束时间 |
| `eveningPostActionStartHour:Minute` | `17:40` | 晚间后置阶段开始时间 |
| `eveningIdleShutdownStartHour:Minute` | `17:50` | 晚间热点关闭时的空闲关机开始时间 |
| `inactivityThresholdMinutes` | `10` | 判定空闲的阈值 |
| `heartbeatSeconds` | `15` | 主循环心跳周期 |

### 音量

| 配置项 | 默认值 | 说明 |
| --- | --- | --- |
| `normalVolumePercent` | `60` | `ScheduledBlocks` 阶段目标音量 |
| `reducedVolumePercent` | `35` | 午间和晚间阶段目标音量 |

### 热点与目标进程

| 配置项 | 默认值 | 说明 |
| --- | --- | --- |
| `postActionSsid` | `CMCC-24dF` | 晚间热点随机别名的后备来源 SSID |
| `blockedProcessNames` | `SeewoHugoLauncher.exe`, `SeewoServiceAssistant.exe` | 需要受联网控制的进程名 |

### 自启动

| 配置项 | 默认值 | 说明 |
| --- | --- | --- |
| `autoRegisterScheduledTask` | `true` | 启动时自动注册计划任务 |
| `scheduledTaskName` | `Solock AutoStart` | 计划任务名称 |

## 外部配置文件

除了 `Options` 中的内置默认值，程序还会在运行时读取并维护一个本地 INI 风格配置文件：

`%LocalAppData%\Solock\config.cfg`

文件格式如下：

```ini
[state]
original_hotspot_ssid=

[schedule]
enable_evening_hotspot=
midday_shutdown_start=
midday_shutdown_end=
evening_hotspot_start=
evening_shutdown_start=

[volume]
normal_percent=
reduced_percent=

[custom_block]
start=
duration_minutes=
interval_minutes=
repeat_count=

[custom_block]
start=
duration_minutes=
interval_minutes=
repeat_count=
```

一个更完整的示例：

```ini
[state]
original_hotspot_ssid=

[schedule]
enable_evening_hotspot=false
midday_shutdown_start=12:10
midday_shutdown_end=12:50
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

字段说明：

- `[state] original_hotspot_ssid`
  由程序维护的运行时状态，用于保存“晚间切换前的原始 SSID”，供白天阶段恢复。通常不需要手动编辑。
- `[schedule] enable_evening_hotspot`
  可选。是否启用晚间热点模式。默认 `true`。为 `false` 时，晚间阶段不再切热点别名，也不再执行锁屏灭屏，而是改为“持续断网 + 空闲关机”。
- `[schedule] midday_shutdown_start`
  可选。午间空闲关机窗口开始时刻，使用 24 小时制 `HH:MM`。
- `[schedule] midday_shutdown_end`
  可选。午间空闲关机窗口结束时刻，使用 24 小时制 `HH:MM`。
- `[schedule] evening_hotspot_start`
  可选。开启晚间热点模式时，`EveningPostAction` 的开始时刻，使用 24 小时制 `HH:MM`。
- `[schedule] evening_shutdown_start`
  可选。关闭晚间热点模式时，`EveningIdleShutdown` 的开始时刻，使用 24 小时制 `HH:MM`。默认 `17:50`。
- `[volume] normal_percent`
  可选。覆盖 `ScheduledBlocks` 阶段的目标音量百分比，范围会被限制到 `0` - `100`。
- `[volume] reduced_percent`
  可选。覆盖 `MiddayIdleShutdown`、`EveningIdleShutdown` 和 `EveningPostAction` 阶段的目标音量百分比，范围会被限制到 `0` - `100`。
- `[custom_block] start`
  一个自定义断网时间窗的开始时刻，使用 24 小时制 `HH:MM`。
- `[custom_block] duration_minutes`
  每一段断网窗口持续的分钟数，可选。
- `[custom_block] interval_minutes`
  本次断网操作执行完毕后，到下一次断网开始前的间隔分钟数，可选。留空时默认视为 `0`，也就是下一段紧接着上一段结束后立即开始。
- `[custom_block] repeat_count`
  总共执行几段相同持续时间的断网窗口，可选。

自定义断网时间窗的规则如下：

1. `[schedule]`、`[volume]` 和 `[state]` 是普通 section，各键最后一次出现的值生效。
2. 可以写入若干个 `[custom_block]` section；程序会按出现顺序读取，并把它们视为并集。
3. 每个 `[custom_block]` 的 `start` 是唯一必填项。
4. 每个 `[custom_block]` 都是“当前进程生命周期内”的自定义窗口，不会按天自动重置；如需重新安排，可以修改配置文件或重启进程。
5. 如果某个 `[custom_block]` 的 `duration_minutes` 为空，则一旦本次进程运行到它的 `start`，目标进程会持续断网直到本次进程生命周期结束，或配置文件被修改后重新判定。
6. 如果填写了 `duration_minutes`，但 `repeat_count` 为空，则只执行 1 段该持续时间的断网窗口。
7. 如果同时填写了 `duration_minutes` 和 `repeat_count`，则每段断网都持续 `duration_minutes` 分钟，总段数为 `repeat_count`。
8. 如果 `interval_minutes` 也填写了，则每一段断网结束后会先等待 `interval_minutes` 分钟，再开始下一段断网。
9. 如果填写了 `duration_minutes` 和 `repeat_count`，但 `interval_minutes` 为空，则各段断网会连续拼接，行为与旧版本一致。
10. 如果只填写了 `repeat_count` 或 `interval_minutes`，而没有填写 `duration_minutes`，这些附加参数都不生效，行为仍等同于“从 `start` 断网到本次进程生命周期结束”。

程序会在心跳周期内重新读取该文件，因此你在程序运行中修改它，后续循环会自动使用新值。

## 断网机制

### 工作方式

程序使用 Windows Filtering Platform（WFP）给目标应用安装动态出站拦截规则：

- 打开动态 WFP session
- 注册自定义 sublayer
- 用 `FwpmGetAppIdFromFileName0` 获取可执行文件的 `ALE_APP_ID`
- 分别在 `ALE_AUTH_CONNECT_V4` 和 `ALE_AUTH_CONNECT_V6` 层安装 `FWP_ACTION_BLOCK`

### 内置定时断网时间窗

在 `ScheduledBlocks` 阶段，默认内置断网窗口为：

- `08:40` - `08:50`
- `09:30` - `09:40`
- `10:20` - `10:30`
- `11:10` - `11:20`
- `15:10` - `15:20`
- `16:00` - `16:10`
- `16:50` - `17:00`

如果当前时间落在这些窗口内，则安装拦截规则；否则清理动态规则，恢复联网能力。

### 自定义断网时间窗

在 `ScheduledBlocks` 阶段，程序还会额外检查 `%LocalAppData%\Solock\config.cfg` 中全部 `[custom_block]` 配置。
如果某个 `[custom_block]` 同时设置了 `duration_minutes`、`interval_minutes` 和 `repeat_count`，程序会按“断网一段 -> 等待一段 -> 再断网一段”的顺序逐段判断是否命中。

自定义断网时间窗与内置时间窗是“并集”关系：

- 只要任意一个内置时间窗命中，就会断网
- 只要任意一个自定义时间窗处于激活状态，也会断网

在以下阶段中，目标进程会被持续断网，而不依赖内置窗口或自定义窗口：

- `MiddayIdleShutdown`
- `EveningIdleShutdown`
- `EveningPostAction`

### 目标进程解析规则

程序会遍历系统进程快照，只关心文件名匹配 `blockedProcessNames` 的进程，然后：

- 通过 `OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)` 打开进程
- 通过 `QueryFullProcessImageNameW` 解析完整路径
- 以完整路径作为 WFP 过滤对象

几个重要细节：

- 只有当前已经运行、并成功解析出完整路径的目标进程，才会被纳入本轮拦截集合。
- 如果同一路径已经装过过滤器，后续从该路径再次启动的新实例仍会被拦截，直到规则被清理。
- 如果上一轮已经装过过滤器，而这一轮暂时没再发现进程，程序会保留现有规则，避免目标程序短暂重启时逃逸联网。

### 规则清理

WFP 规则依附于动态 session，程序会在以下时机清理：

- 启动初期先执行一次“确保可联网”
- 离开断网窗口且当前不再需要断网时
- 控制器析构时
- WFP 单独调试流程结束时

## 热点管理

### 白天阶段

在普通阶段、午间阶段以及关闭晚间热点后的晚间空闲关机阶段，程序执行“前置热点保障”：

- 如果本地 `config.cfg` 的 `[state]` 里记录过原始 SSID，并且当前 SSID 与之不同，则先尝试恢复原始 SSID
- 如果当前 SSID 已经等于记录值，则清理该状态项
- 最终确保热点处于开启状态

### 晚间阶段

仅当 `[schedule] enable_evening_hotspot=true` 时，晚间阶段才会使用随机运营商风格别名。随机别名逻辑会：

1. 尽量保存“切换前的原始 SSID”。
2. 以原始 SSID 为优先来源；如果拿不到，则退回当前 SSID；再不行才退回 `postActionSsid`。
3. 基于这个来源字符串生成一个随机化的“运营商风格”热点别名。
4. 把热点切到该别名并确保热点开启。

随机别名的前缀会从以下候选中随机选一个：

- `CMCC-`
- `ChinaNet-`
- `ChinaUnicom-`
- `CUNET_`
- `TP-LINK_`
- `MERCURY_`
- `H3C_`
- `Tenda_`

后缀会从来源 SSID 中抽取字母数字字符并随机组合。如果来源 SSID 以 `seewo-` 开头，生成时会优先使用去掉此前缀后的内容。

### 密码处理

切换 SSID 时会复用当前热点密码。如果读取到的密码长度不足 8 位，则回退为 `12345678`，以满足热点配置要求。

### 状态文件

程序现在只使用一个本地文件：

- `%LocalAppData%\Solock\config.cfg`
  同时承载运行时状态和用户配置。其中 `[state] original_hotspot_ssid` 用于保存“晚间切换前的原始 SSID”，`[schedule]` 用于覆盖调度与晚间热点开关，`[volume]` 用于覆盖目标音量，`[custom_block]` 用于定义一个或多个自定义断网时间窗。

如果本地还残留旧版 `%LocalAppData%\Solock\hotspot_and_block.ini` 或 `%LocalAppData%\Solock\original_hotspot_ssid.txt`，程序会在读取到它们时自动迁移到 `config.cfg`。

## 音量控制

程序会监听默认音频输出设备，并把系统主音量维持在目标值：

- `ScheduledBlocks`：`60%`
- `MiddayIdleShutdown`：`35%`
- `EveningIdleShutdown`：`35%`
- `EveningPostAction`：`35%`

如果 `%LocalAppData%\Solock\config.cfg` 中存在：

```ini
[volume]
normal_percent=60
reduced_percent=35
```

则会优先使用这里的值覆盖内置默认值。

仅在 `EveningPostAction` 阶段下，如果 Windows 当前处于锁屏且尚未回到桌面，程序会把系统切到静音；当 Windows 解锁并重新进入桌面后，程序会自动取消静音并恢复到 `reduced_percent`（未配置时回退到 `reducedVolumePercent`）。

实现方式：

- 使用 `IMMDeviceEnumerator` 获取默认渲染设备
- 使用 `IAudioEndpointVolume` 读取和设置系统主音量
- 使用自定义 `IMMNotificationClient` 监听默认输出设备变化
- 设备变化时通过事件提前唤醒主循环

如果当前没有默认音频输出设备，程序会把它视为“无需调整”，而不是直接报错退出。

## 空闲检测、关机与锁屏

### 空闲检测

程序通过 `GetLastInputInfo()` 和 `GetTickCount64()` 计算用户空闲时间，默认阈值为 10 分钟。

### 午间空闲关机

在午间空闲关机窗口内，默认是 `12:10` - `12:50`，也可以通过 `[schedule] midday_shutdown_start` 和 `[schedule] midday_shutdown_end` 覆盖：

- 目标进程持续断网
- 热点持续保持可用
- 一旦检测到连续空闲达到阈值，就立即关机

关机实现为无限重试：

1. 先申请 `SE_SHUTDOWN_NAME` 权限，并调用 `ExitWindowsEx(EWX_POWEROFF | EWX_FORCEIFHUNG, ...)`
2. 如果失败，回退到 `shutdown.exe /s /f /t 0 /d p:0:0`
3. 如果仍失败，每秒继续重试直到成功

### 关闭晚间热点时的晚间空闲关机

当 `[schedule] enable_evening_hotspot=false` 时，在晚间空闲关机开始时间之后，默认是 `17:50` 之后：

- 目标进程持续断网
- 热点持续保持可用
- 一旦检测到连续空闲达到阈值，就立即关机

### 开启晚间热点时的晚间空闲锁屏与灭屏

当 `[schedule] enable_evening_hotspot=true` 时，在晚间热点开始时间之后，默认是 `17:40` 之后：

- 目标进程持续断网
- 热点持续保持为晚间随机别名
- 如果连续空闲达到阈值，则调用 `LockWorkStation()` 锁屏
- 锁屏约 1.2 秒后广播 `SC_MONITORPOWER` 关闭显示器
- 在锁屏且显示器关闭、尚未重新回到桌面期间，系统音量会被切到静音
- 当 Windows 解锁并重新进入桌面后，程序会自动恢复到夜间目标音量

这个动作在同一轮晚间阶段里只会成功执行一次；离开晚间阶段后，该标记会被清零。

## 保持系统唤醒

每轮循环都会调用：

`SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED)`

用于阻止系统因空闲而进入睡眠。程序退出时会调用：

`SetThreadExecutionState(ES_CONTINUOUS)`

恢复默认状态。

## 启动时的网络稳定等待

仅当程序在启动时处于 `ScheduledBlocks` 阶段，才会执行网络稳定等待：

- 每秒检查一次当前连接是否达到 `InternetAccess`
- 需要连续稳定 `8` 秒
- 最长等待 `45` 秒
- 只要中途某一秒网络不可用，稳定计数就会清零重新累计

这个等待不会阻止程序先做基础初始化，只是用于避免刚开机或网络切换中的误判。

## 自动注册计划任务

默认情况下，程序启动时会创建或更新计划任务：

- 任务名：`Solock AutoStart`
- 触发器：当前用户登录时
- 运行身份：当前交互登录用户
- 运行级别：最高权限
- 多实例策略：忽略新实例
- 允许按需启动：是
- 不限制执行时间：`PT0S`
- 动作：启动当前 EXE，并把工作目录设为 EXE 所在目录

### 工程说明

- `Debug|x64` 为控制台子系统，并显式要求 `RequireAdministrator`
- `Release|x64` 为 Windows 子系统，入口符号为 `mainCRTStartup`

### 代码结构

- `main.cpp`
  进程入口、WinRT 初始化、顶层异常兜底。
- `SolockController.h`
  控制器对外接口、配置项定义、私有状态与成员函数声明。
- `SolockController.cpp`
  控制器生命周期、调度阶段判定、主循环、启动期网络稳定等待。
- `SolockControllerAudio.cpp`
  默认音频设备监听、音量同步、晚间静音与恢复。
- `SolockControllerHotspot.cpp`
  热点开启、SSID 切换、晚间随机别名生成。
- `SolockControllerNetworkBlock.cpp`
  WFP 动态过滤器安装与清理、目标进程解析、断网时序匹配。
- `SolockControllerConfig.cpp`
  本地状态目录、`config.cfg` 读写、调度覆盖、外部音量覆盖、自定义断网窗口解析。
- `SolockControllerSystem.cpp`
  锁屏、灭屏、关机、开机自启动计划任务注册。
- `SolockControllerInternal.h/.cpp`
  仅供内部复用的共享工具、大小写比较、调试输出、COM 释放辅助。

## 需要注意的实际行为

1. Release 模式下会真实执行计划任务注册、热点控制、WFP 断网、关机、锁屏和灭屏。
2. 晚间热点逻辑只保留内置随机运营商风格别名，不再支持通过配置文件指定固定热点名。
3. 调度覆盖、晚间热点开关、自定义断网时间窗和音量覆盖都是可选的，并且都由 `%LocalAppData%\Solock\config.cfg` 控制。
4. 内置默认策略仍然写在 `SolockController::Options` 中，但运行时原始 SSID 状态、调度覆盖、外部音量覆盖和多个自定义断网窗口现在都放在同一个外部 INI 风格文件里。
5. 如果程序未以足够权限运行，文档描述的部分系统级动作可能无法成功。
