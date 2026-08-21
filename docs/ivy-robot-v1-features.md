# ivy-robot-v1 定制功能说明

本文档记录 ivy-robot-v1 开发板上已完成的自定义功能，包括固件改动与服务器端功能。

## 固件改动

### 1. 唤醒词：框架原生 WakeNet「Hey, Ivy」

`config.json` 的 `sdkconfig_append` 显式启用英文唤醒词（关掉默认的中文「你好小智」）：

- 唤醒引擎：AFE WakeNet（`CONFIG_USE_AFE_WAKE_WORD=y`，S3+PSRAM）
- 模型：`CONFIG_SR_WN_WN9_HEYIVY_TTS2=y` → wakenet `wn9_heyivy_tts2`（=「Hey, Ivy」），关闭 `CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS`
- 串口日志：`AFE Pipeline: [input] -> |VAD(WebRTC)| -> |WakeNet(wn9_heyivy_tts2,)| -> [output]`，`detector: WakeNet`，`wakenet9_tts2h12_Hey Ivy`
- 该模型为乐鑫原生 WakeNet，可靠、无误触发；说 "Hey, Ivy" 即可唤醒
- 曾尝试自定义唤醒词 "OK EMO"（`USE_CUSTOM_WAKE_WORD` + MultiNet6 mn6_en）：命令已成功注册、FST 构建成功，阈值 20 时服务端确实收到过 `detect text="OK EMO"`，证明修改生效。但 mn6_en 词表无 OK/EMO 整词、只能按 O-K/E-M-O 子词拼装，匹配基本靠声学能量而非真正识别该词——吵的环境易被杂音误触发（自唤醒），安静房间说 OK EMO 反而能量不足唤不醒。可靠性差，已回退。
- 注：若想用英文唤醒词，可在 `config.json` 启用 `CONFIG_USE_AFE_WAKE_WORD=y` 并加 `CONFIG_SR_WN_WN9S_HIESP=y`（"Hi, ESP"）/ `CONFIG_SR_WN_WN9_ALEXA=y`（"Alexa"）/ `CONFIG_SR_WN_WN9_JARVIS_TTS=y`（"Jarvis"）之一后重刷。

### 2. 修复"后端语音播放中被唤醒打断"的 Bug

修改 `main/audio/engines/afe_audio_engine.h` 第 38 行：

```cpp
// 原实现：只要检测到唤醒词就判定为 AFE 唤醒
bool IsAfeWakeWord() const override { return HasWakeWord(); }

// 修复后：仅当使用 WakeNet 唤醒检测器时判定为 AFE 唤醒
bool IsAfeWakeWord() const override { return wake_detector_ == WakeDetector::kWakeNet; }
```

**背景**：当后端正在播放 TTS 语音时，若用户说出唤醒词，旧逻辑会把唤醒检测也当作 AFE 唤醒，导致播放被异常打断。修复后仅 WakeNet 检测器触发唤醒。

**构建产物**：`releases/v2.4.3_ivy-robot-v1.zip`（约 3.6MB），已通过 USB 刷入 `/dev/ttyACM0`，全部 Hash verified。

### 3. 英文舞蹈指令识别

修改 `main/boards/ivy-robot-v1/miot_client.cc`，当服务器通过 MIOT 下发歌曲时，同步检查用户原话（STT 原文）是否含舞蹈意图关键词。

新增英文关键词匹配（大小写不敏感）：

| 关键词 | 说明 |
|---|---|
| `dance` / `dancing` / `danced` | 通用跳舞 |
| `groove` | 律动 |
| `party` | 派对 |
| `shake a leg` | 摇腿 |

匹配成功则调用 `OttoControllerQueueContinuousDance()`，使 Otto 在播放音乐的同时进入连续舞蹈模式。

### 4. 连续舞蹈动作编排

修改 `main/boards/ivy-robot-v1/otto_controller.cc`，新增 `DanceMove` 结构体和 `QueueNextContinuousDanceStep()` 方法，将单一 SafeGroove 律动替换为多动作轮播编排。

