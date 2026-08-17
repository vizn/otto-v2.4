# otto-robot-v1 定制功能说明

本文档记录 otto-robot-v1 开发板上已完成的自定义功能，包括固件改动与服务器端功能。

## 固件改动

### 1. 英文唤醒词 "OK EMO"

在 `main/boards/otto-robot-v1/config.json` 中启用自定义唤醒词，使用 MultiNet6 英文模型。

```json
"CONFIG_USE_CUSTOM_WAKE_WORD=y",
"CONFIG_SR_MN_EN_MULTINET6_QUANT=y",
"CONFIG_CUSTOM_WAKE_WORD=\"OK EMO\"",
"CONFIG_CUSTOM_WAKE_WORD_DISPLAY=\"OK EMO\"",
"CONFIG_CUSTOM_WAKE_WORD_THRESHOLD=20"
```

- 唤醒词：`OK EMO`（MultiNet6 英文模型，`mn6_en`）
- 阈值：20（已实机验证，误唤醒与灵敏度平衡良好）
- 刷机后串口日志确认：`Model 0: mn6_en` / `Command: OK EMO` / `detector: MultiNet`

### 2. 修复"后端语音播放中被唤醒打断"的 Bug

修改 `main/audio/engines/afe_audio_engine.h` 第 38 行：

```cpp
// 原实现：只要检测到唤醒词就判定为 AFE 唤醒
bool IsAfeWakeWord() const override { return HasWakeWord(); }

// 修复后：仅当使用 WakeNet 唤醒检测器时判定为 AFE 唤醒
bool IsAfeWakeWord() const override { return wake_detector_ == WakeDetector::kWakeNet; }
```

**背景**：当后端正在播放 TTS 语音时，若用户说出唤醒词，旧逻辑会把唤醒检测也当作 AFE 唤醒，导致播放被异常打断。修复后仅 WakeNet 检测器触发唤醒。

**构建产物**：`releases/v2.4.1_otto-robot-v1.zip`（merged-binary 14,675,796 B），已通过 USB 刷入 COM14，全部 Hash verified。

### 3. 英文舞蹈指令识别

修改 `main/boards/otto-robot-v1/miot_client.cc`，当服务器通过 MIOT 下发歌曲时，同步检查用户原话（STT 原文）是否含舞蹈意图关键词。

新增英文关键词匹配（大小写不敏感）：

| 关键词 | 说明 |
|---|---|
| `dance` / `dancing` / `danced` | 通用跳舞 |
| `groove` | 律动 |
| `party` | 派对 |
| `shake a leg` | 摇腿 |

匹配成功则调用 `OttoControllerQueueContinuousDance()`，使 Otto 在播放音乐的同时进入连续舞蹈模式。

### 4. 连续舞蹈动作编排（v2.4.2）

修改 `main/boards/otto-robot-v1/otto_controller.cc`，新增 `DanceMove` 结构体和 `QueueNextContinuousDanceStep()` 方法，将单一 SafeGroove 律动替换为多动作轮播编排。

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

### 3. 功夫英语课程音频集成（v2.4.2）

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
用户语音 → Otto STT → LLM qwen3.5:9b(function_call)
  → play_music(song_name="功夫英语-FaceFonics-第005课")
  → MIOT 下发设备播放命令
  → Otto PlayByKeyword("功夫英语-FaceFonics-第005课")
  → GET http://192.168.199.162:8003/api/music/stream?song=功夫英语-FaceFonics-第005课
  → miot_gateway._find_best_match()（difflib 模糊匹配文件名）
  → 返回 200 audio/mpeg 流 → 设备端 MP3 解码播放
```

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

## 固件语言能力调研（后续方向）

| 唤醒模型 | 支持语言 | 说明 |
|---|---|---|
| MultiNet6 | 中文（`mn6_cn`）、英文（`mn6_en`） | 各约 3.5MB，`esp_mn_iface.h` 定义 `ESP_MN_ENGLISH "en"` / `ESP_MN_CHINESE "cn"` |
| WakeNet9 | 中文、英文、日文、法文等 | 如 `wN9L_JA_KONNICHIHAESP_TTS3`（こんにちはESP）、`wN9L_FR_BONJOURESP_TTS3`（Bonjour ESP） |

- 16MB flash 的 assets 分区 8MB，`movemodel.py` 可同时打包中英文模型（各约 3.5MB），实现"中英双模型同刷"
- `custom_wake_word.cc` 使用 `esp_srmodel_filter(models_, ESP_MN_PREFIX, language_)` 按语言选择模型，运行时一次仅加载一个 MultiNet 实例
- 日语/韩语唤醒词需使用 WakeNet9 固定唤醒词（如 こんにちはESP），当前 MultiNet 无日/韩模型

## 部署方法

### 固件（v2.4.2）

v2.4.2 通过 COM14 串口烧录（不通过 OTA）：

```bash
source /path/to/esp-idf/export.sh
idf.py -p COM14 flash
```

固件版本保持 2.4.2（与 OTA 版本号相同），烧录后验证：
- `self.get_system_info` → `application.version: 2.4.2`
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
