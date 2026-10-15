#include "miot_client.h"

#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <cstdio>
#include <esp_heap_caps.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "board.h"
#include "application.h"
#include "display.h"
#include "wifi_manager.h"
#include "display/lcd_display.h"
#include "display/lvgl_display/lvgl_display.h"
#include "display/lvgl_display/lvgl_image.h"
#include "jpg/jpeg_to_image.h"
#include "http.h"
#include "miot_client.h"
#include "music_player.h"
#include "network_interface.h"
#include "otto_controller.h"
#include "system_info.h"
#include "web_socket.h"

#define TAG "MiotClient"

// 解析"边跳舞边放<歌曲名>"指令（定义见文件尾部，供 HandleControlCommand 使用）
static bool ParseDanceWithMusic(const std::string& text, std::string& song);

namespace {
// LRC 字幕后台任务参数
struct StudyLrcTaskArgs {
    MiotClient* client;
    std::string key;
    uint32_t generation;
};
}  // namespace

MiotClient::MiotClient() {
    char id[17];
    snprintf(id, sizeof(id), "%08lx%08lx", static_cast<unsigned long>(esp_random()),
             static_cast<unsigned long>(esp_random()));
    boot_id_ = id;
}

cJSON* MiotClient::StudyPlaybackJsonLocked() {
    auto* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "boot_id", boot_id_.c_str());
    cJSON_AddStringToObject(root, "playback_id", playback_id_.c_str());
    cJSON_AddNumberToObject(root, "seq", study_seq_);
    cJSON_AddStringToObject(root, "state", study_state_.c_str());
    cJSON_AddStringToObject(root, "key", last_course_key_.c_str());
    cJSON_AddNumberToObject(root, "round", std::max(1, study_pass_.load()));
    cJSON_AddNumberToObject(root, "total_rounds", study_facefonics_.load() ? 2 : 1);
    cJSON_AddBoolToObject(root, "course", course_mode_.load());
    if (study_error_code_.empty()) {
        cJSON_AddNullToObject(root, "error");
    } else {
        auto* error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "code", study_error_code_.c_str());
        cJSON_AddStringToObject(error, "message", study_error_message_.c_str());
        cJSON_AddItemToObject(root, "error", error);
    }
    return root;
}

cJSON* MiotClient::GetStudyPlaybackJson() {
    std::lock_guard<std::mutex> lock(study_mutex_);
    return cJSON_Parse(study_snapshot_json_.c_str());
}

void MiotClient::ReportStudyStatus(const char* state, const char* code, const char* message) {
    {
        std::lock_guard<std::mutex> lock(study_mutex_);
        study_state_ = state;
        study_error_code_ = code ? code : "";
        study_error_message_ = message ? message : "";
        ++study_seq_;
        auto* snapshot = StudyPlaybackJsonLocked();
        char* json = cJSON_PrintUnformatted(snapshot);
        if (json) {
            study_snapshot_json_ = json;
            cJSON_free(json);
        }
        cJSON_Delete(snapshot);
    }
    ResendStudyStatus();
}

void MiotClient::ResendStudyStatus() {
    auto* snapshot = GetStudyPlaybackJson();
    if (!snapshot || cJSON_IsNull(snapshot)) {
        cJSON_Delete(snapshot);
        return;
    }
    auto* notification = cJSON_CreateObject();
    cJSON_AddStringToObject(notification, "jsonrpc", "2.0");
    cJSON_AddStringToObject(notification, "method", "notifications/ivy/study_status");
    cJSON_AddItemToObject(notification, "params", snapshot);
    char* json = cJSON_PrintUnformatted(notification);
    if (json) {
        Application::GetInstance().SendMcpMessage(json);
        cJSON_free(json);
    }
    cJSON_Delete(notification);
}

void MiotClient::InvalidateStudyAudio() {
    // Serialize token invalidation with the final image/GIF commit, never with HTTP.
    std::lock_guard<std::mutex> lock(study_asset_mutex_);
    uint32_t generation = study_audio_generation_.Advance();
    if (music_player_) music_player_->SetStudyGeneration(generation);
}

void MiotClient::FinishStudy(const char* state, const char* code, const char* message) {
    if (!study_active_) return;
    ReportStudyStatus(state, code, message);
    study_active_ = false;
    InvalidateStudyAudio();
    course_mode_.store(false);
    course_stopped_.store(true);
    study_facefonics_.store(false);
    study_pass_.store(0);
    Application::GetInstance().SetStudyCardPlaying(false);
    ClearStudyPreview();
    RequestReconnect();
}

void MiotClient::HandleStudyEvent(uint32_t generation, MusicPlayer::StudyEvent event) {
    Application::GetInstance().Schedule([this, generation, event]() {
        if (!study_active_ || !study_audio_generation_.Matches(generation)) return;
        switch (event) {
            case MusicPlayer::StudyEvent::Playing: {
                bool first_pcm = study_state_ == "loading";
                ReportStudyStatus("playing");
                if (first_pcm) StartStudyLrc(last_course_key_);
                break;
            }
            case MusicPlayer::StudyEvent::Paused:
                ReportStudyStatus("paused");
                break;
            case MusicPlayer::StudyEvent::Failed:
                FinishStudy("failed", "audio_failed", "Study audio playback failed.");
                break;
            case MusicPlayer::StudyEvent::Stopped:
                FinishStudy("stopped");
                break;
            case MusicPlayer::StudyEvent::Completed:
                if (study_facefonics_.load() && study_pass_.load() == 1) {
                    StartSecondStudyPass();
                } else if (course_mode_.load()) {
                    InvalidateStudyAudio();
                    ClearStudyPreview();
                    ReportStudyStatus("loading");
                    ContinueCourse(last_course_key_);
                } else {
                    FinishStudy("completed");
                }
                break;
        }
    });
}

MiotClient::~MiotClient() {
    Stop();
}

void MiotClient::Start() {
    if (!kEnableMiotWs) {
        ESP_LOGI(TAG, "MIOT /ws 通道已禁用(Phase 4)，保留代码作回退，不发起 %s 连接", kServerUrl);
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) {
        return;
    }
    running_ = true;
    stop_requested_ = false;
    xTaskCreate(TaskEntry, "miot_client", 4096, this, 5, &task_handle_);
    ESP_LOGI(TAG, "MIOT client started");
}

void MiotClient::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return;
        }
        stop_requested_ = true;
        running_ = false;
    }
    if (task_handle_ != nullptr) {
        vTaskDelete(task_handle_);
        task_handle_ = nullptr;
    }
    ESP_LOGI(TAG, "MIOT client stopped");
}

void MiotClient::RequestReconnect() {
    reconnect_requested_.store(true);
    ESP_LOGI(TAG, "Reconnect requested");
}

void MiotClient::TaskEntry(void* arg) {
    auto* client = static_cast<MiotClient*>(arg);
    client->Run();
}

