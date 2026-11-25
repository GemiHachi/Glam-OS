# G'lam OS 更新日志 (Changelog)

> 本文档记录 **G'lam OS** 各主要版本的迭代更新内容（从 GitHub 上一版本 `e4dc7e9` / `v2.0.0` 延续至当前最新工程版本）。  
> 最新版本核心围绕 **Windows PC 双通道 MCP 双向桥接（17 项工具）、SolidWorks 联动、中英双语系统（i18n）** 以及 **小智语音状态机 / 本地 VAD / WebSocket 稳定性治理** 展开。

---

## v2.1.0（2026-11-25）— PC 双向控制桥接（17 项 MCP 工具）、SolidWorks 联动、中英双语系统与语音稳定性升级

### 一、Windows PC 双通道 MCP 控制桥接（扩展至 17 项工具）
- **USB 串口 + Wi-Fi HTTP 双通道桥接（`xz_mcp.c` / `xz_mcp.h` / `os_spk_installer.c`）**：
  - 新增 USB 串口实时信令（`pc_hello` / `pc_hb` / `pc_mcp_req` / `pc_mcp_resp` / `xz_diag`）与 Wi-Fi HTTP 轮询回调通道（`GET /api/pc_mcp/poll`、`POST /api/pc_mcp/reply`），无需固定连接方式即可自动保持 PC 在线绑定。
  - 将小智设备端 MCP 能力由 13 项扩展至 **17 项工具**，新增 4 项桌面协同工具：
    - `pc.control`：直接执行应用启停、窗口聚焦、网页打开、键鼠按键、音量/媒体与 Shell 等快捷操作；
    - `pc.ai_agent`：调用 PC 端视觉 Computer Use 子代理执行点击视频/按钮/链接与多步 GUI 自动化；
    - `pc.ext_mcp`：通过 `list` / `call` 发现并调用 PC 端挂载的外部 Stdio MCP 服务器工具；
    - `pc.solidworks`：直连 Windows SolidWorks 2025 COM 接口执行参数化建模与 `ai_model` 自由建模（支持 `get_status`、`new_part`、`create_box`、`create_cylinder`、`create_tube`、`view`、`rebuild`、`save`、`export`、`ai_model`）。
  - 支持 `tools/list` 按 `withUserTools` 请求按需下发系统特权工具，精简核心工具 Schema 描述以适配云端模型调用。
- **PC 操作三级安全权限模型（`xz_service.h` / `os_ui.c` / `os_xz_view.c`）**：
  - 新增 `XZ_PC_PERM_FULL`（全自动放行）、`XZ_PC_PERM_PARTIAL`（敏感操作拦截确认）、`XZ_PC_PERM_ASK_EVERY`（每次弹窗/口头确认）三级模式，可在设备端设置实时切换。
- **Schema 与超时对齐优化**：
  - 将 `pc.solidworks` 的管件内径参数由 `id` 更名为 `inner_dia`，避免与 JSON Schema `$id` 关键字冲突，并将数值维度参数保持 `"number"` 类型以支持小数毫米输入。
  - 优化 `forward_pc_tool_call` 工具转发等待超时为 `14s`，对齐云端会话超时窗口，防止长任务拖死 WebSocket 语音通道。

### 二、语音交互状态机、本地 VAD 与 WebSocket 稳定性治理
- **消除「说话后随机不回复」状态机死锁（`xz_service.c`）**：
  - 移除 `s_waiting_server_reply` 等待回复期间检测到噪声仅清除等待标志却未重置 `s_vad_stop_sent` / 未重发 `listen/start` 的竞态缺陷；增加 `s_mcp_tool_running` 标记，防止 MCP 工具执行期间误触发 5 秒无回复恢复超时，并在断线、`goodbye` 及退出语音浮层时同步清零复位。
- **本地 VAD 鲁棒性调优**：
  - 优化连续语音触发与尾音归零策略（需连续 3 帧有效能量触发、保留 6 帧尾音、~1.2s 静音窗口收尾 `listen/stop`），降低敲击键盘/瞬间杂音误触发与句间短停顿截断。
