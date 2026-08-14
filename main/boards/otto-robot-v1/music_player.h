#ifndef MUSIC_PLAYER_H
#define MUSIC_PLAYER_H

#include <stdint.h>
#include <esp_ae_rate_cvt.h>
#include <esp_audio_types.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <functional>
#include <memory>
#include <string>

class AudioCodec;

/**
 * @brief MIOT 音乐播放器
 *
 * 播放链路：HTTP 流式下载 MP3 -> esp_audio_codec MP3 解码 -> 立体声降混为单声道
 *          -> esp_ae_rate_cvt 重采样到 codec 输出采样率 -> AudioCodec::OutputData
 *
 * 拥有独立任务和命令队列，WS 回调只入队，绝不阻塞。
 */
class MusicPlayer {
public:
    MusicPlayer();
    ~MusicPlayer();

    void Start();
    void Stop();

    // 以下方法可从任意任务调用（仅入队）
    void PlayByKeyword(const std::string& keyword);
    void Pause();
    void Resume();
    void StopPlay();
    void ShowMessage(const std::string& text);

    void SetAudioCodec(AudioCodec* codec) { codec_ = codec; }
    bool IsPlaying() const { return playing_; }
    // 播放结束（自然播完或被停止）时回调，用于结束跟随音乐的舞蹈等联动
    void SetOnStoppedCallback(std::function<void()> cb) { on_stopped_ = std::move(cb); }

private:
    static constexpr const char* kStreamUrlBase = "http://192.168.199.162:8003/api/music/stream?song=";
    static constexpr int kCommandQueueDepth = 8;

    enum class CommandType : uint8_t {
        kPlay,
        kPause,
        kResume,
        kStop,
        kShowMessage,
    };

    struct Command {
        CommandType type;
        char payload[128];
    };

    TaskHandle_t task_handle_ = nullptr;
    QueueHandle_t command_queue_ = nullptr;
    AudioCodec* codec_ = nullptr;
    std::function<void()> on_stopped_;
    volatile bool running_ = false;
    volatile bool playing_ = false;
    volatile bool paused_ = false;
    volatile bool stop_requested_ = false;

    static void TaskEntry(void* arg);
    void Run();
    void ProcessCommand(const Command& cmd);
    void PlayStream(const std::string& keyword);
    void Enqueue(const Command& cmd);
    void NotifyStopped();
    void ShowMessageOnScreen(const char* text);

    static std::string UrlEncode(const std::string& input);
};

#endif // MUSIC_PLAYER_H