编排序列（共 21 个条目，循环播放）：

| 序号 | 动作 | 参数 |
|---|---|---|
| 1 | SafeGroove（基础律动） | — |
| 2 | Moonwalk（太空步） | 左，speed=900，amount=20 |
| 3 | BeatKeeping（打节拍） | — |
| 4 | SafeGroove | — |
| 5 | Swing（摇摆） | speed=800，amount=25 |
| 6 | Moonwalk（太空步） | 右，speed=900，amount=20 |
| 7 | SafeGroove | — |
| 8 | TiptoeSwing（踮脚摇摆） | speed=800，amount=20 |
| 9 | SafeGroove | — |
| 10 | FlowerDance（花花舞） | — |
| 11 | SafeGroove | — |
| 12 | UpDown（上下律动） | speed=700，amount=15 |
| 13 | SafeGroove | — |
| 14 | Jitter（抖动） | speed=600，amount=15 |
| 15 | SafeGroove | — |
| 16 | AscendingTurn（旋转） | speed=900，amount=10 |
| 17 | SafeGroove | — |
| 18 | BeatKeeping（打节拍） | — |
| 19 | SafeGroove | — |
| 20 | ShakeLeg（摇腿） | 右，speed=1500 |
| 21 | SafeGroove | — |

**设计原则**：
- 所有动作幅度低、重心稳定，SafeGroove 作为基础律动穿插间隔
- FlowerDance / BeatKeeping 内部已处理 `has_hands_` 标志，有手/无手舵机均安全
- Moonwalk 每次仅走 3 步，方向左右交替

`QueueContinuousDance()` 重置 `dance_routine_index_ = 0`，每次新会话从头开始编排。

## 服务器端功能

服务器端改动位于 xiaozhi-esp32-server（`/vol1/docker/xiaozhi-esp32-server`），通过补丁方式部署。

### 1. 唤醒回复可配置

管理后台新增「唤醒回复」卡片，可自定义设备唤醒后的回复文案。

- 文件：`core/helloHandle.py`、`core/admin_api.py`
- 配置项：`wakeup_words_response`（写入 `data/.config.yaml`）
- 默认回复（中文）：`我一直都在呢，您请说。` 等 5 条
- 前端：后台页面「唤醒回复」输入框 `#wkrInput` / 保存按钮 `#wkrSave`
- API：`GET/POST /api/admin/wakeup-responses`

### 2. 后台语言设置（中/英/日/韩）

管理后台新增「语言设置」卡片，一键切换对话/TTS 回复语言。

- 文件：`core/admin_api.py`、`core/connection.py`
- 预设语言：中文 `zh`、英文 `en`、日文 `ja`、韩文 `ko`
- 每个预设包含：
  - `tts_voice`：EdgeTTS 音色
    - 中文 `zh-CN-XiaoxiaoNeural`
    - 英文 `en-US-JennyNeural`
    - 日文 `ja-JP-NanamiNeural`
    - 韩文 `ko-KR-SunHiNeural`
  - `tts_language`：增强提示词模板 `{{language}}` 变量（中文/英语/日语/韩语）
  - `wakeup_responses`：对应语言的唤醒回复
  - `prompt`：对应语言的完整系统提示词（非中文时整体替换，避免中英混合）
- 配置项写入 `data/.config.yaml`：
  - `lang`
  - `TTS.EdgeTTS.voice`
  - `TTS.EdgeTTS.language`
  - `wakeup_words_response`
  - `prompt`（英文/日文/韩文版系统提示词）
  - `prompt_zh`（切换前自动备份的中文系统提示词，切回中文时恢复）
- API：`GET/POST /api/admin/language`
- 前端：后台页面「语言设置」下拉框 `#langSelect` / 保存按钮 `#langSave` / 音色预览 `#langVoice`
- **生效方式**：保存后需重启容器（`docker restart xiaozhi-esp32-server`），因 EdgeTTS 音色在 provider 初始化时固定