void MiotClient::Run() {
    while (!stop_requested_) {
        ConnectAndServe();
        for (int i = 0; i < kReconnectDelayMs / 100 && !stop_requested_; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    vTaskDelete(nullptr);
}

void MiotClient::ConnectAndServe() {
    auto& board = Board::GetInstance();
    auto* network = board.GetNetwork();
    if (network == nullptr) {
        ESP_LOGW(TAG, "No network interface");
        return;
    }

    auto ws = network->CreateWebSocket(kConnectId);
    if (ws == nullptr) {
        ESP_LOGW(TAG, "Failed to create websocket");
        return;
    }

    ws->SetHeader("User-Agent", "ESP32-MIOT/1.0");
    ws->SetReceiveBufferSize(4096);

    std::string mac = SystemInfo::GetMacAddress();
    std::string device_id = "esp32_" + mac;

    ws->OnData([this](const char* data, size_t len, bool binary) {
        (void)binary;
        if (data == nullptr || len == 0) {
            return;
        }
        HandleMessage(std::string(data, len));
    });

    ws->OnDisconnected([this]() {
        ESP_LOGW(TAG, "WebSocket disconnected");
    });

    if (!ws->Connect(kServerUrl)) {
        ESP_LOGE(TAG, "Failed to connect to %s, error=%d", kServerUrl, ws->GetLastError());
        return;
    }
    ESP_LOGI(TAG, "WebSocket connected to %s", kServerUrl);

    // 注册设备（hello 认证）
    cJSON* hello = cJSON_CreateObject();
    cJSON* hello_data = cJSON_CreateObject();
    cJSON_AddStringToObject(hello, "type", "hello");
    cJSON_AddStringToObject(hello_data, "clientType", "esp32");
    cJSON_AddStringToObject(hello_data, "device_id", device_id.c_str());
    cJSON_AddStringToObject(hello_data, "mac", mac.c_str());
    cJSON_AddItemToObject(hello, "data", hello_data);
    char* hello_str = cJSON_PrintUnformatted(hello);
    if (hello_str != nullptr) {
        ws->Send(hello_str);
        cJSON_free(hello_str);
    }
    cJSON_Delete(hello);

    // 心跳循环：发送 JSON ping -> 服务器返回 JSON pong；周期上报设备状态供 Web 控制台展示
    TickType_t last_heartbeat = xTaskGetTickCount();
    TickType_t last_status = last_heartbeat;
    while (!stop_requested_) {
        if (reconnect_requested_.exchange(false)) {
            ESP_LOGI(TAG, "Reconnect requested, disconnecting");
            break;
        }
        TickType_t now = xTaskGetTickCount();
        if (now - last_heartbeat >= pdMS_TO_TICKS(kHeartbeatIntervalMs)) {
            ws->Send("{\"type\":\"ping\"}");
            last_heartbeat = now;
        }
        if (now - last_status >= pdMS_TO_TICKS(kStatusReportIntervalMs)) {
            SendDeviceStatus(ws.get());
            last_status = now;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
        if (!ws->IsConnected()) {
            ESP_LOGW(TAG, "WebSocket lost connection");
            break;
        }
    }

    ws->Close();
}

void MiotClient::HandleMessage(const std::string& message) {
    cJSON* root = cJSON_Parse(message.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "Failed to parse message: %.200s", message.c_str());
        return;
    }

    cJSON* type = cJSON_GetObjectItem(root, "type");
    if (type == nullptr || !cJSON_IsString(type)) {
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type->valuestring, "hello_ack") == 0) {
        ESP_LOGI(TAG, "Device registered: %.200s", message.c_str());
    } else if (strcmp(type->valuestring, "device_online") == 0) {
        ESP_LOGI(TAG, "Device online acknowledged");
    } else if (strcmp(type->valuestring, "device_control_command") == 0) {
        ESP_LOGI(TAG, "收到控制指令原始消息: %.400s", message.c_str());
        cJSON* data = cJSON_GetObjectItem(root, "data");
        if (data != nullptr) {
            HandleControlCommand(data);
        }
    } else if (strcmp(type->valuestring, "pong") == 0) {
        HandleHeartbeatResponse();
    } else if (strcmp(type->valuestring, "error") == 0) {
        ESP_LOGW(TAG, "Server error: %.200s", message.c_str());
    } else {
        ESP_LOGD(TAG, "Unhandled message: %.200s", message.c_str());
    }

    cJSON_Delete(root);
}

void MiotClient::HandleControlCommand(const cJSON* data) {
    cJSON* cmd_type = cJSON_GetObjectItem(data, "type");
    if (cmd_type == nullptr || !cJSON_IsString(cmd_type)) {
        ESP_LOGW(TAG, "Control command missing type");
        return;
    }
    const char* type = cmd_type->valuestring;
    ESP_LOGI(TAG, "Control command: %s", type);

    // 系统控制类指令不依赖 music_player_，优先处理（Web 控制台经 MIOT 网关下发）
    if (strcmp(type, "reboot") == 0) {
        auto& app = Application::GetInstance();
        app.Schedule([&app]() { app.Reboot(); });
        return;
    } else if (strcmp(type, "ota") == 0) {
        auto& app = Application::GetInstance();
        cJSON* url = cJSON_GetObjectItem(data, "url");
        std::string url_str = (url != nullptr && cJSON_IsString(url)) ? url->valuestring : "";
        if (!url_str.empty()) {
            app.Schedule([url_str, &app]() { app.UpgradeFirmware(url_str); });
        } else {
            app.RequestUpgradeFromServer();
        }
        return;
    } else if (strcmp(type, "volume") == 0) {
        cJSON* value = cJSON_GetObjectItem(data, "value");
        if (value != nullptr && cJSON_IsNumber(value)) {
            auto& board = Board::GetInstance();
            auto codec = board.GetAudioCodec();
            if (codec != nullptr) {
                codec->SetOutputVolume(value->valueint);
                ESP_LOGI(TAG, "Set volume to %d", value->valueint);
            }
        }
        return;
    } else if (strcmp(type, "brightness") == 0) {
        cJSON* value = cJSON_GetObjectItem(data, "value");
        if (value != nullptr && cJSON_IsNumber(value)) {
            auto& board = Board::GetInstance();
            auto backlight = board.GetBacklight();
            if (backlight != nullptr) {
                backlight->SetBrightness(static_cast<uint8_t>(value->valueint), true);
                ESP_LOGI(TAG, "Set brightness to %d", value->valueint);
            }
        }
        return;
    }

    if (music_player_ == nullptr) {
        return;
    }

    // MIOT /ws 命令派发：转发到媒体引擎方法（与 MCP 工具共用同一套逻辑）
    if (strcmp(type, "music") == 0) {
        cJSON* text = cJSON_GetObjectItem(data, "text");
        if (text != nullptr && cJSON_IsString(text)) {
            EnginePlayMusic(text->valuestring);
        }
    } else if (strcmp(type, "music_control") == 0) {
        cJSON* action = cJSON_GetObjectItem(data, "action");
        if (action != nullptr && cJSON_IsString(action)) {
            EngineControlMusic(action->valuestring);
        }
    } else if (strcmp(type, "album") == 0) {
        cJSON* album = cJSON_GetObjectItem(data, "album");
        if (album != nullptr && cJSON_IsString(album)) {
            EnginePlayAlbum(album->valuestring);
        }
    } else if (strcmp(type, "chat") == 0) {
        cJSON* text = cJSON_GetObjectItem(data, "text");
        if (text != nullptr && cJSON_IsString(text)) {
            EngineShowChat(text->valuestring);
        }
    } else if (strcmp(type, "radio") == 0) {
        cJSON* station = cJSON_GetObjectItem(data, "station");
        cJSON* name = cJSON_GetObjectItem(data, "name");
        if (station != nullptr && cJSON_IsString(station)) {
            EnginePlayRadio(station->valuestring,
                            (name != nullptr && cJSON_IsString(name)) ? name->valuestring : "");
        }
    } else if (strcmp(type, "weather") == 0) {
        cJSON* city = cJSON_GetObjectItem(data, "city");
        cJSON* enc = cJSON_GetObjectItem(data, "city_enc");
        std::string city_name = (city != nullptr && cJSON_IsString(city)) ? city->valuestring : "";
        std::string city_enc_str =
            (enc != nullptr && cJSON_IsString(enc)) ? enc->valuestring : UrlEncode(city_name);
        EnginePlayWeather(city_name, city_enc_str);
    } else if (strcmp(type, "study_card") == 0 || strcmp(type, "study_course") == 0) {
        bool course = strcmp(type, "study_course") == 0;
        cJSON* word = cJSON_GetObjectItem(data, "word");
        cJSON* key = cJSON_GetObjectItem(data, "key");
        std::string word_str = (word != nullptr && cJSON_IsString(word)) ? word->valuestring : "";
        std::string key_str = (key != nullptr && cJSON_IsString(key)) ? key->valuestring : "";
        if (!key_str.empty()) {
            EnginePlayStudy(key_str, word_str, course);
        }
    }
}

// === 媒体引擎方法：MCP 工具与 MIOT /ws 共用，去除对命令字符串派发的依赖 ===

void MiotClient::EnginePlayMusic(const std::string& keyword) {
    FinishStudy("stopped");
    if (music_player_ == nullptr) return;
    ESP_LOGI(TAG, "播放音乐关键词: [%s]", keyword.c_str());

    // 边跳舞边放<歌曲名>：随机/指定音乐 + 随机舞蹈
    std::string play_keyword;
    if (ParseDanceWithMusic(keyword, play_keyword)) {
        music_player_->PlayByKeyword(play_keyword);
        ESP_LOGI(TAG, "「%s」触发随机音乐舞蹈，播放歌曲: %s", keyword.c_str(), play_keyword.c_str());
        OttoControllerQueueContinuousDance();
    } else {
        music_player_->PlayByKeyword(keyword);

        // 用户原话含跳舞意图（服务器只下发纯歌名，用 stt 原文兜底）
        // 同时匹配英文指令（dance/groove 等），大小写不敏感
        auto& app = Application::GetInstance();
        const std::string& user_text = app.GetLastUserText();
        std::string lower_text = user_text;
        std::transform(lower_text.begin(), lower_text.end(), lower_text.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (lower_text.find("跳舞") != std::string::npos ||
            lower_text.find("舞蹈") != std::string::npos ||
            lower_text.find("边跳边放") != std::string::npos ||
            lower_text.find("跳着") != std::string::npos ||
            lower_text.find("dance") != std::string::npos ||
            lower_text.find("dancing") != std::string::npos ||
            lower_text.find("danced") != std::string::npos ||
            lower_text.find("groove") != std::string::npos ||
            lower_text.find("party") != std::string::npos ||
            lower_text.find("shake a leg") != std::string::npos) {
            ESP_LOGI(TAG, "用户原话「%s」含跳舞意图，触发连续舞蹈", user_text.c_str());
            OttoControllerQueueContinuousDance();
            return;
        }

        // 音乐+动作联动：播放花花舞相关歌曲时触发舞蹈编排
        TriggerDanceForSong(keyword);
    }
}

void MiotClient::EngineControlMusic(const std::string& action) {
    if (music_player_ == nullptr) return;
    if (action == "play") {
        music_player_->Resume();
    } else if (action == "pause") {
        music_player_->Pause();
    } else if (action == "stop") {
        FinishStudy("stopped");
        // 显式停止会打断课程连播：禁止播完后自动续播下一课
        course_mode_.store(false);
        course_stopped_.store(true);
        ClearStudyPreview();
        music_player_->StopPlay();
    } else if (action == "next") {
        FinishStudy("stopped");
        course_mode_.store(false);
        course_stopped_.store(true);
        ClearStudyPreview();
        music_player_->Next();
    } else if (action == "previous") {
        FinishStudy("stopped");
        course_mode_.store(false);
        course_stopped_.store(true);
        ClearStudyPreview();
        music_player_->Previous();
    }
}

void MiotClient::EnginePlayAlbum(const std::string& album) {
    FinishStudy("stopped");
    if (music_player_ == nullptr) return;
    ESP_LOGI(TAG, "收到专辑播放指令: [%s]", album.c_str());
    std::vector<std::string> playlist = FetchAlbumPlaylist(album);
    if (playlist.empty()) {
        ESP_LOGW(TAG, "专辑「%s」歌单为空，无法播放", album.c_str());
    } else {
        ESP_LOGI(TAG, "专辑「%s」共 %d 首，开始连播", album.c_str(), (int)playlist.size());
        music_player_->PlayPlaylist(std::move(playlist));
    }
}

void MiotClient::EngineShowChat(const std::string& text) {
    if (music_player_ == nullptr) return;
    music_player_->ShowMessage(text);
}

void MiotClient::EnginePlayRadio(const std::string& station, const std::string& name) {
    FinishStudy("stopped");
    if (music_player_ == nullptr) return;
    ESP_LOGI(TAG, "收到电台播放指令: [%s]", station.c_str());
    music_player_->PlayRadio(station);
    if (!name.empty()) {
        std::string shown = "电台：" + name;
        music_player_->ShowMessage(shown);
    }
}

void MiotClient::EnginePlayWeather(const std::string& city, const std::string& city_enc) {
    FinishStudy("stopped");
    if (music_player_ == nullptr) return;
    ESP_LOGI(TAG, "收到天气播报指令: [%s]", city.c_str());
    // 先拉取天气文本上屏（复用音乐播放器的消息显示链路）
    std::string weather_text = FetchWeatherText(city);
    if (!weather_text.empty()) {
        music_player_->ShowMessage(weather_text);
    }
    // 再播放服务器 EdgeTTS 合成的天气播报音频；city_enc 缺省时用 city 做 URL 编码兜底
    std::string enc = city_enc.empty() ? UrlEncode(city) : city_enc;
    music_player_->PlayWeather(enc);
}

void MiotClient::EnginePlayStudy(const std::string& key, const std::string& word, bool course) {
    FinishStudy("stopped");
    if (music_player_) music_player_->StopPlay();
    InvalidateStudyAudio();
    study_active_ = true;
    playback_id_ = boot_id_ + "-" + std::to_string(++study_request_id_);
    last_course_key_ = key;
    course_mode_.store(course);
    course_stopped_.store(false);
    study_pass_.store(1);
    study_facefonics_.store(IsFaceFonicsCourse(key));
    ReportStudyStatus("loading");
    if (music_player_ == nullptr) {
        FinishStudy("failed", "audio_failed", "Study audio playback failed.");
        return;
    }

    // 学习卡片播放期间禁用 LLM：置位标志（丢弃出站/入站音频）并中止当前会话。
    // 唤醒词暂停仅关闭唤醒检测，不停已在进行的监听会话，故需显式停听。
    auto& app = Application::GetInstance();
    app.SetStudyCardPlaying(true);
    auto st = app.GetDeviceState();
    if (st == kDeviceStateSpeaking || st == kDeviceStateListening) {
        app.AbortSpeaking(kAbortReasonNone);
    }
    if (st == kDeviceStateListening) app.StopListening();
    while (app.GetAudioService().PopPacketFromSendQueue()) {
    }
    OttoControllerStopAll();
    StopStudyLrc();
    ShowStudySubtitle("");
    DispatchStudyWork(key, word, false, 1);
}

void MiotClient::DispatchStudyWork(const std::string& key, const std::string& word,
                                   bool next, int round) {
    if (!study_worker_) {
        study_work_queue_ = xQueueCreate(1, sizeof(StudyWork*));
        if (!study_work_queue_ ||
            xTaskCreate(StudyWorkerEntry, "study_prepare", 8192, this, 5, &study_worker_) != pdPASS) {
            if (study_work_queue_) vQueueDelete(study_work_queue_);
            study_work_queue_ = nullptr;
            study_worker_ = nullptr;
            FinishStudy("failed", "preparation_failed", "Study preparation is unavailable.");
            return;
        }
    }
    auto* work = new StudyWork{study_audio_generation_.Current(), key, word,
                               course_mode_.load(), next, round};
    StudyWork* old = nullptr;
    if (xQueueReceive(study_work_queue_, &old, 0) == pdTRUE) delete old;
    if (xQueueSend(study_work_queue_, &work, 0) != pdTRUE) {
        delete work;
        FinishStudy("failed", "preparation_failed", "Study preparation is unavailable.");
    }
}

void MiotClient::StudyWorkerEntry(void* arg) {
    auto* client = static_cast<MiotClient*>(arg);
    StudyWork* pending;
    for (;;) {
        if (xQueueReceive(client->study_work_queue_, &pending, portMAX_DELAY) != pdTRUE) continue;
        std::unique_ptr<StudyWork> work(pending);
        const uint32_t generation = work->generation;
        auto current = [client, generation]() { return client->study_audio_generation_.Matches(generation); };
        if (!current()) continue;
        NextCourseResult result = NextCourseResult::Next;
        if (work->next) {
            std::string after = work->key;
            work->key.clear();
            result = client->FetchNextCourse(after, work->word, work->key, generation);
        } else if (work->key.empty()) {
            const std::string text = work->word;
            bool ok = work->course ? client->FetchStudyCourse(text, work->word, work->key, generation)
                                   : client->FetchStudyCard(work->word, work->key, generation);
            if (!ok) result = NextCourseResult::Failed;
        }
        if (!current()) continue;
        const bool next = work->next;
        if (result != NextCourseResult::Next || work->key.empty()) {
            Application::GetInstance().Schedule([client, generation, result, next]() {
                if (!client->study_active_ || !client->study_audio_generation_.Matches(generation)) return;
                if (result == NextCourseResult::Done) client->FinishStudy("completed");
                else if (next) client->FinishStudy("failed", "next_course_failed", "Next study lesson lookup failed.");
                else client->FinishStudy("failed", "card_failed", "Study card lookup failed.");
            });
            continue;
        }
        const std::string key = work->key;
        const std::string word = work->word;
        const int round = work->round;
        Application::GetInstance().Schedule([client, generation, key, round]() {
            if (!client->study_active_ || !client->study_audio_generation_.Matches(generation)) return;
            client->last_course_key_ = key;
            client->study_pass_.store(round);
            client->study_facefonics_.store(IsFaceFonicsCourse(key));
            client->ReportStudyStatus("loading");
        });
        if (!current()) continue;
        if (round == 1) client->ShowStudyImage(key, generation);
        if (!current()) continue;
        const char* base = round == 2 ? kStudyCartoonGifUrlBase : kStudyGifUrlBase;
        int64_t deadline_ms = esp_timer_get_time() / 1000 + 15000;
        for (int attempt = 0; attempt < 3 && current() && esp_timer_get_time() / 1000 < deadline_ms; ++attempt) {
            if (client->ShowStudyGif(std::string(base) + UrlEncode(key), generation, deadline_ms)) break;
            for (int delay = 0; attempt < 2 && delay < 2000 && current() &&
                 esp_timer_get_time() / 1000 < deadline_ms; delay += 50) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }
        if (!current()) continue;
        Application::GetInstance().Schedule([client, generation, key, word]() {
            if (!client->study_active_ || !client->study_audio_generation_.Matches(generation)) return;
            if (!word.empty()) client->music_player_->ShowMessage("学习卡片：" + word);
            if (!client->music_player_->PlayStudyCard(key, generation)) {
                client->FinishStudy("failed", "audio_queue_failed", "Study audio queue is unavailable.");
            }
        });
    }
}

// 解析"边跳舞边放<歌曲名>"指令。命中返回 true 并输出实际要播放的歌曲关键词。
// 未指定具体歌曲（如"边跳舞边放音乐"）时使用 "random" 让服务器随机选歌。
static bool ParseDanceWithMusic(const std::string& text, std::string& song) {
    const std::string prefix = "边跳舞边放";
    size_t pos = text.find(prefix);
    if (pos == std::string::npos) {
        return false;
    }
    std::string rest = text.substr(pos + prefix.size());
    // 去掉前导的"播/播放/首"等词，兼容"边跳舞边播放音乐"（UTF-8 安全）
    const char* strip_words[] = {"播放", "播", "首", " "};
    for (const char* word : strip_words) {
        size_t wlen = strlen(word);
        while (rest.compare(0, wlen, word) == 0) {
            rest.erase(0, wlen);
        }
    }
    if (rest.empty() || rest == "音乐" || rest == "随机音乐" || rest == "随机" ||
        rest == "歌" || rest == "歌曲" || rest == "首歌" || rest == "一首歌") {
        song = "random";
    } else {
        song = rest;
    }
    return true;
}

void MiotClient::TriggerDanceForSong(const std::string& keyword) {
    if (keyword.empty() || !OttoControllerAvailable()) {
        return;
    }

    // 花花舞：《花园种花》及其相关关键词触发编排舞蹈
    static const char* kFlowerDanceKeywords[] = {
        "花园种花", "种花", "花花舞", "花",
    };
    for (const char* kw : kFlowerDanceKeywords) {
        if (keyword.find(kw) != std::string::npos) {
            ESP_LOGI(TAG, "播放「%s」触发花花舞", keyword.c_str());
            OttoControllerQueueFlowerDance();
            return;
        }
    }

    // 打节拍：节拍/拍手/律动类歌曲触发
    static const char* kBeatKeepingKeywords[] = {
        "节拍", "拍手", "Clap", "clap",
    };
    for (const char* kw : kBeatKeepingKeywords) {
        if (keyword.find(kw) != std::string::npos) {
            ESP_LOGI(TAG, "播放「%s」触发打节拍", keyword.c_str());
            OttoControllerQueueBeatKeeping();
            return;
        }
    }
}

void MiotClient::HandleHeartbeatResponse() {
    // pong 已收到，连接保持
}

void MiotClient::SendDeviceStatus(WebSocket* ws) {
    if (ws == nullptr || !ws->IsConnected()) {
        return;
    }
    auto& board = Board::GetInstance();
    auto* codec = board.GetAudioCodec();

    cJSON* root = cJSON_CreateObject();
    cJSON* data = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "device_status");
    cJSON_AddStringToObject(data, "version", PROJECT_VER);
    if (codec != nullptr) {
        cJSON_AddNumberToObject(data, "volume", codec->output_volume());
    }
    auto* backlight = board.GetBacklight();
    if (backlight != nullptr) {
        cJSON_AddNumberToObject(data, "brightness", backlight->brightness());
    }
    cJSON_AddNumberToObject(data, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(data, "min_free_heap",
                            heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    if (music_player_ != nullptr) {
        cJSON_AddBoolToObject(data, "music_playing", music_player_->IsPlaying());
    }

    int level = 0;
    bool charging = false, discharging = false;
    if (board.GetBatteryLevel(level, charging, discharging)) {
        auto* battery = cJSON_CreateObject();
        cJSON_AddNumberToObject(battery, "level", level);
        cJSON_AddBoolToObject(battery, "charging", charging);
        cJSON_AddItemToObject(data, "battery", battery);
    }

    auto& wifi = WifiManager::GetInstance();
    auto* network = cJSON_CreateObject();
    cJSON_AddStringToObject(network, "type", "wifi");
    cJSON_AddStringToObject(network, "ssid", wifi.GetSsid().c_str());
    int rssi = wifi.GetRssi();
    const char* signal = rssi >= -60 ? "strong" : (rssi >= -70 ? "medium" : "weak");
    cJSON_AddStringToObject(network, "signal", signal);
    cJSON_AddItemToObject(data, "network", network);

    float temp = 0.0f;
    if (board.GetTemperature(temp)) {
        auto* chip = cJSON_CreateObject();
        cJSON_AddNumberToObject(chip, "temperature", temp);
        cJSON_AddItemToObject(data, "chip", chip);
    }

    cJSON_AddItemToObject(root, "data", data);

    char* msg = cJSON_PrintUnformatted(root);
    if (msg != nullptr) {
        ws->Send(msg);
        cJSON_free(msg);
    }
    cJSON_Delete(root);
}

std::string MiotClient::UrlEncode(const std::string& input) {
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

std::vector<std::string> MiotClient::FetchAlbumPlaylist(const std::string& album_name) {
    std::vector<std::string> songs;
    auto& board = Board::GetInstance();
    auto* network = board.GetNetwork();
    if (network == nullptr) {
        ESP_LOGW(TAG, "No network interface for album fetch");
        return songs;
    }

    std::string url = kPlaylistApiBase + UrlEncode(album_name);
    auto http = network->CreateHttp(3);
    if (http == nullptr) {
        ESP_LOGE(TAG, "Failed to create http client for album fetch");
        return songs;
    }
    http->SetTimeout(15000);
    http->SetHeader("User-Agent", "ESP32-MIOT/1.0");
    http->SetHeader("X-Device-Mac", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Accept-Encoding", "identity");

    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "Album fetch HTTP open failed: %s", url.c_str());
        http->Close();
        return songs;
    }
    int status = http->GetStatusCode();
    if (status != 200) {
        ESP_LOGE(TAG, "Album fetch HTTP status %d", status);
        http->Close();
        return songs;
    }

    std::string body = http->ReadAll();
    http->Close();
    ESP_LOGI(TAG, "Album fetch response %d bytes", (int)body.size());

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "Album fetch invalid JSON");
        return songs;
    }
    cJSON* songs_arr = cJSON_GetObjectItem(root, "songs");
    if (songs_arr != nullptr && cJSON_IsArray(songs_arr)) {
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, songs_arr) {
            if (cJSON_IsString(item) && item->valuestring != nullptr &&
                strlen(item->valuestring) > 0) {
                songs.emplace_back(item->valuestring);
            }
        }
    }
    cJSON_Delete(root);
    return songs;
}

