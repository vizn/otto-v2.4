#include "miot_client.h"

#include <esp_log.h>
#include <cstring>

#include "board.h"
#include "application.h"
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
                auto& app = Application::GetInstance();
                const std::string& user_text = app.GetLastUserText();
                if (user_text.find("跳舞") != std::string::npos ||
                    user_text.find("舞蹈") != std::string::npos ||
                    user_text.find("边跳边放") != std::string::npos ||
                    user_text.find("跳着") != std::string::npos) {
                    ESP_LOGI(TAG, "用户原话「%s」含跳舞意图，触发随机舞蹈", user_text.c_str());
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
                music_player_->StopPlay();
            } else if (strcmp(a, "next") == 0) {
                // TODO: 播放列表顺序播放
            } else if (strcmp(a, "previous") == 0) {
                // TODO: 播放列表顺序播放
            }
        }
    } else if (strcmp(type, "chat") == 0) {
        // 文本消息，音乐播放器显示歌词/文本（可选）
        cJSON* text = cJSON_GetObjectItem(data, "text");
        if (text != nullptr && cJSON_IsString(text)) {
            music_player_->ShowMessage(text->valuestring);
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
