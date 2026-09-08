#include "music_player.h"

#include <stdint.h>
#include <esp_ae_rate_cvt.h>
#include <esp_audio_types.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_audio_dec.h>
#include <esp_mp3_dec.h>
#include <cctype>
#include <cstring>

#include "board.h"
#include "display/display.h"
#include "audio_codec.h"
#include "http.h"
#include "network_interface.h"
#include "system_info.h"
#include "application.h"

#define TAG "MusicPlayer"

MusicPlayer::MusicPlayer() = default;

MusicPlayer::~MusicPlayer() { Stop(); }

void MusicPlayer::Start() {
    if (running_) {
        return;
    }
    running_ = true;
    command_queue_ = xQueueCreate(kCommandQueueDepth, sizeof(Command));
    xTaskCreate(TaskEntry, "music_player", 8192, this, 5, &task_handle_);
    ESP_LOGI(TAG, "Music player started");
}

void MusicPlayer::Stop() {
    if (!running_) {
        return;
    }
    running_ = false;
    stop_requested_ = true;
    if (command_queue_ != nullptr) {
        Command cmd{CommandType::kStop, {0}};
        xQueueSend(command_queue_, &cmd, pdMS_TO_TICKS(100));
    }
    if (task_handle_ != nullptr) {
        vTaskDelete(task_handle_);
        task_handle_ = nullptr;
    }
    if (command_queue_ != nullptr) {
        vQueueDelete(command_queue_);
        command_queue_ = nullptr;
    }
    playing_ = false;
    paused_ = false;
    ESP_LOGI(TAG, "Music player stopped");
}

void MusicPlayer::PlayByKeyword(const std::string& keyword) {
    Command cmd{CommandType::kPlay, {0}};
    strncpy(cmd.payload, keyword.c_str(), sizeof(cmd.payload) - 1);
    Enqueue(cmd);
}

void MusicPlayer::PlayRadio(const std::string& station) {
    Command cmd{CommandType::kPlayRadio, {0}};
    strncpy(cmd.payload, station.c_str(), sizeof(cmd.payload) - 1);
    Enqueue(cmd);
}

void MusicPlayer::PlayWeather(const std::string& city) {
    Command cmd{CommandType::kPlayWeather, {0}};
    strncpy(cmd.payload, city.c_str(), sizeof(cmd.payload) - 1);
    Enqueue(cmd);
}

void MusicPlayer::PlayStudyCard(const std::string& key) {
    Command cmd{CommandType::kPlayStudyCard, {0}};
    strncpy(cmd.payload, key.c_str(), sizeof(cmd.payload) - 1);
    Enqueue(cmd);
}

void MusicPlayer::PlayPlaylist(const std::vector<std::string>& keywords) {
    if (keywords.empty()) {
        return;
    }
    Command cmd{CommandType::kPlaylist, {0}};
    cmd.payload[0] = '\0';
    cmd.extra = new std::vector<std::string>(keywords);  // 所有权转移给播放任务
    Enqueue(cmd);
}

void MusicPlayer::Next() {
    Command cmd{CommandType::kNext, {0}};
    Enqueue(cmd);
}

void MusicPlayer::Previous() {
    Command cmd{CommandType::kPrevious, {0}};
    Enqueue(cmd);
}

void MusicPlayer::Pause() {
    Command cmd{CommandType::kPause, {0}};
    Enqueue(cmd);
}

void MusicPlayer::Resume() {
    Command cmd{CommandType::kResume, {0}};
    Enqueue(cmd);
}

void MusicPlayer::StopPlay() {
    Command cmd{CommandType::kStop, {0}};
    Enqueue(cmd);
}

void MusicPlayer::ShowMessage(const std::string& text) {
    Command cmd{CommandType::kShowMessage, {0}};
    strncpy(cmd.payload, text.c_str(), sizeof(cmd.payload) - 1);
    Enqueue(cmd);
}