std::string MiotClient::FetchWeatherText(const std::string& city) {
    std::string text;
    if (city.empty()) {
        return text;
    }
    auto& board = Board::GetInstance();
    auto* network = board.GetNetwork();
    if (network == nullptr) {
        ESP_LOGW(TAG, "No network interface for weather fetch");
        return text;
    }
    std::string url = kWeatherApiBase + UrlEncode(city);
    auto http = network->CreateHttp(3);
    if (http == nullptr) {
        ESP_LOGE(TAG, "Failed to create http client for weather fetch");
        return text;
    }
    http->SetTimeout(15000);
    http->SetHeader("User-Agent", "ESP32-MIOT/1.0");
    http->SetHeader("X-Device-Mac", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Accept-Encoding", "identity");
    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "Weather fetch HTTP open failed: %s", url.c_str());
        http->Close();
        return text;
    }
    int status = http->GetStatusCode();
    if (status != 200) {
        ESP_LOGE(TAG, "Weather fetch HTTP status %d", status);
        http->Close();
        return text;
    }
    std::string body = http->ReadAll();
    http->Close();
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "Weather fetch invalid JSON");
        return text;
    }
    cJSON* t = cJSON_GetObjectItem(root, "text");
    if (t != nullptr && cJSON_IsString(t) && t->valuestring != nullptr) {
        text = t->valuestring;
    }
    cJSON_Delete(root);
    return text;
}