**关键实现细节**：

- `connection.py` 在 `_initialize_components` 中按 `lang` 前置语言指令（zh 为空、en 前置 `You MUST respond in English only. Never use Chinese.`）
- `build_enhanced_prompt` 会以原始 `config["prompt"]` 重建增强提示词并覆盖临时注入，因此切换语言时直接整体替换 `prompt`，并在 `TTS.EdgeTTS.language` 写入对应语种，供模板 `<output_language_directive>` 使用
- ASR（SenseVoice/FunASR）原生支持 zh/en/ja/ko/yue，无需切换

### 3. 功夫英语课程音频集成

将功夫英语（kungfuenglish.com）本地视频课程提取音频，集成到 Otto 语音点播系统。

**资源来源**：fnOS 主机 `/vol1/kungfu-english-app/public/videos/`，共 7 个课程目录、约 16GB 视频。

**音频提取**（在 fnOS 主机上用 ffmpeg 批量转换）：

| 课程 | 来源 | 输出命名 | 数量 |
|---|---|---|---|
| FaceFonics 发音训练 | `facefonics-fixed/*-no-subtitles.mp4` | `功夫英语-FaceFonics-第NNN课.mp3` | 109 条 |
| 泡脑子（Brain Soaking） | `lesson-NN/brain-soakingN-audio.mp4` | `功夫英语-泡脑子-第N课.mp3` | 10 条 |
| 词汇拓展（单词歌） | `lesson-NN/word-song-N.mp4` | `功夫英语-单词歌-第N首.mp3` | 73 条 |
| 第三只耳朵专辑 | `audio-ok/chapter-NN.mp3`（已有 MP3） | `功夫英语-第三只耳朵专辑-第NN章.mp3` | 16 条 |

共 **208 条** MP3，总计约 1.9GB，存放在 fnOS `/vol1/1000/小赫用/`（容器挂载为 `/opt/xiaozhi-esp32-server/music:ro`）。

**播放链路**：
```
用户语音 → Otto STT → LLM (function_call)
  → play_music(song_name="功夫英语-FaceFonics-第005课")        # 服务端函数 plugins_func/functions/play_music.py
  → 服务端解析：扫描 music_dir 文件列表，difflib _find_best_match() 模糊匹配出真实文件名（阈值 ratio>0.4）
  → 经 MIOT 网关下发：core.miot_gateway.send_device_command(device_id, {"type":"music","text":<匹配到的文件名>})
  → 设备侧 miot_client.cc 收到（WebSocket, ESP32-MIOT/1.0）→ HandleControlCommand → music_player_->PlayByKeyword(<文件名>)
  → 设备端 GET http://192.168.199.162:8003/api/music/stream?song=<文件名> 向音乐网关拉取音频流
  → 返回 200 audio/mpeg → 设备 MP3 解码播放
```

- **模糊匹配在「服务端」完成**（`play_music.py` 的 `_find_best_match`，见 §4 服务端函数）；MIOT 只负责把"文件名关键词"下发到设备，设备 `PlayByKeyword` 再去音乐网关拉流——原文档写的 `miot_gateway._find_best_match()` 位置有误。
- **避免双路音频**：MIOT 下发成功后，服务端会 `clear_queues()` 停掉自身 TTS，并另发一条 `{"type":"chat","text":歌名}` 让设备屏幕显示歌名（见 `play_music.py` 的 `handle_music_command`）。
- 注：当前 `play_music` 描述已把功夫英语课程改走 `play_course` / 学习卡片（`study_card`）通道，本节的功夫英语示例为早期集成方式，链路机制（服务端匹配 → MIOT 下发 → 设备拉流）不变。

**LLM 工具调用**（已验证 qwen3.5:9b 行为）：

| 用户指令 | LLM 返回的 song_name |
|---|---|
| "播放FaceFonics第5课" | `FaceFonics第5课` |
| "播放泡脑子第3课" | `功夫英语-泡脑子-第3课` |
| "播放功夫英语课程" | `功夫英语`（随机匹配） |
| "播放音乐" | `random` |

