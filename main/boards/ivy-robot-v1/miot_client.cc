#include "miot_client.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <vector>

#include "board.h"
#include "application.h"
#include "display.h"
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

MiotClient::MiotClient() = default;

MiotClient::~MiotClient() { Stop(); }

void MiotClient::Start() {
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

    // 心跳循环：发送 JSON ping -> 服务器返回 JSON pong
    TickType_t last_heartbeat = xTaskGetTickCount();
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

    if (music_player_ == nullptr) {
        return;
    }

    if (strcmp(type, "music") == 0) {
        // {"type":"music","text":"关键词"}
        cJSON* text = cJSON_GetObjectItem(data, "text");
        if (text != nullptr && cJSON_IsString(text)) {
            const char* keyword = text->valuestring;
            ESP_LOGI(TAG, "收到音乐指令关键词: [%s]", keyword);

            // 边跳舞边放<歌曲名>：随机/指定音乐 + 随机舞蹈
            std::string play_keyword;
            if (ParseDanceWithMusic(keyword, play_keyword)) {
                music_player_->PlayByKeyword(play_keyword);
                ESP_LOGI(TAG, "「%s」触发随机音乐舞蹈，播放歌曲: %s", keyword, play_keyword.c_str());
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
    } else if (strcmp(type, "music_control") == 0) {
        // {"type":"music_control","action":"play|pause|next|previous|stop"}
        cJSON* action = cJSON_GetObjectItem(data, "action");
        if (action != nullptr && cJSON_IsString(action)) {
            const char* a = action->valuestring;
            if (strcmp(a, "play") == 0) {
                music_player_->Resume();
            } else if (strcmp(a, "pause") == 0) {
                music_player_->Pause();
            } else if (strcmp(a, "stop") == 0) {
                // 显式停止会打断课程连播：禁止播完后自动续播下一课
                course_mode_.store(false);
                course_stopped_.store(true);
                music_player_->StopPlay();
            } else if (strcmp(a, "next") == 0) {
                course_mode_.store(false);
                course_stopped_.store(true);
                music_player_->Next();
            } else if (strcmp(a, "previous") == 0) {
                course_mode_.store(false);
                course_stopped_.store(true);
                music_player_->Previous();
            }
        }
    } else if (strcmp(type, "album") == 0) {
        // {"type":"album","album":"专辑名"}：从服务器拉取歌单并连播
        cJSON* album = cJSON_GetObjectItem(data, "album");
        if (album != nullptr && cJSON_IsString(album)) {
            const char* name = album->valuestring;
            ESP_LOGI(TAG, "收到专辑播放指令: [%s]", name);
            std::vector<std::string> playlist = FetchAlbumPlaylist(name);
            if (playlist.empty()) {
                ESP_LOGW(TAG, "专辑「%s」歌单为空，无法播放", name);
            } else {
                ESP_LOGI(TAG, "专辑「%s」共 %d 首，开始连播", name, (int)playlist.size());
                music_player_->PlayPlaylist(std::move(playlist));
            }
        }
    } else if (strcmp(type, "chat") == 0) {
        // 文本消息，音乐播放器显示歌词/文本（可选）
        cJSON* text = cJSON_GetObjectItem(data, "text");
        if (text != nullptr && cJSON_IsString(text)) {
            music_player_->ShowMessage(text->valuestring);
        }
    } else if (strcmp(type, "radio") == 0) {
        // {"type":"radio","station":"qtfm-dszs","name":"中国之声"}
        cJSON* station = cJSON_GetObjectItem(data, "station");
        cJSON* name = cJSON_GetObjectItem(data, "name");
        if (station != nullptr && cJSON_IsString(station)) {
            ESP_LOGI(TAG, "收到电台播放指令: [%s]", station->valuestring);
            music_player_->PlayRadio(station->valuestring);
            if (name != nullptr && cJSON_IsString(name) && strlen(name->valuestring) > 0) {
                std::string shown = "电台：" + std::string(name->valuestring);
                music_player_->ShowMessage(shown);
            }
        }
    } else if (strcmp(type, "weather") == 0) {
        // {"type":"weather","city":"北京","city_enc":"..."}
        cJSON* city = cJSON_GetObjectItem(data, "city");
        cJSON* enc = cJSON_GetObjectItem(data, "city_enc");
        std::string city_name = (city != nullptr && cJSON_IsString(city)) ? city->valuestring : "";
        std::string city_enc_str =
            (enc != nullptr && cJSON_IsString(enc)) ? enc->valuestring : UrlEncode(city_name);
        ESP_LOGI(TAG, "收到天气播报指令: [%s]", city_name.c_str());
        // 先拉取天气文本上屏（复用音乐播放器的消息显示链路）
        std::string weather_text = FetchWeatherText(city_name);
        if (!weather_text.empty()) {
            music_player_->ShowMessage(weather_text);
        }
        // 再播放服务器 EdgeTTS 合成的天气播报音频
        music_player_->PlayWeather(city_enc_str);
    } else if (strcmp(type, "study_card") == 0 || strcmp(type, "study_course") == 0) {
        // {"type":"study_card","word":"cat","key":"aflq68xb"} 单卡直学
        // {"type":"study_course","word":"...","key":"kc-001"} 课程连播（播完自动续下一课）
        bool course = strcmp(type, "study_course") == 0;
        cJSON* word = cJSON_GetObjectItem(data, "word");
        cJSON* key = cJSON_GetObjectItem(data, "key");
        std::string word_str = (word != nullptr && cJSON_IsString(word)) ? word->valuestring : "";
        std::string key_str = (key != nullptr && cJSON_IsString(key)) ? key->valuestring : "";
        ESP_LOGI(TAG, "收到学习%s指令: word=[%s] key=[%s]", course ? "课程" : "卡片",
                 word_str.c_str(), key_str.c_str());
        if (!key_str.empty()) {
            // 学习卡片播放期间禁用 LLM：置位标志（丢弃出站/入站音频）并中止当前会话。
            // 唤醒词暂停仅关闭唤醒检测，不停已在进行的监听会话，故需显式停听。
            auto& app = Application::GetInstance();
            app.SetStudyCardPlaying(true);
            app.Schedule([&app]() {
                auto st = app.GetDeviceState();
                if (st == kDeviceStateSpeaking || st == kDeviceStateListening) {
                    app.AbortSpeaking(kAbortReasonNone);
                }
                if (st == kDeviceStateListening) {
                    app.StopListening();
                }
                while (app.GetAudioService().PopPacketFromSendQueue()) {
                }
            });
            // 课程连播状态（单卡学习不进入连播）
            course_mode_.store(course);
            course_stopped_.store(!course);
            last_course_key_ = key_str;
            ShowStudyImage(key_str);
            if (!word_str.empty()) {
                music_player_->ShowMessage("学习卡片：" + word_str);
            }
            music_player_->PlayStudyCard(key_str);
        }
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

bool MiotClient::ShowStudyImage(const std::string& key) {
    if (key.empty()) {
        return false;
    }
    auto& board = Board::GetInstance();
    auto* network = board.GetNetwork();
    if (network == nullptr) {
        ESP_LOGW(TAG, "No network interface for study image");
        return false;
    }
    std::string url = kStudyImageUrlBase + UrlEncode(key);
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
    display->SetPreviewImage(std::move(image));
    ESP_LOGI(TAG, "Study card image shown: %s (%ux%u %uB)", key.c_str(), (unsigned)out_width,
             (unsigned)out_height, (unsigned)out_len);
    return true;
}

void MiotClient::HandlePlaybackStopped() {
    // 在音乐播放器任务内调用：课程连播时把续播调度到主任务（屏幕/音频安全），
    // 其余情况与原逻辑一致——解除学习卡片静音并重连（模拟自动下线，恢复唤醒）。
    auto& app = Application::GetInstance();
    if (!course_mode_.load() || course_stopped_.load()) {
        app.SetStudyCardPlaying(false);
        RequestReconnect();
        return;
    }
    std::string after = last_course_key_;
    app.Schedule([this, after]() { ContinueCourse(after); });
}

bool MiotClient::FetchNextCourse(const std::string& after, std::string& word, std::string& key) {
    if (after.empty()) {
        return false;
    }
    auto& board = Board::GetInstance();
    auto* network = board.GetNetwork();
    if (network == nullptr) {
        ESP_LOGW(TAG, "No network interface for course fetch");
        return false;
    }
    std::string url = kStudyCourseNextApiBase + UrlEncode(after);
    auto http = network->CreateHttp(3);
    if (http == nullptr) {
        ESP_LOGE(TAG, "Failed to create http client for course fetch");
        return false;
    }
    http->SetTimeout(15000);
    http->SetHeader("User-Agent", "ESP32-MIOT/1.0");
    http->SetHeader("X-Device-Mac", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Accept-Encoding", "identity");
    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "Course fetch HTTP open failed: %s", url.c_str());
        http->Close();
        return false;
    }
    int status = http->GetStatusCode();
    if (status != 200) {
        ESP_LOGE(TAG, "Course fetch HTTP status %d", status);
        http->Close();
        return false;
    }
    std::string body = http->ReadAll();
    http->Close();
    ESP_LOGI(TAG, "Course fetch response %d bytes", (int)body.size());

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "Course fetch invalid JSON");
        return false;
    }
    if (cJSON_IsTrue(cJSON_GetObjectItem(root, "done"))) {
        ESP_LOGI(TAG, "All courses finished");
        cJSON_Delete(root);
        return false;
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
    return !key.empty();
}

void MiotClient::ContinueCourse(const std::string& after) {
    if (music_player_ == nullptr) {
        course_mode_.store(false);
        Application::GetInstance().SetStudyCardPlaying(false);
        RequestReconnect();
        return;
    }
    std::string word, key;
    if (!FetchNextCourse(after, word, key) || key.empty()) {
        ESP_LOGI(TAG, "Course playback finished");
        course_mode_.store(false);
        course_stopped_.store(true);
        Application::GetInstance().SetStudyCardPlaying(false);
        RequestReconnect();
        return;
    }
    last_course_key_ = key;
    Application::GetInstance().SetStudyCardPlaying(true);
    ShowStudyImage(key);
    if (!word.empty()) {
        music_player_->ShowMessage("学习卡片：" + word);
    }
    music_player_->PlayStudyCard(key);
}

// === MCP 媒体工具入口：复用既有 MIOT 处理逻辑，不复制播放代码 ===
void MiotClient::McpPlayMusic(const std::string& keyword) {
    cJSON* cmd = cJSON_CreateObject();
    cJSON_AddStringToObject(cmd, "type", "music");
    cJSON_AddStringToObject(cmd, "text", keyword.c_str());
    HandleControlCommand(cmd);
    cJSON_Delete(cmd);
}

void MiotClient::McpControlPlayback(const std::string& action) {
    cJSON* cmd = cJSON_CreateObject();
    cJSON_AddStringToObject(cmd, "type", "music_control");
    cJSON_AddStringToObject(cmd, "action", action.c_str());
    HandleControlCommand(cmd);
    cJSON_Delete(cmd);
}

void MiotClient::McpPlayAlbum(const std::string& album) {
    cJSON* cmd = cJSON_CreateObject();
    cJSON_AddStringToObject(cmd, "type", "album");
    cJSON_AddStringToObject(cmd, "album", album.c_str());
    HandleControlCommand(cmd);
    cJSON_Delete(cmd);
}

void MiotClient::McpPlayRadio(const std::string& station, const std::string& name) {
    cJSON* cmd = cJSON_CreateObject();
    cJSON_AddStringToObject(cmd, "type", "radio");
    cJSON_AddStringToObject(cmd, "station", station.c_str());
    if (!name.empty()) {
        cJSON_AddStringToObject(cmd, "name", name.c_str());
    }
    HandleControlCommand(cmd);
    cJSON_Delete(cmd);
}

void MiotClient::McpPlayWeather(const std::string& city, const std::string& city_enc) {
    cJSON* cmd = cJSON_CreateObject();
    cJSON_AddStringToObject(cmd, "type", "weather");
    cJSON_AddStringToObject(cmd, "city", city.c_str());
    if (!city_enc.empty()) {
        cJSON_AddStringToObject(cmd, "city_enc", city_enc.c_str());
    }
    HandleControlCommand(cmd);
    cJSON_Delete(cmd);
}

void MiotClient::McpPlayStudy(const std::string& key, const std::string& word, bool course) {
    cJSON* cmd = cJSON_CreateObject();
    cJSON_AddStringToObject(cmd, "type", course ? "study_course" : "study_card");
    if (!word.empty()) {
        cJSON_AddStringToObject(cmd, "word", word.c_str());
    }
    cJSON_AddStringToObject(cmd, "key", key.c_str());
    HandleControlCommand(cmd);
    cJSON_Delete(cmd);
}
