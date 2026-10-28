#ifndef MIOT_CLIENT_H
#define MIOT_CLIENT_H

#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "music_player.h"

class MusicPlayer;
class WebSocket;

/**
 * @brief MIOT 智能网关客户端（jsrc.top）
 *
 * 协议（通过逆向 jsrc.top 网关验证）：
 *   1. 连接 ws://www.jsrc.top:6677/ws
 *   2. 发送 {"type":"hello","data":{"clientType":"esp32","device_id":"esp32_<mac>","mac":"<mac>"}}
 *      -> 服务器返回 hello_ack + device_online
 *   3. 周期性发送 {"type":"ping"} -> 服务器返回 {"type":"pong"}
 *   4. 服务器推送 {"type":"device_control_command","data":{"type":"music","text":"关键词","mac":"..."}}
 *      等控制命令，分发给 MusicPlayer
 */
class MiotClient {
public:
    MiotClient();
    ~MiotClient();

    void Start();
    void Stop();
    void SetMusicPlayer(MusicPlayer* player) { music_player_ = player; }

    // === MCP 媒体工具入口：以与服务器下发 MIOT 指令完全相同的 JSON 触发既有处理逻辑 ===
    // 复用 HandleControlCommand，不复制播放逻辑，保证 MIOT 与 MCP 行为一致。
    void McpPlayMusic(const std::string& keyword);
    void McpControlPlayback(const std::string& action);
    void McpPlayAlbum(const std::string& album);
    void McpPlayRadio(const std::string& station, const std::string& name = "");
    void McpPlayWeather(const std::string& city, const std::string& city_enc = "");
    void McpPlayStudy(const std::string& key, const std::string& word = "", bool course = false);
    void McpPlayUrl(const std::string& url);
    void McpPlayContinuous(const std::string& keyword);

    // 主动断开当前连接并重连（模拟自动下线），用于音乐播放结束后重置唤醒状态
    void RequestReconnect();

    // Generic stop callback is retained; study continuation uses typed round events.
    void HandlePlaybackStopped();
    void HandleStudyEvent(uint32_t generation, MusicPlayer::StudyEvent event);
    cJSON* GetStudyPlaybackJson();
    const std::string& GetStudyBootId() const { return boot_id_; }
    void ResendStudyStatus();

private:
    static constexpr const char* kServerUrl = "ws://192.168.199.162:8003/ws";
    // MCP 是唯一通道。MIOT /ws 已按迁移计划（Phase 2/3）退役：Web 控制台指令与设备
    // 状态一律经 MCP 工具（self.reboot / self.upgrade_firmware / set_volume / set_brightness /
    // get_device_status）走服务器->设备链路，不依赖本长连接。以下代码保留作回退。
    static constexpr bool kEnableMiotWs = false;
    static constexpr const char* kPlaylistApiBase = "/api/music/playlist?album=";
    static constexpr const char* kWeatherApiBase = "/api/weather_now?city=";
    static constexpr const char* kStudyImageUrlBase = "/api/study/image?key=";
    static constexpr const char* kStudyCourseNextApiBase = "/api/study/course/next?after=";
    // 学习卡片/课程起始卡：从 8001 词库服务自取 {word,key}（MCP 路径下 key 为空时自动获取，去除对 /ws 的依赖）
    static constexpr const char* kStudyCardApiBase = "/api/study/card";
    static constexpr const char* kStudyCourseFindApiBase = "/api/study/course/find";
    static constexpr const char* kStudyGifUrlBase = "/api/study/gif?key=";
    static constexpr const char* kStudyCartoonGifUrlBase = "/api/study/cartoon_gif?key=";
    // 学习卡同步字幕：LRC（时间轴文本），播放时逐行更新屏幕底部字幕
    static constexpr const char* kStudyLrcUrlBase = "/api/study/lrc?key=";
    static constexpr int kConnectId = 2;
    static constexpr int kHeartbeatIntervalMs = 30000;
    static constexpr int kStatusReportIntervalMs = 60000;
    static constexpr int kReconnectDelayMs = 5000;

    TaskHandle_t task_handle_ = nullptr;
    MusicPlayer* music_player_ = nullptr;
    std::mutex mutex_;
    bool running_ = false;
    bool stop_requested_ = false;
    std::atomic<bool> reconnect_requested_{false};
    std::mutex study_mutex_;
    std::mutex study_asset_mutex_;
    std::string boot_id_;
    std::string playback_id_;
    uint64_t study_request_id_ = 0;
    uint64_t study_seq_ = 0;
    std::string study_state_;
    std::string study_error_code_;
    std::string study_error_message_;
    std::string study_snapshot_json_ = "null";
    ivy::PlaybackGeneration study_audio_generation_;
    struct StudyWork {
        uint32_t generation;
        std::string key;
        std::string word;
        bool course;
        bool next;
        int round;
    };
    QueueHandle_t study_work_queue_ = nullptr;
    QueueHandle_t study_lrc_queue_ = nullptr;
    // Firmware-lifetime workers, each with one pending request; replacement never spawns a task.
    TaskHandle_t study_worker_ = nullptr;
    TaskHandle_t study_lrc_worker_ = nullptr;
    void DispatchStudyWork(const std::string& key, const std::string& word, bool next, int round);
    static void StudyWorkerEntry(void* arg);
    bool study_active_ = false;  // Lifecycle decisions are owned by the main task.
    cJSON* StudyPlaybackJsonLocked();
    void ReportStudyStatus(const char* state, const char* code = nullptr,
                           const char* message = nullptr);
    void FinishStudy(const char* state, const char* code = nullptr,
                     const char* message = nullptr);
    void InvalidateStudyAudio();
    // 课程连播状态：study_course 指令置位；每课播完自动续播下一课，播完全部或被打断时复位
    std::atomic<bool> course_mode_{false};
    std::atomic<bool> course_stopped_{false};
    std::string last_course_key_;
    // FaceFonics 课程（kc-001~109）需音频播两遍：第1遍对口型 GIF，第2遍对卡通 GIF。
    // Round transitions are owned by the main task, not the generic stop callback.
    std::atomic<bool> study_facefonics_{false};
    std::atomic<int> study_pass_{0};  // 0=未开始 1=第1遍(口型) 2=第2遍(卡通)
    // 学习卡字幕：音频播放时按 LRC 时间轴逐行刷新屏幕底部字幕。
    // lrc 任务代次，避免旧字幕显示重叠到下一课。
    std::atomic<uint32_t> study_lrc_generation_{0};