std::string MiotClient::FetchStudyBody(const std::string& url, uint32_t generation) {
    if (!study_audio_generation_.Matches(generation)) return "";
    auto* network = Board::GetInstance().GetNetwork();
    if (!network) return "";
    auto http = network->CreateHttp(3);
    if (!http) return "";
    const int64_t deadline_ms = esp_timer_get_time() / 1000 + 15000;
    http->SetTimeout(15000);
    http->SetHeader("User-Agent", "ESP32-MIOT/1.0");
    http->SetHeader("X-Device-Mac", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Accept-Encoding", "identity");
    if (!http->Open("GET", url) || http->GetStatusCode() != 200) {
        http->Close();
        return "";
    }
    std::string body;
    const size_t expected = http->GetBodyLength();
    bool complete = false;
    char buffer[1024];
    while (study_audio_generation_.Matches(generation) && esp_timer_get_time() / 1000 < deadline_ms) {
        // Read cannot distinguish cancellation polling timeout from a transport failure.
        int remaining_ms = ivy::RemainingReadBudgetMs(deadline_ms, esp_timer_get_time() / 1000);
        if (!remaining_ms) break;
        http->SetTimeout(remaining_ms);
        int count = http->Read(buffer, sizeof(buffer));
        if (count == 0) {
            complete = !expected || body.size() == expected;
            break;
        }
        if (count < 0 || body.size() + count > 64 * 1024) break;
        body.append(buffer, count);
        if (expected && body.size() == expected) {
            complete = true;
            break;
        }
    }
    http->Close();
    return complete && study_audio_generation_.Matches(generation) ? body : "";
}

bool MiotClient::FetchStudyCard(std::string& word, std::string& key, uint32_t generation) {
    std::string body = FetchStudyBody(kStudyCardApiBase, generation);
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "Study card fetch invalid JSON");
        return false;
    }
    cJSON* w = cJSON_GetObjectItem(root, "word");
    cJSON* k = cJSON_GetObjectItem(root, "key");
    if (k != nullptr && cJSON_IsString(k) && k->valuestring != nullptr && strlen(k->valuestring) > 0) {
        key = k->valuestring;
        if (w != nullptr && cJSON_IsString(w) && w->valuestring != nullptr) {
            word = w->valuestring;
        }
        cJSON_Delete(root);
        return true;
    }
    cJSON_Delete(root);
    return false;
}