- **WebSocket 传输层抗抖动加固（`esp_websocket_client.c` / `sdkconfig` / `sdkconfig.defaults`）**：
  - 将 `CONFIG_WS_BUFFER_SIZE` 扩容至 `4096` 并开启 `CONFIG_WS_DYNAMIC_BUFFER` 与 `CONFIG_ESP_WS_CLIENT_ENABLE_DYNAMIC_BUFFER`；关闭易与 mbedTLS 并发读写抢占冲突的 `CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK`，消除高频 Opus 发帧时的 `transport_poll_write(0)` 写超时丢帧问题。
  - 在 `esp_websocket_client.c` 中优化发送瞬时超时的保活处理（丢弃超时单帧并保持 WSS 长连接不断开）。

### 三、中英双语系统（i18n）、UI 交互与音频链路打磨
- **轻量级中英双语系统（`os_i18n.c` / `os_i18n.h`）**：新增 `OS_LANG_ZH` / `OS_LANG_EN` 双语运行与 NVS 持久化，支持首次刷机开机语言选择及全界面实时切换。
- **UI 与全双工音频体验提升（`os_ui.c` / `os_kbd24.c` / `os_xz_view.c` / `ui_manager.c` / `bsp_audio.c` / `hub_player.c`）**：持续打磨桌面视图、虚拟键盘、小智表情视图与音乐切歌体验。

---

## v2.0.0（2026-11-23）— 小智 AI 系统级原生集成 & 全双工语音架构升级

### 一、核心重磅：小智 AI（XiaoZhi AI）系统级深度集成

本次并非简单移植第三方 Demo，而是以**原生 C + LVGL 8.3 + ESP-SR**在 G'lam OS 内部完整构建了一套深度融合系统的 AI 语音中枢（源码位于 `os_firmware/main/xz/` 与 `os_firmware/main/ui/os_xz_view.c`）：

#### 1. 系统最高权限应用化（`com.glam.xiaozhi`）
- **最高权限注册**：在应用管理与安装器核心（`os_spk_installer`）中引入权限分级 `OS_APP_PRIV_SYSTEM` 与常驻后台策略 `OS_APP_BG_PERSISTENT_SVC`，将小智正式注册为系统最高权限内置应用 `com.glam.xiaozhi`。
- **可见但不可删除**：可在桌面启动器、「应用管理」与「多任务后台」中查看实时运行状态，标记为**「系统最高权限 · 不可卸载」**，免疫一键清理后台，且在底层卸载接口与 Wi-Fi REST API（`POST /api/uninstall?id=com.glam.xiaozhi` 返回 `HTTP 403 Forbidden`）均实施硬拦截。

#### 2. 开机静默常驻离线语音唤醒插件（ESP-SR WakeNet9「你好小智」）
- **全场景随时唤醒**：集成 `espressif/esp-sr`（WakeNet9 量化模型 `wn9_nihaoxiaozhi_tts`），开机自动静默拉起后台唤醒监听插件，**在系统桌面、设置、计算器、番茄钟、本地音乐以及任意动态加载的 `.elf` 第三方应用中**，直接喊一声 **「你好小智」** 即可即时唤醒对话。
- **音乐播放期间伴随唤醒**：共享 `I2S1` 在 44.1 kHz 播放音乐时，底层自动跟踪音乐采样率，并通过 **Q16 定点线性重采样（44.1 kHz → 16 kHz）**持续向 WakeNet9 喂音；检测到唤醒词后自动暂停当前音乐，对话结束后自动恢复（若通过 MCP 指令显式暂停/切歌则尊重用户指令）。
- **静默 OTA 与激活**：连上 Wi-Fi 后后台自动向 `xiaozhi.me`（或自定义开源服务端）完成 OTA 鉴权，首次绑定在全屏视图展示 6 位验证码。