void MusicPlayer::Enqueue(const Command& cmd) {
    if (command_queue_ == nullptr) {
        return;
    }
    // 队列满时丢弃最旧的播放命令，确保新命令不被阻塞
    if (xQueueSend(command_queue_, &cmd, 0) != pdTRUE) {
        Command dummy;
        xQueueReceive(command_queue_, &dummy, 0);
        xQueueSend(command_queue_, &cmd, 0);
    }
}

void MusicPlayer::TaskEntry(void* arg) {
    auto* player = static_cast<MusicPlayer*>(arg);
    player->Run();
}

void MusicPlayer::Run() {
    Command cmd;
    while (running_) {
        // 空闲时阻塞等待命令
        if (xQueueReceive(command_queue_, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        switch (cmd.type) {
            case CommandType::kPlay:
                // 单曲播放：结束（自然播完/停止/切歌终止）后由 PlayStream 统一通知联动
                single_play_requested_ = false;
                pending_playlist_ = false;
                playlist_mode_ = false;
                PlayStream(cmd.payload, StreamMode::kMusic);
                break;
            case CommandType::kPlayRadio:
                // 网络电台：持续流，播放后由停止/切歌终止
                single_play_requested_ = false;
                pending_playlist_ = false;
                playlist_mode_ = false;
                PlayStream(cmd.payload, StreamMode::kRadio);
                break;
            case CommandType::kPlayWeather:
                // 天气播报（服务器 TTS 一次性音频），结束后通知
                single_play_requested_ = false;
                pending_playlist_ = false;
                playlist_mode_ = false;
                PlayStream(cmd.payload, StreamMode::kWeather);
                break;
            case CommandType::kPlayStudyCard:
                // 学习卡片（词库一次性音频）
                single_play_requested_ = false;
                pending_playlist_ = false;
                playlist_mode_ = false;
                PlayStream(cmd.payload, StreamMode::kStudy);
                break;
            case CommandType::kPlaylist: {
                std::vector<std::string>* pl =
                    static_cast<std::vector<std::string>*>(cmd.extra);
                cmd.extra = nullptr;
                pending_playlist_ = false;
                single_play_requested_ = false;
                if (pl != nullptr) {
                    PlayPlaylistInternal(std::move(*pl));
                    delete pl;
                } else {
                    playlist_mode_ = false;
                }
                break;
            }
            case CommandType::kPause:
                paused_ = true;
                break;
            case CommandType::kResume:
                paused_ = false;
                break;
            case CommandType::kStop:
                stop_requested_ = true;
                playing_ = false;
                paused_ = false;
                // 歌单模式下停止由 PlayPlaylistInternal 收尾统一通知，
                // 避免与连播循环退出时的通知重复触发重连。
                if (!playlist_mode_) {
                    NotifyStopped();
                }
                playlist_mode_ = false;
                break;
            case CommandType::kNext:
                pending_next_ = true;
                pending_prev_ = false;
                break;
            case CommandType::kPrevious:
                pending_prev_ = true;
                pending_next_ = false;
                break;
            case CommandType::kShowMessage:
                ESP_LOGI(TAG, "Message: %.120s", cmd.payload);
                ShowMessageOnScreen(cmd.payload);
                break;
        }
    }
    vTaskDelete(nullptr);
}

// 歌单连播主循环：首曲后逐首 PlayStream，直到停止/切歌/歌单播完。
void MusicPlayer::PlayPlaylistInternal(std::vector<std::string> playlist) {
    playlist_ = std::move(playlist);
    playlist_mode_ = true;
    playlist_index_ = 0;
    if (playlist_.empty()) {
        playlist_mode_ = false;
        NotifyStopped();
        return;
    }
    stop_requested_ = false;
    pending_next_ = false;
    pending_prev_ = false;
    ShowMessageOnScreen(DisplaySongName(playlist_[0]).c_str());
    PlayStream(playlist_[0], StreamMode::kMusic);
    while (playlist_mode_ && running_) {
        // 停止（无切歌意图）或新单曲/新歌单介入：终止当前歌单
        if (stop_requested_ && !pending_next_ && !pending_prev_) {
            break;
        }
        if (single_play_requested_ || pending_playlist_) {
            break;
        }
        int next_index = -1;
        if (pending_next_) {
            pending_next_ = false;
            stop_requested_ = false;
            next_index = static_cast<int>(playlist_index_) + 1;
        } else if (pending_prev_) {
            pending_prev_ = false;
            stop_requested_ = false;
            next_index = static_cast<int>(playlist_index_) - 1;
        } else {
            next_index = static_cast<int>(playlist_index_) + 1;
        }
        if (next_index < 0 || next_index >= static_cast<int>(playlist_.size())) {
            break;
        }
        playlist_index_ = next_index;
        ShowMessageOnScreen(DisplaySongName(playlist_[playlist_index_]).c_str());
        PlayStream(playlist_[playlist_index_], StreamMode::kMusic);
    }
    playlist_mode_ = false;
    NotifyStopped();
}

// 屏幕显示用歌曲名：去掉专辑路径、扩展名与曲序前缀（如 "64. " / "3.音频-"）。
std::string MusicPlayer::DisplaySongName(const std::string& keyword) {
    size_t slash = keyword.find_last_of('/');
    std::string base = (slash == std::string::npos) ? keyword : keyword.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) {
        base = base.substr(0, dot);
    }
    // 去除开头的曲序前缀：连续数字 + '.'（可选尾随空格），如 "64. " / "3.音频-"
    size_t i = 0;
    while (i < base.size() && std::isdigit(static_cast<unsigned char>(base[i]))) {
        i++;
    }
    if (i > 0 && i < base.size() && base[i] == '.') {
        base = base.substr(i + 1);
        size_t j = 0;
        while (j < base.size() && base[j] == ' ') {
            j++;
        }
        if (j > 0) {
            base = base.substr(j);
        }
    }
    return base;
}

// ---------- MP3 帧头解析 ----------
// 从 buf[start..] 中寻找合法 MPEG 帧头。找到时返回 true，并输出：
//   frame_len 帧总长度（含帧头）；skip 从 start 到帧头的垃圾字节数（应丢弃）
// 未找到（含数据不足）返回 false。仅支持 Layer1/2/3、MPEG1/2/2.5。
static bool FindNextMp3Frame(const std::vector<uint8_t>& buf, size_t start, size_t& frame_len, size_t& skip) {
    static const uint16_t kBitrateV1[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    static const uint16_t kBitrateV2[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    static const uint32_t kSampleRateV1[4] = {44100, 48000, 32000, 0};
    static const uint32_t kSampleRateV2[4] = {22050, 24000, 16000, 0};
    static const uint32_t kSampleRateV25[4] = {11025, 12000, 8000, 0};

    for (size_t i = start; i + 4 <= buf.size(); i++) {
        if (buf[i] != 0xFF || (buf[i + 1] & 0xE0) != 0xE0) {
            continue;
        }
        int ver = (buf[i + 1] >> 3) & 0x3;    // 0=MPEG2.5 1=reserved 2=MPEG2 3=MPEG1
        int layer = (buf[i + 1] >> 1) & 0x3;  // 1=Layer3 2=Layer2 3=Layer1
        if (ver == 1 || layer == 0) {
            continue;
        }
        int br_idx = (buf[i + 2] >> 4) & 0xF;
        int sr_idx = (buf[i + 2] >> 2) & 0x3;
        if (br_idx == 0 || br_idx == 15 || sr_idx == 3) {
            continue;
        }
        uint16_t bitrate_kbps = (ver == 3) ? kBitrateV1[br_idx] : kBitrateV2[br_idx];
        uint32_t sample_rate = (ver == 3) ? kSampleRateV1[sr_idx]
                              : (ver == 2) ? kSampleRateV2[sr_idx]
                                           : kSampleRateV25[sr_idx];
        if (bitrate_kbps == 0 || sample_rate == 0) {
            continue;
        }
        int padding = (buf[i + 2] >> 1) & 0x1;
        uint32_t rate = bitrate_kbps * 1000u;
        uint32_t bytes;
        if (layer == 3) {  // Layer1
            bytes = (12u * rate / sample_rate + padding) * 4u;
        } else {           // Layer2/3
            bytes = (ver == 3 ? 144u : 72u) * rate / sample_rate + padding;
        }
        if (bytes < 4u || bytes > 16384u) {
            continue;
        }
        skip = i - start;
        frame_len = bytes;
        return true;
    }
    return false;
}

void MusicPlayer::PlayStream(const std::string& keyword, StreamMode mode) {
    if (codec_ == nullptr) {
        ESP_LOGW(TAG, "No audio codec");
        NotifyStopped();
        return;
    }

    auto* network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        ESP_LOGW(TAG, "No network interface");
        NotifyStopped();
        return;
    }

    stop_requested_ = false;
    std::string keyword_to_play = keyword;
    bool restart = true;
    const uint32_t dest_rate = codec_->output_sample_rate();

    // 播放开始：暂停唤醒词检测，防止本地音乐播放被 LLM/唤醒语音打断
    SuspendWakeWordForPlayback();

    // 空闲超时后 AudioService 会关闭 I2S 输出(掉电)；本地播放需自行打开，
    // 否则 i2s_channel_write 会在未运行的通道上永久阻塞导致无声。
    bool output_enabled_by_music = false;
    if (!codec_->output_enabled()) {
        codec_->EnableOutput(true);
        output_enabled_by_music = true;
    }
    auto& audio_service = Application::GetInstance().GetAudioService();
    int64_t last_keepalive_ms = esp_timer_get_time() / 1000;
    audio_service.NotifyOutputActivity();

    StreamMode active_mode = mode;
    const char* url_base =
        (active_mode == StreamMode::kRadio)    ? kRadioStreamUrlBase
        : (active_mode == StreamMode::kWeather) ? kWeatherTtsUrlBase
        : (active_mode == StreamMode::kStudy)   ? kStudyStreamUrlBase
                                                : kStreamUrlBase;

    while (restart && running_ && !stop_requested_) {
            restart = false;
            std::string url = url_base + UrlEncode(keyword_to_play);
            // 携带设备 MAC，便于服务端回传真实歌名（修正 random/模糊匹配导致的屏显不一致）
            url += "&mac=" + UrlEncode(SystemInfo::GetMacAddress());
            const char* mode_str = (mode == StreamMode::kMusic) ? "music"
                               : (mode == StreamMode::kRadio) ? "radio"
                               : (mode == StreamMode::kStudy) ? "study" : "weather";
        ESP_LOGI(TAG, "Playing stream (%s): %s", mode_str, keyword_to_play.c_str());

        auto http = network->CreateHttp(3);
        if (http == nullptr) {
            ESP_LOGE(TAG, "Failed to create http client");
            break;
        }
        http->SetTimeout(30000);
        http->SetHeader("User-Agent", "ESP32-MIOT/1.0");
        http->SetHeader("X-Device-Mac", SystemInfo::GetMacAddress().c_str());
        http->SetHeader("Accept-Encoding", "identity");

        if (!http->Open("GET", url)) {
            ESP_LOGE(TAG, "HTTP open failed");
            http->Close();
            break;
        }
        int status = http->GetStatusCode();
        if (status != 200) {
            ESP_LOGE(TAG, "HTTP status %d, song not found", status);
            http->Close();
            break;
        }

        // 打开 MP3 解码器（逐帧喂入：自行解析帧头、确认完整帧后才调用）
        void* decoder = nullptr;
        if (esp_mp3_dec_open(nullptr, 0, &decoder) != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "Failed to open MP3 decoder");
            http->Close();
            break;
        }

        esp_ae_rate_cvt_handle_t resampler = nullptr;
        uint32_t src_rate = 0;
        bool need_resample = false;

        constexpr size_t kReadChunk = 4096;
        constexpr size_t kDecodeBufferSize = 8192;
        std::vector<uint8_t> encoded_buffer;
        encoded_buffer.reserve(kReadChunk);
        std::vector<uint8_t> pcm_buffer(kDecodeBufferSize);
        std::vector<int16_t> mono_buffer;
        std::vector<int16_t> resample_buffer;

        playing_ = true;
        paused_ = false;

        // 音乐/电台播放时，屏幕仅显示当前歌名（歌词需服务端另行提供）。
        // random/空关键词由服务端回传真实歌名，避免屏显出现 "random"。
        if (mode == StreamMode::kMusic || mode == StreamMode::kRadio) {
            std::string disp = DisplaySongName(keyword_to_play);
            if (!disp.empty() && disp != "random") {
                ShowMessageOnScreen(disp.c_str());
            }
        }

        char chunk[kReadChunk];
        int read_result;
        bool stream_ended = false;

        while (!stop_requested_ && !stream_ended) {
            // 处理队列中的控制命令（不阻塞 HTTP 读取）
            Command cmd;
            while (xQueueReceive(command_queue_, &cmd, 0) == pdTRUE) {
                switch (cmd.type) {
                    case CommandType::kStop:
                        stop_requested_ = true;
                        break;
                    case CommandType::kPause:
                        paused_ = true;
                        break;
                    case CommandType::kResume:
                        paused_ = false;
                        break;
                    case CommandType::kShowMessage:
                        ESP_LOGI(TAG, "Message: %.120s", cmd.payload);
                        ShowMessageOnScreen(cmd.payload);
                        break;
                    case CommandType::kPlay:
                        // 新歌曲命令：记录关键词，停止当前流后重新开始
                        single_play_requested_ = true;  // 接管歌单
                        keyword_to_play = cmd.payload;
                        restart = true;
                        stop_requested_ = true;
                        break;
                    case CommandType::kPlayRadio:
                        // 切换电台：更新目标，重启当前流
                        active_mode = StreamMode::kRadio;
                        url_base = kRadioStreamUrlBase;
                        keyword_to_play = cmd.payload;
                        restart = true;
                        stop_requested_ = true;
                        break;
                    case CommandType::kPlayWeather:
                        active_mode = StreamMode::kWeather;
                        url_base = kWeatherTtsUrlBase;
                        keyword_to_play = cmd.payload;
                        restart = true;
                        stop_requested_ = true;
                        break;
                    case CommandType::kPlayStudyCard:
                        active_mode = StreamMode::kStudy;
                        url_base = kStudyStreamUrlBase;
                        keyword_to_play = cmd.payload;
                        restart = true;
                        stop_requested_ = true;
                        break;
                    case CommandType::kPlaylist:
                        // 新歌单命令：让当前流尽快结束，交由 Run 处理新歌单
                        pending_playlist_ = true;
                        stop_requested_ = true;
                        break;
                    case CommandType::kNext:
                        pending_next_ = true;
                        stop_requested_ = true;
                        break;
                    case CommandType::kPrevious:
                        pending_prev_ = true;
                        stop_requested_ = true;
                        break;
                }
            }
            if (stop_requested_) {
                break;
            }

            // 暂停时丢弃数据但不关闭连接
            if (paused_) {
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }

            // 定期刷新 AudioService 输出活动时间，防止掉电定时器在播放中关闭 I2S
            {
                int64_t now_ms = esp_timer_get_time() / 1000;
                if (now_ms - last_keepalive_ms >= 3000) {
                    last_keepalive_ms = now_ms;
                    audio_service.NotifyOutputActivity();
                }
            }

            read_result = http->Read(chunk, sizeof(chunk));
            if (read_result > 0) {
                encoded_buffer.insert(encoded_buffer.end(), chunk, chunk + read_result);

                // 自行解析 MP3 帧头，仅在有完整帧时才喂给解码器；
                // 这样不会把半个帧交给解码器（避免错误 12 风暴），
                // 数据不足时保留在缓冲区等待更多数据。
                size_t offset = 0;
                while (offset < encoded_buffer.size()) {
                    size_t frame_len = 0, skip = 0;
                    if (!FindNextMp3Frame(encoded_buffer, offset, frame_len, skip)) {
                        // 无合法帧头或数据不足：等待更多数据
                        break;
                    }
                    if (skip > 0) {
                        offset += skip;
                    }
                    if (offset + frame_len > encoded_buffer.size()) {
                        // 帧数据不完整：等待更多数据
                        break;
                    }

                    esp_audio_dec_in_raw_t raw = {};
                    raw.buffer = encoded_buffer.data() + offset;
                    // 喂入自帧头起的全部剩余数据：解码器消费首帧，帧后数据供位池使用
                    raw.len = static_cast<uint32_t>(encoded_buffer.size() - offset);
                    raw.consumed = 0;
                    raw.frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE;

                    esp_audio_dec_out_frame_t frame = {};
                    frame.buffer = pcm_buffer.data();
                    frame.len = static_cast<uint32_t>(pcm_buffer.size());
                    frame.needed_size = 0;
                    frame.decoded_size = 0;

                    esp_audio_dec_info_t dec_info = {};
                    esp_audio_err_t ret = esp_mp3_dec_decode(decoder, &raw, &frame, &dec_info);
                    if (ret == ESP_AUDIO_ERR_OK) {
                        offset += (raw.consumed > 0) ? raw.consumed : frame_len;

                        if (frame.decoded_size > 0) {
                            // 首次获取采样率时初始化重采样器
                            if (src_rate == 0 && dec_info.sample_rate != dest_rate) {
                                src_rate = dec_info.sample_rate;
                                need_resample = true;
                                esp_ae_rate_cvt_cfg_t cfg = {};
                                cfg.src_rate = src_rate;
                                cfg.dest_rate = dest_rate;
                                cfg.channel = 1;
                                cfg.bits_per_sample = 16;
                                cfg.complexity = 3;
                                cfg.perf_type = ESP_AE_RATE_CVT_PERF_TYPE_MEMORY;
                                if (esp_ae_rate_cvt_open(&cfg, &resampler) == ESP_AE_ERR_OK) {
                                    ESP_LOGI(TAG, "Resampler %u -> %u Hz", src_rate, dest_rate);
                                } else {
                                    resampler = nullptr;
                                }
                            }

                            int channels = dec_info.channel > 0 ? dec_info.channel : 2;
                            int sample_count = frame.decoded_size / 2;  // 16-bit

                            // 立体声降混为单声道
                            if (channels >= 2) {
                                mono_buffer.resize(sample_count / 2);
                                const int16_t* pcm = reinterpret_cast<const int16_t*>(frame.buffer);
                                for (size_t i = 0; i < mono_buffer.size(); i++) {
                                    mono_buffer[i] = static_cast<int16_t>(
                                        (static_cast<int32_t>(pcm[2 * i]) + pcm[2 * i + 1]) / 2);
                                }
                            } else {
                                mono_buffer.resize(sample_count);
                                memcpy(mono_buffer.data(), frame.buffer, frame.decoded_size);
                            }

                            std::vector<int16_t>* out = &mono_buffer;
                            if (need_resample && resampler != nullptr) {
                                uint32_t in_samples = mono_buffer.size();
                                uint32_t max_out = 0;
                                if (esp_ae_rate_cvt_get_max_out_sample_num(resampler, in_samples, &max_out) == ESP_AE_ERR_OK &&
                                    max_out > 0) {
                                    resample_buffer.resize(max_out);
                                    uint32_t out_samples = max_out;
                                    esp_ae_sample_t in = mono_buffer.data();
                                    esp_ae_sample_t out_p = resample_buffer.data();
                                    if (esp_ae_rate_cvt_process(resampler, in, in_samples, out_p, &out_samples) == ESP_AE_ERR_OK &&
                                        out_samples > 0) {
                                        resample_buffer.resize(out_samples);
                                        out = &resample_buffer;
                                    }
                                }
                            }

                            // 输出到音频 codec（16-bit 单声道 @ dest_rate）
                            // 兜底：若空闲定时器已关输出，重新打开
                            if (!codec_->output_enabled()) {
                                codec_->EnableOutput(true);
                            }
                            codec_->OutputData(*out);
                        }
                    } else if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                        // 输出缓冲区不足，扩容后重试
                        if (frame.needed_size > pcm_buffer.size()) {
                            pcm_buffer.resize(frame.needed_size);
                        }
                        // 不推进 offset，重试同一帧
                    } else {
                        // 完整帧仍解码失败：跳过该帧继续，避免死循环
                        ESP_LOGW(TAG, "mp3 frame decode error %d, skip frame at %u", ret, (unsigned)offset);
                        offset += frame_len;
                    }

                    // 防止死循环：如果无法推进则退出内层循环
                    if (offset >= encoded_buffer.size()) {
                        break;
                    }
                }

                // 清除已消费的数据（保留末尾不足一帧的部分等待补充）
                if (offset > 0) {
                    encoded_buffer.erase(encoded_buffer.begin(), encoded_buffer.begin() + offset);
                }
            } else if (read_result == 0) {
                stream_ended = true;
                ESP_LOGI(TAG, "Stream ended");
            } else {
                ESP_LOGW(TAG, "HTTP read error");
                break;
            }
        }

        // 清理本首歌的资源
        if (resampler != nullptr) {
            esp_ae_rate_cvt_close(resampler);
        }
        if (decoder != nullptr) {
            esp_mp3_dec_close(decoder);
        }
        http->Close();

        if (restart) {
            // 切歌：继续播放下一首
            stop_requested_ = false;
        }
    }

    playing_ = false;
    paused_ = false;

    // 播放结束：若输出是本播放器打开的，恢复掉电状态
    if (output_enabled_by_music && !restart) {
        codec_->EnableOutput(false);
    }

    ESP_LOGI(TAG, "Playback finished");
    // 歌单连播模式下（PlayPlaylistInternal 内部驱动逐首播放），单首结束不触发停止回调，
    // 由连播主循环统一决定继续下一首或整体停止后再回调，否则首曲播完就会断开 MIOT 导致连播中断。
    if (!playlist_mode_) {
        NotifyStopped();
    }
}

