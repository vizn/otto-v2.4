#ifndef MIOT_CLIENT_H
#define MIOT_CLIENT_H

#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>

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

    // 主动断开当前连接并重连（模拟自动下线），用于音乐播放结束后重置唤醒状态
    void RequestReconnect();

private:
    static constexpr const char* kServerUrl = "ws://192.168.199.162:8003/ws";
    static constexpr int kConnectId = 2;
    static constexpr int kHeartbeatIntervalMs = 30000;
    static constexpr int kReconnectDelayMs = 5000;

    TaskHandle_t task_handle_ = nullptr;
    MusicPlayer* music_player_ = nullptr;
    std::mutex mutex_;
    bool running_ = false;
    bool stop_requested_ = false;
    std::atomic<bool> reconnect_requested_{false};

    void Run();
    void ConnectAndServe();
    void HandleMessage(const std::string& message);
    void HandleControlCommand(const cJSON* data);
    void TriggerDanceForSong(const std::string& keyword);
    void HandleHeartbeatResponse();

    static void TaskEntry(void* arg);
};

#endif // MIOT_CLIENT_H
