# 会话基础能力 v1（2.5.2）

任务优先级和 Server 适配见相邻 `ivy-robot-server/docs/device-capability-roadmap.md`。

## 职责

语音会话状态与本地媒体状态独立。结束会话释放语音处理和旧 TTS 输出，
不把音乐、学习卡或舵机业务迁入应用主循环。应用需要时先停止媒体，再结束会话。

## MCP

`self.conversation.get_status` 无参数，返回：

```json
{
  "schema_version": 1,
  "boot_id": "0123456789abcdef",
  "seq": 1,
  "session_id": "current-transport-session",
  "state": "listening",
  "ended": false,
  "audio_channel_open": true,
  "wake_word_enabled": true,
  "voice_processing": true,
  "media_playing": false,
  "study_playing": false,
  "last_end_request_id": ""
}
```

`self.conversation.end` 参数：

```json
{"request_id":"unique-command-id","session_id":"current-transport-session"}
```

- `request_id` 非空、最长 128 字节。`session_id` 必须与当前传输会话一致。
- 仅 idle/connecting/listening/speaking 状态允许结束，升级、配网、音频测试等
  状态不会被该操作改为 idle。
- 在 MCP 的主任务回调中关闭语音处理、重置解码队列、丢弃上行残留，转 idle。
- 同一已结束会话重复调用不重复清理。再次开启交互后，重放上一结束请求会被拒绝。
- 结果包含上述快照和 `status=completed`、`request_id`；代表本地语音交互已结束。
  `audio_channel_open=true` 可以与 `state=idle, ended=true` 同时出现。
- 保留 MCP 通道返回结果，由 Server 收到并确认结果后关闭旧语音传输，避免 ACK
  尚未发出即断开连接。无需依赖固定延时、睡眠或重复调用结束命令。
- 已结束的交互拒绝下行音频和迟到 TTS start/stop。新本地唤醒、按键或新音频通道
  开放新语音代次；旧代次回调不会在新交互中重新启动播报。

`notifications/ivy/conversation_status` 的 params 与状态查询一致。
初始化完成时发送快照，应用状态变化时更新；boot 内 seq 递增。
这些事件与 `notifications/ivy/study_status` 并存。

## 构建与验证

```sh
source /home/asin/esp/esp-idf/export.sh
python3 -m unittest discover -s scripts/tests -v
python3 scripts/release.py ivy-robot-v2 --name ivy-robot-v2
python3 scripts/release.py xmini-c3 --name xmini-c3
```

已有同版本 release zip 时，发布脚本会跳过该变体；重新验证前应归档旧产物。
不要把跳过构建误报为重新构建通过。

ESP-IDF 6.0.2 下两种芯片均构建成功，MQTT 与 WebSocket 实现均编译。
主机回归 14 项通过，外部 MP3 样本测试 1 项跳过。只对新增/修改的 C++ 区域
应用仓库 clang-format（23.1.3），保留既有文件其他格式。

最终 S3 构建产物：

- `build/ivy-robot.bin`：SHA256 `7bdb468f95c8cfd219607e116601c931464de4fdd8d32e5e8f67e33f1b320c25`。
- `releases/v2.5.2_ivy-robot-v2.zip`：完整合并镜像，适用于全量烧录。
- `releases/v2.5.2_xmini-c3.zip`：代表性 C3 构建，不能用于 Ivy Robot v2 设备。

## 上线验收状态

2026-10-19 已部署配套 Server，并通过 USB 仅刷写目标设备活动应用分区至 2.5.2。
完整 16MB Flash 已备份，写入哈希校验通过；新能力发现、真实音乐停止与本地待机/
唤醒开关查询通过。详见 Server 的 `docs/conversation-p0-release-20261019.md`。
以下验收项中，实际麦克风复唤醒与直接 WebSocket 语音传输仍待确认：

1. 监听、播报中结束后状态为 idle、ended=true、voice_processing=false。
2. 清空旧播报，不再输出迟到音频，重新唤醒可正常收音。
3. 播放音乐期间单独结束语音会话不错误停止媒体；组合停止能正确退出。
4. 重复结束、旧 session 和上一交互 request_id 不破坏新交互。
5. MQTT 与直接 WebSocket 分别进行物理联调。