void MusicPlayer::NotifyStopped() {
    // 播放彻底结束：恢复唤醒词检测，允许用户再次用语音唤醒交互
    ResumeWakeWordForPlayback();
    if (on_stopped_ != nullptr) {
        on_stopped_();
    }
}

void MusicPlayer::SuspendWakeWordForPlayback() {
    if (wake_word_suspended_) {
        return;
    }
    wake_word_suspended_ = true;
    auto& audio_service = Application::GetInstance().GetAudioService();
    if (audio_service.IsWakeWordRunning()) {
        audio_service.EnableWakeWordDetection(false);
        ESP_LOGI(TAG, "本地音乐播放中，暂停唤醒词检测");
    }
}

void MusicPlayer::ResumeWakeWordForPlayback() {
    if (!wake_word_suspended_) {
        return;
    }
    wake_word_suspended_ = false;
    auto& audio_service = Application::GetInstance().GetAudioService();
    audio_service.EnableWakeWordDetection(true);
    ESP_LOGI(TAG, "本地音乐播放结束，恢复唤醒词检测");
}

void MusicPlayer::ShowMessageOnScreen(const char* text) {
    auto& app = Application::GetInstance();
    std::string message = text != nullptr ? text : "";
    app.Schedule([message]() {
        ESP_LOGI(TAG, "ShowMessageOnScreen executing, message=[%s]", message.c_str());
        auto* display = Board::GetInstance().GetDisplay();
        if (display != nullptr) {
            display->SetChatMessage("system", message.c_str());
        }
    });
}

std::string MusicPlayer::UrlEncode(const std::string& input) {
    const char* hex = "0123456789ABCDEF";
    std::string output;
    for (unsigned char c : input) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            output += static_cast<char>(c);
        } else {
            output += '%';
            output += hex[(c >> 4) & 0xF];
            output += hex[c & 0xF];
        }
    }
    return output;
}
