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
#include <vector>

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
    // 网络电台：播放服务器转码的电台流（stream 复用 HTTP->MP3->codec 链路）
    void PlayRadio(const std::string& station);
    // 天气播报：播放服务器 EdgeTTS 合成的天气音频
    void PlayWeather(const std::string& city);
    // 学习卡片：播放本地词库一张单词卡的音频（短音频，播完自然结束）
    void PlayStudyCard(const std::string& key);
    // 歌单连播：按次序逐首播放 keywords（每首经 /api/music/stream?song=<kw>）。
    // 全部播完（或被停止）后触发 on_stopped 回调；播放中可再入队 next/previous/stop。
    void PlayPlaylist(const std::vector<std::string>& keywords);
    void Pause();
    void Resume();
    void StopPlay();
    // 歌单模式下一首/上一首（非歌单模式忽略）
    void Next();
    void Previous();
    void ShowMessage(const std::string& text);

    void SetAudioCodec(AudioCodec* codec) { codec_ = codec; }
    bool IsPlaying() const { return playing_; }
    // 播放结束（自然播完或被停止）时回调，用于结束跟随音乐的舞蹈等联动
    void SetOnStoppedCallback(std::function<void()> cb) { on_stopped_ = std::move(cb); }

private:
    static constexpr const char* kStreamUrlBase = "http://192.168.199.162:8003/api/music/stream?song=";
    static constexpr const char* kRadioStreamUrlBase = "http://192.168.199.162:8003/api/radio/stream?station=";
    static constexpr const char* kWeatherTtsUrlBase = "http://192.168.199.162:8003/api/weather_tts?city=";
    static constexpr const char* kStudyStreamUrlBase = "http://192.168.199.162:8003/api/study/stream?key=";
    static constexpr int kCommandQueueDepth = 8;

    enum class CommandType : uint8_t {
        kPlay,
        kPlaylist,
        kPlayRadio,
        kPlayWeather,
        kPlayStudyCard,
        kPause,
        kResume,
        kStop,
        kNext,
        kPrevious,
        kShowMessage,
    };

    struct Command {
        CommandType type;
        char payload[128];
        void* extra = nullptr;  // kPlaylist: std::vector<std::string>*（所有权转移给播放任务）
    };

    TaskHandle_t task_handle_ = nullptr;
    QueueHandle_t command_queue_ = nullptr;
    AudioCodec* codec_ = nullptr;
    std::function<void()> on_stopped_;
    volatile bool running_ = false;
    volatile bool playing_ = false;
    volatile bool paused_ = false;
    volatile bool stop_requested_ = false;
    // 歌单状态（仅播放任务访问）
    std::vector<std::string> playlist_;
    size_t playlist_index_ = 0;
    volatile bool playlist_mode_ = false;
    volatile bool pending_next_ = false;
    volatile bool pending_prev_ = false;
    volatile bool single_play_requested_ = false;
    volatile bool pending_playlist_ = false;
    volatile bool wake_word_suspended_ = false;

    static void TaskEntry(void* arg);
    void Run();
    void ProcessCommand(const Command& cmd);
    // 播放类型，决定流 URL 前缀：music / radio / weather / study 词卡
    enum class StreamMode : uint8_t { kMusic, kRadio, kWeather, kStudy };
    void PlayStream(const std::string& keyword, StreamMode mode);
    void Enqueue(const Command& cmd);
    void NotifyStopped();
    void ShowMessageOnScreen(const char* text);
    void PlayPlaylistInternal(std::vector<std::string> playlist);
    std::string DisplaySongName(const std::string& keyword);
    // 播放期间暂停/恢复唤醒词识别，避免本地音乐被 LLM/唤醒打断
    void SuspendWakeWordForPlayback();
    void ResumeWakeWordForPlayback();

    static std::string UrlEncode(const std::string& input);
};

#endif // MUSIC_PLAYER_H