**服务器插件改动**：
- `plugins_func/functions/play_music.py`：`play_music_function_desc` 更新，允许功夫英语课程音频点播，说明文件名格式
- `plugins_func/functions/call_openclaw.py`：`_DEFAULT_DESCRIPTION` 更新，播放音频交 `play_music`，课程咨询交 `call_openclaw`
  - `data/.config.yaml`：`Intent.function_call.functions` 中 `call_openclaw` 已启用

### 4. 服务端函数：`call_openclaw`（OpenClaw 任务桥）与 `play_music` 分工

（基于 `plugins_func/functions/call_openclaw.py` 实际实现；不是"课程问答"，而是把任务交给 fnOS 上的 OpenClaw 智能体执行的中转桥）

- **触发**：LLM 调用 `call_openclaw(task)`，必填参数 `task` 为用户需求的完整自然语言描述；空则返回"请提供要执行的任务"。
- **配置项**（写入 `data/.config.yaml` 的 `plugins.openclaw`）：
  - `base_url`：OpenClaw 服务地址，默认 `http://192.168.199.162:18789`
  - `token`：访问令牌，**必填**；缺失则返回"OpenClaw未配置访问令牌"
  - `ack_message`：提交任务后即时回给 LLM 的确认话术（默认"任务已提交给OpenClaw后台执行…正在处理"，并指示 LLM 不要再调任何工具，结果稍后自动播报）
- **执行**：`POST {base_url}/v1/chat/completions`（OpenAI 兼容接口），模型 `openclaw/default`，后台线程运行、超时 300s；成功取 `choices[0].message.content` 作为结果文本。
- **回播**：结果经 `speak_txt(conn, text)` 在后台直接语音播报给用户。

**与 `play_music` 的分工**：
- 用户**要求播放**某首歌曲/某段课程音频（如"播放FaceFonics第5课"）→ 用 `play_music`，传入提取出的文件名关键词 `song_name`（见第 3 节播放链路）。
- 用户仅**咨询**课程内容、学习方法、业务/产品信息等（不要求播放）→ 用 `call_openclaw`，把需求作为 `task` 交给 OpenClaw 执行。

实际能力取决于 OpenClaw 本身（联网搜索、查资料、文件操作、运行命令、多步任务、业务咨询等），本函数只负责"传话 + 念回结果"。

**服务端全部可用函数**（`plugins_func/functions/`，当前 `Intent.function_call.functions` 启用 6 个）：

| 函数 | 用途 | 状态 |
|---|---|---|
| `call_openclaw` | 调 fnOS OpenClaw 智能体执行任务（见上） | 启用 |
| `play_music` | 音乐/功夫英语点播（第 3 节） | 启用 |
| `web_search` | 联网搜索 | 启用 |
| `get_weather` / `show_weather` | 天气查询 | 启用 |
| `get_news_from_newsnow` | NewsNow 新闻 | 启用 |
| `call_device` | 桥接层：把 LLM 调用转发到固件侧 MCP 工具（`self.ivy.*` 等经此下去） | 基础设施 |
| `change_role` | 切换角色 | 未启用 |
| `get_news_from_chinanews` | 中新网新闻 | 未启用 |
| `get_time` | 获取时间 | 未启用 |
| `handle_exit_intent` | 处理"退出/再见"意图 | 未启用 |
| `hass_*`（`init`/`get_state`/`set_state`/`play_music`） | Home Assistant 智能家居联动 | 未启用 |
| `play_radio` | 播放电台 | 未启用 |
| `search_from_ragflow` | RAGFlow 知识库检索 | 未启用 |
| `xiaozhi_push` | 推送消息到设备 | 未启用 |

## MIOT → MCP 迁移（设备侧，已完成 Phase 1）