#### 3. 全局语音浮层深度融合「灵动岛」（Dynamic Island）
- **顶层全局挂载**：将小智全局语音悬浮交互直接融合到系统顶栏 **灵动岛**（挂载在 `lv_layer_top()` 顶层，覆盖包括全屏 G'lam 音乐和动态 `.elf` 应用在内的所有页面）。
- **胶囊态与展开态无缝切换**：
  - 后台离线唤醒待命时，灵动岛胶囊亮起青色唤醒状态指示；
  - 喊出「你好小智」后自动展开灵动岛语音卡片，实时展示连接/聆听/回复状态、**STT 语音识别文字**、**TTS 回复字幕**与**实时麦克风电平条**；
  - 卡片内置 **「全屏」**（跳转至经典表情交互界面）与 **「×」**（快速结束会话）按钮，空闲或报错超时后自动折叠回胶囊态并恢复后台静默唤醒。

#### 4. 设备端 MCP 系统控制中枢（13 项本地工具）
小智通过标准 **MCP（JSON-RPC 2.0）** 协议获得系统最高级控制权限，可直接用自然语言操控全部本地应用与系统设置（实现于 `os_firmware/main/xz/xz_mcp.c`）：

| 分类 | MCP 工具名 | 功能说明 |
| :--- | :--- | :--- |
| **系统状态** | `self.get_device_status` | 查询音量、屏幕亮度、主题、前台应用、Wi-Fi、TF 卡、唤醒开关、音乐播放状态及内存占用 |
| **系统设置** | `self.audio_speaker.set_volume` | 调节系统喇叭音量（`0–100`） |
| **系统设置** | `self.screen.set_brightness` | 调节屏幕背光亮度（`10–100`） |
| **系统设置** | `self.theme.set` | 实时切换系统主题（`经典深色` / `初雪` / `深邃`） |
| **应用调度** | `self.app.list` | 列出所有内置系统应用与 TF 卡动态安装的 `.elf` 应用及其权限 |
| **应用调度** | `self.app.open` | 按名称或 ID 启动任意本地应用（如设置、计算器、时钟、音乐、赛车、系统监控等） |
| **应用调度** | `self.app.close` | 关闭当前前台或指定后台应用（自动保护系统常驻服务） |
| **应用调度** | `self.system.go_home` | 返回系统主桌面 |
| **本地音乐** | `self.music.play` | **接管点歌**：在本地 G'lam 音乐中搜索并自动播放指定歌曲/歌手 |
| **本地音乐** | `self.music.control` | 控制本地 G'lam 音乐播放状态（`play` / `resume` / `pause` / `stop` / `next` / `prev`） |
| **系统工具** | `self.clock.pomodoro` | 控制内置时钟番茄专注计时器（`start` / `pause` / `reset` 及自定义分钟数） |
| **语音插件** | `self.xiaozhi.set_wakeup` | 语音开启或关闭后台「你好小智」离线唤醒插件 |
| **电源控制** | `self.system.reboot` | 安全重启 ESP32-S3 设备 |

#### 5. 音乐链路 100% 路由至本地 G'lam 音乐
- 彻底屏蔽小智云端音乐流，所有语音点歌与切歌指令统一通过本地 G'lam 音乐引擎执行。
- 引入 **延迟音乐交接机制（`xz_service_schedule_music_handoff`）**：等待小智简短 TTS 确认语音播报完毕后，干净关闭 16 kHz 全双工语音通道、收起灵动岛，再无缝切至 44.1 kHz 启动本地 G'lam 音乐播歌，杜绝音频硬件冲突。

#### 6. 原生实时语音链路与全屏交互视图
- 自集成轻量 `esp_opus`（16 kHz / 60 ms 语音帧编解码，实例放 PSRAM）与 `esp_websocket_client`（WSS 加密长连接）。
- 提供原生全屏表情交互视图（`os_xz_view.c`），支持动态眼部/嘴部表情反馈、按住说话/自动连续对话、设置弹窗（自定义 OTA 服务器地址、离线唤醒总开关与模型状态显示、3 秒麦克风 Opus 录音回放自测）。

---

### 二、全双工音频驱动重构与系统稳定性治理