bool MiotClient::FetchStudyCourse(const std::string& text, std::string& word, std::string& key,
                                 uint32_t generation) {
    std::string url = kStudyCourseFindApiBase;
    if (!text.empty()) {
        url += "?text=" + UrlEncode(text);
    }
    std::string body = FetchStudyBody(url, generation);
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "Study course fetch invalid JSON");
        return false;
    }
    cJSON* w = cJSON_GetObjectItem(root, "word");
    cJSON* k = cJSON_GetObjectItem(root, "key");
    if (k != nullptr && cJSON_IsString(k) && k->valuestring != nullptr && strlen(k->valuestring) > 0) {
        key = k->valuestring;
        if (w != nullptr && cJSON_IsString(w) && w->valuestring != nullptr) {
            word = w->valuestring;
        }
        cJSON_Delete(root);
        return true;
    }
    cJSON_Delete(root);
    return false;
}

bool MiotClient::ShowStudyImage(const std::string& key, uint32_t generation) {
    if (key.empty()) {
        return false;
    }
    return ShowStudyJpeg(kStudyImageUrlBase + UrlEncode(key), ("study:" + key).c_str(), generation);
}

bool MiotClient::ShowStudyJpeg(const std::string& url, const char* label, uint32_t generation) {
    if (!study_audio_generation_.Matches(generation)) return false;
    const int64_t deadline_ms = esp_timer_get_time() / 1000 + 15000;
    auto& board = Board::GetInstance();
    auto* network = board.GetNetwork();
    if (network == nullptr) {
        ESP_LOGW(TAG, "No network interface for study image");
        return false;
    }
    auto http = network->CreateHttp(3);
    if (http == nullptr) {
        ESP_LOGE(TAG, "Failed to create http client for study image");
        return false;
    }
    http->SetTimeout(15000);
    http->SetHeader("User-Agent", "ESP32-MIOT/1.0");
    http->SetHeader("X-Device-Mac", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Accept-Encoding", "identity");
    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "Study image HTTP open failed: %s", url.c_str());
        http->Close();
        return false;
    }
    int status = http->GetStatusCode();
    if (status != 200) {
        ESP_LOGE(TAG, "Study image HTTP status %d", status);
        http->Close();
        return false;
    }
    size_t content_length = http->GetBodyLength();
    if (content_length == 0 || content_length > 512 * 1024) {
        ESP_LOGW(TAG, "Study image invalid body length %u", (unsigned)content_length);
        http->Close();
        return false;
    }
    char* data = (char*)heap_caps_malloc(content_length, MALLOC_CAP_8BIT);
    if (data == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate %u bytes for study image",
                 (unsigned)content_length);
        http->Close();
        return false;
    }
    size_t total_read = 0;
    while (total_read < content_length) {
        if (!study_audio_generation_.Matches(generation) || esp_timer_get_time() / 1000 >= deadline_ms) {
            heap_caps_free(data);
            http->Close();
            return false;
        }
        int remaining_ms = ivy::RemainingReadBudgetMs(deadline_ms, esp_timer_get_time() / 1000);
        if (!remaining_ms) break;
        http->SetTimeout(remaining_ms);
        int ret = http->Read(data + total_read, content_length - total_read);
        if (ret < 0) {
            ESP_LOGE(TAG, "Study image HTTP read failed");
            heap_caps_free(data);
            http->Close();
            return false;
        }
        if (ret == 0) {
            break;
        }
        total_read += ret;
    }
    http->Close();
    if (total_read < content_length) {
        ESP_LOGW(TAG, "Study image truncated: %u/%u", (unsigned)total_read,
                 (unsigned)content_length);
        heap_caps_free(data);
        return false;
    }

    uint8_t* out_data = nullptr;
    size_t out_len = 0, out_width = 0, out_height = 0, out_stride = 0;
    esp_err_t ret = jpeg_to_image((const uint8_t*)data, content_length, &out_data, &out_len,
                                  &out_width, &out_height, &out_stride);
    heap_caps_free(data);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Study image JPEG decode failed: %d (%s)", (int)ret, esp_err_to_name(ret));
        if (out_data != nullptr) {
            heap_caps_free(out_data);
        }
        return false;
    }

    auto* display = dynamic_cast<LvglDisplay*>(board.GetDisplay());
    if (display == nullptr) {
        ESP_LOGW(TAG, "No Lvgl display for study image");
        heap_caps_free(out_data);
        return false;
    }
    auto image = std::make_unique<LvglAllocatedImage>(out_data, out_len, out_width, out_height,
                                                       out_stride, LV_COLOR_FORMAT_RGB565);
    std::lock_guard<std::mutex> lock(study_asset_mutex_);
    if (!study_audio_generation_.Matches(generation)) return false;
    display->SetPreviewImage(std::move(image));
    ESP_LOGI(TAG, "Study image shown: %s (%ux%u %uB)", label, (unsigned)out_width,
             (unsigned)out_height, (unsigned)out_len);
    return true;
}