把本板私有 MIOT 能力以 `self.ivy.*` MCP 工具形式暴露，使服务端/LLM 可直接通过 MCP 调用，
最终逐步替代现有 MIOT 下发路径（增量、不破坏现有 MIOT）。

### 设计

- 复用既有逻辑：在 `miot_client.cc` 新增一组薄封装 `McpPlayMusic / McpControlPlayback /
  McpPlayAlbum / McpPlayRadio / McpPlayWeather / McpPlayStudy`，每个方法仅构造与服务器下发
  MIOT 指令**完全相同**的 JSON（`{"type":"music","text":...}` 等），再调用既有
  `HandleControlCommand`，因此 MIOT 与 MCP 行为完全一致，零逻辑复制。
- 工具注册：在 `otto_robot.cc` 的 `OttoRobot` 构造函数（网络连通前）调用
  `RegisterIvyMediaTools()`，经 `McpServer::GetInstance().AddTool(...)` 注册 8 个工具；
  回调捕获 `this`，调用时再空指针判定 `music_player_`/`miot_client_`（连接后才创建）。
- 命名空间沿用 `self.ivy.*`（与动作工具 `self.ivy.action` 等同属 Ivy 板能力）。

### 工具映射（MIOT 功能 → MCP 工具）

| MCP 工具 | 对应 MIOT | 说明 |
|---|---|---|
| `self.ivy.play_music(keyword, dance)` | `music` | 点播；`dance=true` 额外触发连续舞蹈 |
| `self.ivy.control_playback(action)` | `music_control` | play/pause/next/previous/stop |
| `self.ivy.play_album(album)` | `album` | 拉取歌单连播 |
| `self.ivy.play_radio(station, name)` | `radio` | 电台 |
| `self.ivy.show_weather(city, city_enc)` | `weather` | 天气播报（上屏+EdgeTTS） |
| `self.ivy.play_course(key, word, course)` | `study_card`/`study_course` | 学习卡片/课程连播 |
| `self.ivy.show_message(text)` | `chat` | 屏幕文本 |
| `self.ivy.media_status()` | — | 查询 playing/paused |

### 验证（2026-08-21 固件）

- 构建通过（ESP-IDF v6.0.2），烧录 `ivy-robot-v1` 后启动日志确认 8 个 `self.ivy.*` 媒体工具
  全部注册（`MCP 媒体工具(self.ivy.*)注册完成`），无 error/panic；
  `MiotClient` 连接 `ws://192.168.199.162:8003/ws` 正常、`Hey Ivy` 唤醒词正常。
- 工具回调复用 MIOT 既有代码路径，行为与 MIOT 下发等价。

### 后续阶段（未做，需决策）

- **Phase 2（服务端切换）**：将服务端 `play_music.py` 等从 `miot_gateway.send_device_command`
  改为调用本板 `self.ivy.*` MCP 工具；需服务端适配且不影响现有 MIOT 路径灰度。
- **Phase 3（退役 MIOT）**：确认 MCP 路径稳定后，移除 `miot_client.*`，仅保留 MCP 媒体能力。

## 固件语言能力调研（后续方向）

| 唤醒模型 | 支持语言 | 说明 |
|---|---|---|
| MultiNet6 | 中文（`mn6_cn`）、英文（`mn6_en`） | 各约 3.5MB，`esp_mn_iface.h` 定义 `ESP_MN_ENGLISH "en"` / `ESP_MN_CHINESE "cn"` |
| WakeNet9 | 中文、英文、日文、法文等 | 如 `wN9L_JA_KONNICHIHAESP_TTS3`（こんにちはESP）、`wN9L_FR_BONJOURESP_TTS3`（Bonjour ESP） |

- 16MB flash 的 assets 分区 8MB，`movemodel.py` 可同时打包中英文模型（各约 3.5MB），实现"中英双模型同刷"
- `custom_wake_word.cc` 使用 `esp_srmodel_filter(models_, ESP_MN_PREFIX, language_)` 按语言选择模型，运行时一次仅加载一个 MultiNet 实例
- 日语/韩语唤醒词需使用 WakeNet9 固定唤醒词（如 こんにちはESP），当前 MultiNet 无日/韩模型