    void Run();
    void ConnectAndServe();
    void HandleMessage(const std::string& message);
    void HandleControlCommand(const cJSON* data);
    void TriggerDanceForSong(const std::string& keyword);

    // === 媒体引擎方法（MCP 与 MIOT /ws 共用，去除对命令字符串派发的依赖）===
    // MCP 工具直接调用这些方法；HandleControlCommand 也转发到这里，保证两条链路行为一致。
    void EnginePlayMusic(const std::string& keyword);
    void EngineControlMusic(const std::string& action);
    void EnginePlayAlbum(const std::string& album);
    void EnginePlayRadio(const std::string& station, const std::string& name);
    void EnginePlayWeather(const std::string& city, const std::string& city_enc);
    void EnginePlayStudy(const std::string& key, const std::string& word, bool course);
    void EnginePlayUrl(const std::string& url);
    void EnginePlayContinuous(const std::string& keyword);
    void EngineShowChat(const std::string& text);
    void HandleHeartbeatResponse();
    // 向服务器上报设备实时状态（版本/音量/亮度/内存/播放状态），供 Web 控制台展示
    void SendDeviceStatus(WebSocket* ws);
    // 专辑播放：从服务器拉取 /api/music/playlist?album= 歌单并连播。
    // 磁盘/HLS 之外均为 HTTP JSON 请求，失败时仅记录日志，不阻塞主链路。
    std::vector<std::string> FetchAlbumPlaylist(const std::string& album_name);
    // 天气：拉取 /api/weather_now?city= 返回中文播报文本（失败返回空串），供屏幕显示
    std::string FetchWeatherText(const std::string& city);
    // 学习卡片图片：下载词卡 JPG 并上屏（预览图，5 秒自动隐藏）；失败返回 false
    bool ShowStudyImage(const std::string& key, uint32_t generation);
    bool ShowStudyJpeg(const std::string& url, const char* label, uint32_t generation);
    // 口型/卡通动画 GIF：下载课程 GIF 整段循环播放；失败返回 false（回退封面）
    // url 为完整资源地址（口型或卡通）。
    bool ShowStudyGif(const std::string& url, uint32_t generation, int64_t deadline_ms);
    // 使用 BSD socket 直连服务器下载口型 GIF（http://host:port/path），
    // 避免 HttpClient 逐 1500B 分块解析对大文件的极慢吞吐。成功返回 true 并填充输出缓冲。
    bool DownloadStudyGifHttp(const std::string& host, int port, const std::string& path,
                              std::vector<uint8_t>& out, uint32_t generation, int64_t deadline_ms);
    bool DownloadStudyGifTls(const std::string& url, std::vector<uint8_t>& out, uint32_t generation,
                             int64_t deadline_ms);
    // 结束学习（单卡播完/课程播完/显式停止）后清理 GIF 预览，避免残留循环
    void ClearStudyPreview();
    // FaceFonics 课程（kc-001~109，有卡通 GIF 素材）返回 true；仅这类课程音频播两遍
    static bool IsFaceFonicsCourse(const std::string& key);
    // Start round two only from the generation-checked natural EOF event.
    void StartSecondStudyPass();
    // 音频开始播放时启动 LRC 字幕：后台任务拉取字幕并按时间轴刷新屏幕底部文本。
    // Started on first PCM, independently for each round.
    void StartStudyLrc(const std::string& key);
    void StopStudyLrc();
    static void StudyLrcTaskEntry(void* arg);
    void RunStudyLrc(const std::string& key, uint32_t generation);
    // 解析一行 LRC "[mm:ss.cs] text"；返回秒级时间并输出文本；非时间行返回 -1
    static bool ParseLrcLine(const std::string& line, int& seconds, std::string& text);
    // 更新屏幕底部字幕（空串隐藏）；主任务安全的薄包装
    void ShowStudySubtitle(const std::string& text, uint32_t generation = 0);
    // Distinguish exhausted courses from HTTP/JSON failures.
    enum class NextCourseResult { Next, Done, Failed };
    NextCourseResult FetchNextCourse(const std::string& after, std::string& word, std::string& key,
                                    uint32_t generation);
    std::string FetchStudyBody(const std::string& url, uint32_t generation);
    // 随机学习卡片：GET /api/study/card -> {word,key}；成功返回 true
    bool FetchStudyCard(std::string& word, std::string& key, uint32_t generation);
    // 课程起始卡：GET /api/study/course/find?text= -> {word,key}；成功返回 true
    bool FetchStudyCourse(const std::string& text, std::string& word, std::string& key,
                          uint32_t generation);
    // Main-task dispatch only; lookup and assets are handled by the preparation worker.
    void ContinueCourse(const std::string& after);
    static std::string UrlEncode(const std::string& input);

    static void TaskEntry(void* arg);
};

#endif // MIOT_CLIENT_H