bool MiotClient::DownloadStudyGifHttp(const std::string& host, int port, const std::string& path,
                                      std::vector<uint8_t>& out, uint32_t generation, int64_t deadline_ms) {
    auto current = [this, generation, deadline_ms]() {
        return study_audio_generation_.Matches(generation) && esp_timer_get_time() / 1000 < deadline_ms;
    };
    if (!current()) return false;
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        ESP_LOGE(TAG, "Study gif bad host: %s", host.c_str());
        return false;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        ESP_LOGE(TAG, "Study gif socket failed: %d", errno);
        return false;
    }
    // Bound both connect and individual I/O; the overall GIF budget is shared across retries.
    struct timeval tv{};
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        return false;
    }
    int connected = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    if (connected < 0 && errno == EINPROGRESS) {
        while (current()) {
            fd_set writable;
            FD_ZERO(&writable);
            FD_SET(fd, &writable);
            timeval wait{0, 200000};
            int ready = select(fd + 1, nullptr, &writable, nullptr, &wait);
            if (ready == 0) continue;
            int error = 0;
            socklen_t length = sizeof(error);
            connected = ready > 0 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 &&
                        error == 0 ? 0 : -1;
            break;
        }
    }
    if (connected < 0 || !current() || fcntl(fd, F_SETFL, flags) < 0) {
        close(fd);
        return false;
    }

    std::string request = "GET " + path + " HTTP/1.1\r\n"
                          "Host: " + host + ":" + std::to_string(port) + "\r\n"
                          "User-Agent: ESP32-MIOT/1.0\r\n"
                          "Accept-Encoding: identity\r\n"
                          "Connection: close\r\n\r\n";
    if (send(fd, request.data(), request.size(), 0) != (ssize_t)request.size()) {
        ESP_LOGE(TAG, "Study gif send failed: %d", errno);
        close(fd);
        return false;
    }

    // 整段读入内存：socket 内核缓冲直接交付大块数据，避免逐 1500B 分块解析
    std::vector<uint8_t> raw;
    raw.reserve(512 * 1024);
    std::vector<uint8_t> chunk(16384);
    int64_t t0 = esp_timer_get_time() / 1000;
    int64_t last_report_ms = t0;
    while (true) {
        if (!current()) {
            close(fd);
            return false;
        }
        ssize_t n = recv(fd, chunk.data(), chunk.size(), 0);
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                ESP_LOGW(TAG, "Study gif recv timeout after %u bytes", (unsigned)raw.size());
            } else {
                ESP_LOGE(TAG, "Study gif recv failed: %d", errno);
            }
            close(fd);
            return false;
        }
        if (raw.size() + n > 4 * 1024 * 1024 + 8192) {
            close(fd);
            return false;
        }
        raw.insert(raw.end(), chunk.data(), chunk.data() + n);
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - last_report_ms >= 1000) {
            ESP_LOGI(TAG, "Study gif download progress: %u bytes in %lld ms", (unsigned)raw.size(),
                     now_ms - t0);
            last_report_ms = now_ms;
        }
    }
    close(fd);

    if (raw.size() < 4) {
        ESP_LOGW(TAG, "Study gif empty response (%u bytes)", (unsigned)raw.size());
        return false;
    }
    // 定位响应头结束 \r\n\r\n
    size_t hdr_end = std::string::npos;
    for (size_t i = 0; i + 3 < raw.size(); ++i) {
        if (raw[i] == '\r' && raw[i + 1] == '\n' && raw[i + 2] == '\r' && raw[i + 3] == '\n') {
            hdr_end = i;
            break;
        }
    }
    if (hdr_end == std::string::npos) {
        ESP_LOGW(TAG, "Study gif invalid response (no header terminator)");
        return false;
    }
    std::string header(raw.begin(), raw.begin() + hdr_end);
    ESP_LOGI(TAG, "Study gif response header: %.120s", header.c_str());
    bool ok_status = header.find(" 200 ") != std::string::npos || header.find("200 ") != std::string::npos;
    if (!ok_status) {
        ESP_LOGW(TAG, "Study gif bad HTTP status");
        return false;
    }
    std::string lheader = header;
    std::transform(lheader.begin(), lheader.end(), lheader.begin(), ::tolower);
    size_t cl_pos = lheader.find("content-length:");
    if (cl_pos != std::string::npos) {
        size_t val = cl_pos + 15;
        while (val < lheader.size() && (lheader[val] == ' ' || lheader[val] == '\r' || lheader[val] == '\n')) {
            ++val;
        }
        size_t end = val;
        while (end < lheader.size() && isdigit((unsigned char)lheader[end])) {
            ++end;
        }
        size_t content_length = 0;
        if (end > val) {
            content_length = std::strtoul(lheader.substr(val, end - val).c_str(), nullptr, 10);
        }
        size_t body_start = hdr_end + 4;
        size_t body_bytes = raw.size() - body_start;
        if (content_length > 0 && body_bytes < content_length) {
            ESP_LOGW(TAG, "Study gif truncated: %u/%u", (unsigned)body_bytes,
                     (unsigned)content_length);
            return false;
        }
        out.assign(raw.begin() + body_start, raw.end());
    } else {
        out.assign(raw.begin() + hdr_end + 4, raw.end());
    }
    return true;
}

