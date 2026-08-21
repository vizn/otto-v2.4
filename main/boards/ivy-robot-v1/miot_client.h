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

class MusicPlayer;

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
    void McpControlPlayback(const std::string& action);  // play|pause|next|previous|stop
    void McpPlayAlbum(const std::string& album);
    void McpPlayRadio(const std::string& station, const std::string& name = "");
    void McpPlayWeather(const std::string& city, const std::string& city_enc = "");
    void McpPlayStudy(const std::string& key, const std::string& word = "", bool course = false);

    // 主动断开当前连接并重连（模拟自动下线），用于音乐播放结束后重置唤醒状态
    void RequestReconnect();

    // 任一本地播放（音乐/电台/天气/学习卡片/课程连播）结束后调用（音乐播放器任务内）。
    // 课程连播模式下会自动续播下一课；否则解除学习卡片静音并重连。
    void HandlePlaybackStopped();

private:
    static constexpr const char* kServerUrl = "ws://192.168.199.162:8003/ws";
    // Phase 4: 迁移到 MCP 为主通道后，默认关闭设备对 kServerUrl(/ws) 的主动连接。
    // MIOT /ws 通道代码（ConnectAndServe / HandleMessage / HandleControlCommand 等）全部保留作回退，
    // 将本开关置 true 即可恢复设备侧 /ws 连接，无需改动其他逻辑。
    static constexpr bool kEnableMiotWs = false;
    static constexpr const char* kPlaylistApiBase = "http://192.168.199.162:8003/api/music/playlist?album=";
    static constexpr const char* kWeatherApiBase = "http://192.168.199.162:8003/api/weather_now?city=";
    static constexpr const char* kStudyImageUrlBase = "http://192.168.199.162:8003/api/study/image?key=";
    static constexpr const char* kStudyCourseNextApiBase = "http://192.168.199.162:8003/api/study/course/next?after=";
    // 学习卡片/课程起始卡：从 8001 词库服务自取 {word,key}（MCP 路径下 key 为空时自动获取，去除对 /ws 的依赖）
    static constexpr const char* kStudyCardApiBase = "http://192.168.199.162:8001/api/study/card";
    static constexpr const char* kStudyCourseFindApiBase = "http://192.168.199.162:8001/api/study/course/find";
    static constexpr int kConnectId = 2;
    static constexpr int kHeartbeatIntervalMs = 30000;
    static constexpr int kReconnectDelayMs = 5000;

    TaskHandle_t task_handle_ = nullptr;
    MusicPlayer* music_player_ = nullptr;
    std::mutex mutex_;
    bool running_ = false;
    bool stop_requested_ = false;
    std::atomic<bool> reconnect_requested_{false};
    // 课程连播状态：study_course 指令置位；每课播完自动续播下一课，播完全部或被打断时复位
    std::atomic<bool> course_mode_{false};
    std::atomic<bool> course_stopped_{false};
    std::string last_course_key_;

    void Run();
    void ConnectAndServe();
    void HandleMessage(const std::string& message);
    void HandleControlCommand(const cJSON* data);
    void TriggerDanceForSong(const std::string& keyword);

    // === 媒体引擎方法（MCP 与 MIOT /ws 共用，去除对命令字符串派发的依赖）===
    // MCP 工具直接调用这些方法；HandleControlCommand 也转发到这里，保证两条链路行为一致。
    void EnginePlayMusic(const std::string& keyword);
    void EngineControlMusic(const std::string& action);  // play|pause|next|previous|stop
    void EnginePlayAlbum(const std::string& album);
    void EnginePlayRadio(const std::string& station, const std::string& name);
    void EngineShowChat(const std::string& text);
    void EnginePlayWeather(const std::string& city, const std::string& city_enc);
    void EnginePlayStudy(const std::string& key, const std::string& word, bool course);
    void HandleHeartbeatResponse();
    // 专辑播放：从服务器拉取 /api/music/playlist?album= 歌单并连播。
    // 磁盘/HLS 之外均为 HTTP JSON 请求，失败时仅记录日志，不阻塞主链路。
    std::vector<std::string> FetchAlbumPlaylist(const std::string& album_name);
    // 天气：拉取 /api/weather_now?city= 返回中文播报文本（失败返回空串），供屏幕显示
    std::string FetchWeatherText(const std::string& city);
    // 学习卡片图片：下载词卡 JPG 并上屏（预览图，5 秒自动隐藏）；失败返回 false
    bool ShowStudyImage(const std::string& key);
    // 课程连播：拉取 /api/study/course/next?after= 的下一课；成功返回 true 并输出 word/key
    bool FetchNextCourse(const std::string& after, std::string& word, std::string& key);
    // 随机学习卡片：GET /api/study/card -> {word,key}；成功返回 true
    bool FetchStudyCard(std::string& word, std::string& key);
    // 课程起始卡：GET /api/study/course/find?text= -> {word,key}；成功返回 true
    bool FetchStudyCourse(const std::string& text, std::string& word, std::string& key);
    // 主任务内续播下一课（显示封面 -> 播放音频）；课程结束/失败时退出连播并复位
    void ContinueCourse(const std::string& after);
    static std::string UrlEncode(const std::string& input);

    static void TaskEntry(void* arg);
};

#endif // MIOT_CLIENT_H