## 部署方法

### 固件（v2.4.3）

v2.4.3 通过串口烧录（不通过 OTA）：

```bash
source /path/to/esp-idf/export.sh
idf.py -p /dev/ttyACM0 flash
```

固件版本保持 2.4.3（与 OTA 版本号相同），烧录后验证：
- `self.get_system_info` → `application.version: 2.4.3`
- `elf_sha256` 变为新值（含编排代码）
- 系统日志出现 `连续舞蹈编排 #N/TOTAL: 动作=X`

### 服务器端补丁

1. 将 `core/admin_api.py`、`core/connection.py`、`core/helloHandle.py` 复制到服务器补丁目录
2. 执行 `reapply.sh` 复制进容器（注意：`docker cp` 更可靠，reapply.sh 偶尔静默失败）
3. 部署后用 `docker exec xiaozhi-esp32-server grep -c` 验证容器内文件已更新
4. `docker restart xiaozhi-esp32-server` 重启生效

### 功夫英语音频

音频文件存放在 fnOS `/vol1/1000/小赫用/`，已挂载到服务器容器 music 目录。更新时：
1. 将新 MP3 文件放入 `/vol1/1000/小赫用/`
2. 服务器自动扫描（refresh_time: 300s），或 `docker restart xiaozhi-esp32-server` 立即刷新索引

## 上游同步基线（2026-08-20）

ivy-robot-v1 与上游 `78/xiaozhi-esp32` 的关系已通过 3-way 比对确认（基线 commit `361ab01` 的板子文件作为共同祖先，对比上游 tag `v2.4.2`）。

结论：**本板代码已与上游 v2.4.2 基本对齐，无需功能性合并**。差异仅分为两类，均为有意为之：

1. **无摄像头变体（硬件差异）**：上游 `otto-robot` 是摄像头版（OV2640/OV3660，运行时硬件版本检测），本板为 `OTTO_VERSION_NO_CAMERA`，`config.h` 使用硬编码的 `OTTO_V1_CONFIG`（取自上游早期 NON_CAMERA 引脚，已实测驱动 ST7789/INMP441/MAX98357）。上游 `otto_robot.cc` 的非摄像头初始化代码（SPI/LCD/按键/音频/WebSocket/电源/电量）与本板**逐字节相同**，仅多出摄像头检测/初始化（`DetectHardwareVersion`/`InitializeCamera`/`GetCamera`），本板有意不包含。
2. **本板私有扩展（叠加在最新上游之上）**：
   - `miot_client.*` + `music_player.*`：MIOT 音乐/功夫英语/双路音频（上游无）
   - `otto_controller.cc`/`otto_movements.*`：连续舞蹈（`QueueContinuousDance`/`QueueNextContinuousDanceStep`）、`FlowerDance`/`BeatKeeping`/`SafeGroove`、跨实例停止控制（`RequestStop`/`ClearStop`/`StopRequested`/`StopAll`）
   - `power_manager.h`：300k/100k 分压 + USB 充电电压抬升推断
   - `websocket_control_server.cc`：ESP-IDF v6.0.2 握手适配
   - `config.json`/`README.md`：Hey Ivy 唤醒词、OTA 域名（自有服务器）、Ivy 品牌

已验证的上游 v2.4.2 与本板**完全一致**的文件（祖先 == 上游，本板已是最新）：`otto_controller.cc`、`otto_movements.cc`、`otto_movements.h`、`oscillator.*`、`otto_emoji_display.*`、`otto_icon_font.c`。后续如需吸收上游新增的板级改进，对以上文件直接 `git merge` 即可，不会与本板特性冲突。

上游 tag 对照：上游固件最新 `v2.4.2`（2026-08-06）；txp666 的 Otto 固件（ottodiy.tech）`v2.3.0`；本板 `PROJECT_VER` 自定为 `2.4.3`（非上游 tag）。