bool MiotClient::ShowStudyGif(const std::string& url, uint32_t generation, int64_t deadline_ms) {
    if (url.empty()) {
        return false;
    }
    auto& board = Board::GetInstance();
    // 拆分 http://host:port/path -> host/port/path
    const char* base = "http://";
    size_t host_start = strlen(base);
    std::string rest = url.substr(host_start);
    size_t port_end = rest.find('/');
    if (port_end == std::string::npos) {
        ESP_LOGW(TAG, "Study gif invalid url: %s", url.c_str());
        return false;
    }
    std::string hostport = rest.substr(0, port_end);
    std::string http_path = rest.substr(port_end);
    size_t colon = hostport.find(':');
    std::string host = hostport;
    int port = 80;
    if (colon != std::string::npos) {
        host = hostport.substr(0, colon);
        std::string port_str = hostport.substr(colon + 1);
        port = std::atoi(port_str.c_str());
        if (port <= 0 || port > 65535) {
            ESP_LOGW(TAG, "Study gif invalid port: %s", port_str.c_str());
            return false;
        }
    }
    std::vector<uint8_t> data;
    if (!DownloadStudyGifHttp(host, port, http_path, data, generation, deadline_ms)) {
        ESP_LOGW(TAG, "Study gif download failed: %s", url.c_str());
        return false;
    }
    size_t content_length = data.size();
    // 口型/卡通 GIF 可能超过默认预览图上限：放宽到 4MB，且要求至少 512KB 以避开噪声小图。
    if (content_length < 256 * 1024 || content_length > 4 * 1024 * 1024) {
        ESP_LOGW(TAG, "Study gif invalid body length %u", (unsigned)content_length);
        return false;
    }
    // GIF 魔数校验
    if (data.size() < 6 || data[0] != 'G' || data[1] != 'I' || data[2] != 'F') {
        ESP_LOGW(TAG, "Study gif magic mismatch");
        return false;
    }

    // 交给显示层：SetPreviewGif 接管字节所有权并整段循环播放。
    auto* display = dynamic_cast<LcdDisplay*>(board.GetDisplay());
    if (display == nullptr) {
        ESP_LOGW(TAG, "No LCD display for study gif");
        return false;
    }
    std::lock_guard<std::mutex> lock(study_asset_mutex_);
    if (!study_audio_generation_.Matches(generation)) return false;
    bool ok = display->SetPreviewGif(std::move(data));
    ESP_LOGI(TAG, "Study gif %s: %s", ok ? "shown" : "failed", url.c_str());
    return ok;
}

void MiotClient::ClearStudyPreview() {
    StopStudyLrc();
    ShowStudySubtitle("");
    auto* display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
    if (display != nullptr) {
        display->SetPreviewImage(nullptr);
    }
}

void MiotClient::HandlePlaybackStopped() {
    // Study continuation is driven only by the typed, generation-checked audio event.
    Application::GetInstance().Schedule([this]() {
        if (!study_active_) RequestReconnect();
    });
}

void MiotClient::StartSecondStudyPass() {
    // Natural first-round EOF, dispatched onto the main task.
    study_pass_.store(2);
    InvalidateStudyAudio();
    StopStudyLrc();
    ReportStudyStatus("loading");
    std::string key = last_course_key_;
    ESP_LOGI(TAG, "FaceFonics 第1遍播完，切换卡通 GIF 重播: %s", key.c_str());
    ClearStudyPreview();
    DispatchStudyWork(key, "", false, 2);
}

bool MiotClient::ParseLrcLine(const std::string& line, int& seconds, std::string& text) {
    // 支持 "[mm:ss.cs]" 或 "[mm:ss.ccc]"；文本在时间标签后。
    size_t pos = line.find(']');
    if (line.rfind('[', 0) != 0 || pos == std::string::npos) {
        text.clear();
        return false;
    }
    const std::string tag = line.substr(1, pos - 1);
    int sec_piece = -1;
    int m = 0, s = 0, cs = 0;
    if (sscanf(tag.c_str(), "%d:%d.%d", &m, &s, &cs) == 3) {
        sec_piece = m * 60 + s;
    } else if (sscanf(tag.c_str(), "%d:%d", &m, &s) == 2) {
        sec_piece = m * 60 + s;
    }
    if (sec_piece < 0) {
        text.clear();
        return false;
    }
    seconds = sec_piece;
    text = line.substr(pos + 1);
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.erase(text.begin());
    }
    return true;
}

void MiotClient::ShowStudySubtitle(const std::string& text, uint32_t generation) {
    auto& app = Application::GetInstance();
    std::string msg = text;
    app.Schedule([this, msg, generation]() {
        if (generation && study_lrc_generation_.load() != generation) return;
        auto* display = Board::GetInstance().GetDisplay();
        if (display != nullptr) {
            display->SetChatMessage("system", msg.empty() ? nullptr : msg.c_str());
        }
    });
}

void MiotClient::StartStudyLrc(const std::string& key) {
    if (key.empty()) {
        return;
    }
    uint32_t generation = ++study_lrc_generation_;
    if (!study_lrc_worker_) {
        study_lrc_queue_ = xQueueCreate(1, sizeof(StudyLrcTaskArgs*));
        if (!study_lrc_queue_ || xTaskCreate(StudyLrcTaskEntry, "study_lrc", 4096, this,
                                            4, &study_lrc_worker_) != pdPASS) {
            if (study_lrc_queue_) vQueueDelete(study_lrc_queue_);
            study_lrc_queue_ = nullptr;
            study_lrc_worker_ = nullptr;
            ESP_LOGE(TAG, "Study lrc worker create failed");
            return;
        }
    }
    auto* args = new StudyLrcTaskArgs{this, key, generation};
    StudyLrcTaskArgs* old = nullptr;
    if (xQueueReceive(study_lrc_queue_, &old, 0) == pdTRUE) delete old;
    if (xQueueSend(study_lrc_queue_, &args, 0) != pdTRUE) delete args;
}

void MiotClient::StopStudyLrc() {
    study_lrc_generation_.fetch_add(1);
}

void MiotClient::StudyLrcTaskEntry(void* arg) {
    auto* client = static_cast<MiotClient*>(arg);
    StudyLrcTaskArgs* pending;
    for (;;) {
        if (xQueueReceive(client->study_lrc_queue_, &pending, portMAX_DELAY) != pdTRUE) continue;
        std::unique_ptr<StudyLrcTaskArgs> args(pending);
        client->RunStudyLrc(args->key, args->generation);
    }
}

