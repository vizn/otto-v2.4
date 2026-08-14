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

## 固件语言能力调研（后续方向）

| 唤醒模型 | 支持语言 | 说明 |
|---|---|---|
| MultiNet6 | 中文（`mn6_cn`）、英文（`mn6_en`） | 各约 3.5MB，`esp_mn_iface.h` 定义 `ESP_MN_ENGLISH "en"` / `ESP_MN_CHINESE "cn"` |
| WakeNet9 | 中文、英文、日文、法文等 | 如 `wN9L_JA_KONNICHIHAESP_TTS3`（こんにちはESP）、`wN9L_FR_BONJOURESP_TTS3`（Bonjour ESP） |

- 16MB flash 的 assets 分区 8MB，`movemodel.py` 可同时打包中英文模型（各约 3.5MB），实现"中英双模型同刷"
- `custom_wake_word.cc` 使用 `esp_srmodel_filter(models_, ESP_MN_PREFIX, language_)` 按语言选择模型，运行时一次仅加载一个 MultiNet 实例
- 日语/韩语唤醒词需使用 WakeNet9 固定唤醒词（如 こんにちはESP），当前 MultiNet 无日/韩模型

## 部署方法（服务器端补丁）

1. 将 `core/admin_api.py`、`core/connection.py`、`core/helloHandle.py` 复制到服务器补丁目录
2. 执行 `reapply.sh` 复制进容器（注意：`docker cp` 更可靠，reapply.sh 偶尔静默失败）
3. 部署后用 `docker exec xiaozhi-esp32-server grep -c` 验证容器内文件已更新
4. `docker restart xiaozhi-esp32-server` 重启生效