- **ES7210 + ES8311 全双工音频链路（`bsp_audio.c` / `bsp_profiles.c`）**：
  - 在默认立创·实战派 ESP32-S3（`lcsc_szp_s3`）及 `ESP32-S3-BOX-3` 档案中启用 `ES7210` 麦克风 ADC 探测（`0x41` / `0x40`）与 `I2S1` 全双工模式（`DIN=IO12`）。
- **彻底修复 ES8311 切采样率无声隐患**：
  - 禁用 `esp_codec_dev` 关闭时自动执行 `es8311_suspend()` 软复位（`esp_codec_set_disable_when_closed(s_play_dev, false)`），避免模拟输出驱动寄存器 `REG13 (0x10)` 被清零；新增 `bsp_es8311_ensure_dac_ready()` 在每次切换采样率与开启功放时强校验关键 DAC/功放寄存器。
  - 规范全双工 I2S1 重配顺序（强制先关 RX 再关 TX），并由 32 KB PSRAM 栈的 `xz_audio_task` 惰性重开唤醒 RX，减轻 `Audio Task` 4 KB 栈压力。
- **内部 SRAM 深度优化与防崩溃修复**：
  - 调优 Wi-Fi IRAM 与静/动态收发缓冲配置（`sdkconfig.defaults`），配合后台任务栈合理分配至 PSRAM，为系统腾出约 **44 KB** 宝贵内部 SRAM（开机后空闲内部 SRAM 维持 ~62 KB）。
  - 将音乐在线拉流线程 `online_worker` 栈扩容至 **12 KB**，彻底解决在线播歌时的栈溢出重启问题。
  - 修复 HTTP `/api/info` 与 `/api/apps` 因局部应用数组过大导致的 `httpd` 栈溢出问题。
- **新增硬件音频诊断 REST 接口**：
  - `GET /api/audio_diag`：实时回读 `PCA9557`、`ES8311` 关键寄存器、`I2S1` 硬件状态与音频管线计数；
  - `POST /api/audio_beep`：直接输出 660 Hz 测试正弦波，便于快速排查喇叭与功放硬件链路。

---

### 三、系统 UI、开机动画与基础体验升级

- **灵动岛（Dynamic Island）与桌面视觉体系升级（`os_ui.c` / `ui_manager.c` / `ui_theme.h`）**：
  - 全面打磨主桌面卡片、控制中心、多任务后台管理、虚拟键盘（`os_kbd24.c`）与音乐播放器界面质感，新增 `docs/Glam_OS_UI设计规范.md`。
- **直写 DMA 开机动画引擎（`boot_anim.c` / `tools/pack_boot_anim.py`）**：
  - 支持从内部 Flash `storage` 分区自动迁移至 TF 卡 `/sdcard/system/boot_anim.gmv`，开机预载 PSRAM 并通过双 DMA 缓冲区以 320×240 15 fps 流畅播放。
- **触控与封面优化（`bsp_board.c` / `cover_fetcher.c`）**：
  - 优化电容触控防抖滤波算法，兼顾单帧 EMI 尖峰过滤与快速滑动不断触；支持音乐专辑封面解码与 320×240 高斯模糊背景同步生成。

---

### 四、Flash 分区表变更与升级刷写提示

为存放 ESP-SR 离线唤醒模型 `srmodels.bin`，`os_firmware/partitions.csv` 将原 7 MB `storage` 分区拆分为：
- **`model`**：`0x820000`，大小 `1M`（`spiffs`，存放 `srmodels.bin`）
- **`storage`**：`0x920000`，大小 `6M`（`fat`）
- `nvs`（`0x9000`）、`otadata`（`0xf000`）、`ota_0`（`0x20000`）、`ota_1`（`0x420000`）地址保持不变。

> **升级提示**：从旧版（`3dc8054` 及之前）首次升级至本版本时，请使用 `idf.py flash`（或完整刷写 `partition-table.bin`、`glam_os.bin` 及 `0x820000` 处的 `srmodels.bin`），**无需擦除 `nvs`**，原有 Wi-Fi 配置与 TF 卡已安装应用均可无损保留。