// LRC 解析：行集合（时间行 + 文本行）。文本紧跟时间行 = 该时刻起显示。
void MiotClient::RunStudyLrc(const std::string& key, uint32_t generation) {
    if (study_lrc_generation_.load() != generation) {
        return;
    }
    const int64_t deadline_ms = esp_timer_get_time() / 1000 + 15000;
    auto& board = Board::GetInstance();
    auto* network = board.GetNetwork();
    if (network == nullptr) {
        return;
    }
    std::string url = std::string(kStudyLrcUrlBase) + UrlEncode(key);
    auto http = network->CreateHttp(3);
    if (http == nullptr) {
        return;
    }
    http->SetTimeout(15000);
    http->SetHeader("User-Agent", "ESP32-MIOT/1.0");
    if (!http->Open("GET", url)) {
        ESP_LOGW(TAG, "Study lrc open failed: %s", url.c_str());
        http->Close();
        return;
    }
    int status = http->GetStatusCode();
    if (status != 200) {
        ESP_LOGW(TAG, "Study lrc http status %d", status);
        http->Close();
        return;
    }
    std::string body;
    const size_t expected = http->GetBodyLength();
    bool complete = false;
    char buffer[1024];
    while (study_lrc_generation_.load() == generation && esp_timer_get_time() / 1000 < deadline_ms) {
        int remaining_ms = ivy::RemainingReadBudgetMs(deadline_ms, esp_timer_get_time() / 1000);
        if (!remaining_ms) break;
        http->SetTimeout(remaining_ms);
        int count = http->Read(buffer, sizeof(buffer));
        if (count == 0) {
            complete = !expected || body.size() == expected;
            break;
        }
        if (count < 0 || body.size() + count > 64 * 1024) break;
        body.append(buffer, count);
        if (expected && body.size() == expected) {
            complete = true;
            break;
        }
    }
    http->Close();
    ESP_LOGI(TAG, "Study lrc %s: %d bytes", key.c_str(), (int)body.size());
    if (!complete || body.empty() || study_lrc_generation_.load() != generation) {
        return;
    }

    // 解析为 (time_sec, text) 有序列表
    std::vector<std::pair<int, std::string>> cues;
    std::string last_text;
    std::string line;
    for (size_t i = 0; i <= body.size(); ++i) {
        char c = (i < body.size()) ? body[i] : '\n';
        if (c != '\n' && i < body.size()) {
            line.push_back(c);
            continue;
        }
        // 处理 \r
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        int t = 0;
        std::string txt;
        if (ParseLrcLine(line, t, txt)) {
            if (!txt.empty()) {
                cues.emplace_back(t, txt);
            }
        }
        line.clear();
    }
    if (cues.empty()) {
        ESP_LOGW(TAG, "Study lrc empty cues: %s", key.c_str());
        return;
    }
    ESP_LOGI(TAG, "Study lrc %s: %u cues", key.c_str(), (unsigned)cues.size());

    // 50ms tick 按时间轴推进，直到音频结束（取最后 cue 时间 + 5s 上限）。
    const int64_t t0 = esp_timer_get_time() / 1000;
    const int kEndMs = (cues.back().first + 5) * 1000;
    size_t idx = 0;
    std::string showed;
    while (study_lrc_generation_.load() == generation) {
        int64_t elapsed = esp_timer_get_time() / 1000 - t0;
        if (elapsed >= kEndMs) {
            break;
        }
        // 推进到应显示的最新 cue
        int64_t now_ms = elapsed;
        size_t next = idx;
        while (next < cues.size() && (int64_t)cues[next].first * 1000 <= now_ms) {
            next++;
        }
        if (next > 0 && (next != idx)) {
            const std::string& tgt = cues[next - 1].second;
            if (tgt != showed) {
                showed = tgt;
                ESP_LOGI(TAG, "Study lrc cue: %dms -> %s", (int)now_ms, tgt.c_str());
                ShowStudySubtitle(tgt, generation);
            }
        }
        idx = next;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    // Stale tasks must not clear the next round's subtitles.
    ShowStudySubtitle("", generation);
}

MiotClient::NextCourseResult MiotClient::FetchNextCourse(const std::string& after,
                                                       std::string& word, std::string& key,
                                                       uint32_t generation) {
    if (after.empty()) {
        return NextCourseResult::Failed;
    }
    std::string url = kStudyCourseNextApiBase + UrlEncode(after);
    std::string body = FetchStudyBody(url, generation);

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "Course fetch invalid JSON");
        return NextCourseResult::Failed;
    }
    if (cJSON_IsTrue(cJSON_GetObjectItem(root, "done"))) {
        ESP_LOGI(TAG, "All courses finished");
        cJSON_Delete(root);
        return NextCourseResult::Done;
    }
    cJSON* w = cJSON_GetObjectItem(root, "word");
    cJSON* k = cJSON_GetObjectItem(root, "key");
    if (k != nullptr && cJSON_IsString(k) && k->valuestring != nullptr) {
        key = k->valuestring;
    }
    if (w != nullptr && cJSON_IsString(w) && w->valuestring != nullptr) {
        word = w->valuestring;
    }
    cJSON_Delete(root);
    return key.empty() ? NextCourseResult::Failed : NextCourseResult::Next;
}

void MiotClient::ContinueCourse(const std::string& after) {
    if (!study_active_ || !course_mode_.load()) return;
    if (music_player_ == nullptr) {
        FinishStudy("failed", "audio_failed", "Study audio playback failed.");
        return;
    }
    DispatchStudyWork(after, "", true, 1);
}

bool MiotClient::IsFaceFonicsCourse(const std::string& key) {
    // FaceFonics 课程键为 kc-001 ~ kc-109（有口型+卡通 GIF 素材）。其余 kc-1xx
    // （单词歌/第三只耳朵/泡脑子）无卡通素材，音频仅播一遍。
    if (key.size() != 6 || key.compare(0, 3, "kc-") != 0) {
        return false;
    }
    for (size_t i = 3; i < key.size(); ++i) {
        if (key[i] < '0' || key[i] > '9') {
            return false;
        }
    }
    int num = 0;
    for (size_t i = 3; i < key.size(); ++i) {
        num = num * 10 + (key[i] - '0');
    }
    return num >= 1 && num <= 109;
}

// === MCP 媒体工具入口：直接调用媒体引擎方法，不构造命令 JSON ===
void MiotClient::McpPlayMusic(const std::string& keyword) {
    EnginePlayMusic(keyword);
}

void MiotClient::McpControlPlayback(const std::string& action) {
    EngineControlMusic(action);
}

void MiotClient::McpPlayAlbum(const std::string& album) {
    EnginePlayAlbum(album);
}

void MiotClient::McpPlayRadio(const std::string& station, const std::string& name) {
    EnginePlayRadio(station, name);
}

void MiotClient::McpPlayWeather(const std::string& city, const std::string& city_enc) {
    EnginePlayWeather(city, city_enc);
}

void MiotClient::McpPlayStudy(const std::string& key, const std::string& word, bool course) {
    EnginePlayStudy(key, word, course);
}

void MiotClient::McpPlayUrl(const std::string& url) {
    EnginePlayUrl(url);
}

void MiotClient::McpPlayContinuous(const std::string& keyword) {
    EnginePlayContinuous(keyword);
}

void MiotClient::EnginePlayUrl(const std::string& url) {
    FinishStudy("stopped");
    if (music_player_ == nullptr) return;
    ESP_LOGI(TAG, "播放URL: [%s]", url.c_str());
    music_player_->PlayUrl(url);
}

void MiotClient::EnginePlayContinuous(const std::string& keyword) {
    FinishStudy("stopped");
    if (music_player_ == nullptr) return;
    ESP_LOGI(TAG, "连续播放: [%s]", keyword.c_str());
    music_player_->PlayContinuous(keyword);
}
