#include <driver/spi_common.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>

#include "application.h"
#include "button.h"
#include "codecs/no_audio_codec.h"
#include "config.h"
#include "display/lcd_display.h"
#include "mcp_server.h"
#include "miot_client.h"
#include "music_player.h"
#include "otto_controller.h"
#include "otto_emoji_display.h"
#include "power_manager.h"
#include "system_reset.h"
#include "websocket_control_server.h"
#include "wifi_board.h"

#define TAG "OttoRobot"

extern void InitializeOttoController(const HardwareConfig& hw_config);

class OttoRobot : public WifiBoard {
private:
    LcdDisplay* display_;
    PowerManager* power_manager_;
    Button boot_button_;
    WebSocketControlServer* ws_control_server_;
    HardwareConfig hw_config_;
    AudioCodec* audio_codec_;
    MiotClient* miot_client_;
    MusicPlayer* music_player_;

    void InitializePowerManager() {
        power_manager_ = new PowerManager(hw_config_.power_charge_detect_pin,
                                          hw_config_.power_adc_unit, hw_config_.power_adc_channel);
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = hw_config_.display_mosi_pin;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = hw_config_.display_clk_pin;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeLcdDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = hw_config_.display_cs_pin;
        io_config.dc_gpio_num = hw_config_.display_dc_pin;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = hw_config_.display_rst_pin;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;

        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

        display_ = new OttoEmojiDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                        DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                                        DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

    void InitializeOttoController() { ::InitializeOttoController(hw_config_); }

public:
    const HardwareConfig& GetHardwareConfig() const { return hw_config_; }

private:
    void InitializeWebSocketControlServer() {
        ws_control_server_ = new WebSocketControlServer();
        if (!ws_control_server_->Start(8080)) {
            delete ws_control_server_;
            ws_control_server_ = nullptr;
            return;
        }
        // 将 MCP 响应同时广播回连接到 8080 端口的 WebSocket 客户端
        Application::GetInstance().RegisterMcpBroadcastCallback([this](const std::string& payload) {
            if (ws_control_server_) {
                ws_control_server_->BroadcastMessage(payload);
            }
        });
    }

    void StartNetwork() override {
        WifiBoard::StartNetwork();
        vTaskDelay(pdMS_TO_TICKS(1000));

        InitializeWebSocketControlServer();
    }

    void SetNetworkEventCallback(NetworkEventCallback callback) override {
        WifiBoard::SetNetworkEventCallback([this, callback](NetworkEvent event, const std::string& data) {
            callback(event, data);
            if (event == NetworkEvent::Connected && miot_client_ == nullptr) {
                StartMiotClient();
            }
        });
    }

    void StartMiotClient() {
        music_player_ = new MusicPlayer();
        music_player_->SetAudioCodec(GetAudioCodec());
        music_player_->Start();

        miot_client_ = new MiotClient();
        miot_client_->SetMusicPlayer(music_player_);
        // 音乐播放结束（自然播完/被停止）：结束跟随音乐的连续舞蹈；
        // 若处于课程连播则自动续播下一课，否则解除学习卡片静音并重连恢复唤醒
        music_player_->SetOnStoppedCallback([this]() {
            ESP_LOGI(TAG, "Music stopped, stopping continuous dance");
            OttoControllerStopAll();
            if (miot_client_ != nullptr) {
                miot_client_->HandlePlaybackStopped();
            }
        });
        miot_client_->Start();
        ESP_LOGI(TAG, "MIOT client started");
    }

    void RegisterIvyMediaTools() {
        auto& mcp_server = McpServer::GetInstance();

        mcp_server.AddTool("self.ivy.play_music",
            "播放音乐。keyword: 歌曲/歌手/歌词片段关键词；dance=true 时同步触发连续舞蹈。",
            PropertyList({Property("keyword", kPropertyTypeString, ""),
                          Property("dance", kPropertyTypeBoolean, false)}),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string keyword = properties["keyword"].value<std::string>();
                bool dance = properties["dance"].value<bool>();
                if (keyword.empty()) return "缺少 keyword 参数";
                if (miot_client_ != nullptr) miot_client_->McpPlayMusic(keyword);
                if (dance && music_player_ != nullptr) OttoControllerQueueContinuousDance();
                return true;
            });

        mcp_server.AddTool("self.ivy.control_playback",
            "播放控制。action: play(继续)/pause(暂停)/next(下一首)/previous(上一首)/stop(停止)。",
            PropertyList({Property("action", kPropertyTypeString, "play")}),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string action = properties["action"].value<std::string>();
                if (miot_client_ != nullptr) miot_client_->McpControlPlayback(action);
                return true;
            });

        mcp_server.AddTool("self.ivy.play_album",
            "播放专辑。album: 专辑名，从服务器拉取歌单连播。",
            PropertyList({Property("album", kPropertyTypeString, "")}),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string album = properties["album"].value<std::string>();
                if (album.empty()) return "缺少 album 参数";
                if (miot_client_ != nullptr) miot_client_->McpPlayAlbum(album);
                return true;
            });

        mcp_server.AddTool("self.ivy.play_radio",
            "播放电台。station: 电台标识(如 qtfm-dszs)；name: 可选显示名。",
            PropertyList({Property("station", kPropertyTypeString, ""),
                          Property("name", kPropertyTypeString, "")}),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string station = properties["station"].value<std::string>();
                std::string name = properties["name"].value<std::string>();
                if (station.empty()) return "缺少 station 参数";
                if (miot_client_ != nullptr) miot_client_->McpPlayRadio(station, name);
                return true;
            });

        mcp_server.AddTool("self.ivy.show_weather",
            "天气播报。city: 城市名(如 北京)；city_enc: 可选 URL 编码(缺省自动编码)。",
            PropertyList({Property("city", kPropertyTypeString, ""),
                          Property("city_enc", kPropertyTypeString, "")}),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string city = properties["city"].value<std::string>();
                std::string city_enc = properties["city_enc"].value<std::string>();
                if (city.empty()) return "缺少 city 参数";
                if (miot_client_ != nullptr) miot_client_->McpPlayWeather(city, city_enc);
                return true;
            });

        mcp_server.AddTool("self.ivy.play_course",
            "学习卡片/课程。key: 课程卡片 key；word: 可选显示词；course=true 为课程连播。",
            PropertyList({Property("key", kPropertyTypeString, ""),
                          Property("word", kPropertyTypeString, ""),
                          Property("course", kPropertyTypeBoolean, false)}),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string key = properties["key"].value<std::string>();
                std::string word = properties["word"].value<std::string>();
                bool course = properties["course"].value<bool>();
                if (key.empty()) return "缺少 key 参数";
                if (miot_client_ != nullptr) miot_client_->McpPlayStudy(key, word, course);
                return true;
            });

        mcp_server.AddTool("self.ivy.show_message",
            "在屏幕上显示文本消息。",
            PropertyList({Property("text", kPropertyTypeString, "")}),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string text = properties["text"].value<std::string>();
                if (!text.empty() && music_player_ != nullptr) music_player_->ShowMessage(text);
                return true;
            });

        mcp_server.AddTool("self.ivy.media_status",
            "查询当前媒体播放状态。",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                cJSON* status = cJSON_CreateObject();
                if (music_player_ == nullptr) {
                    cJSON_AddBoolToObject(status, "playing", false);
                    cJSON_AddBoolToObject(status, "paused", false);
                } else {
                    cJSON_AddBoolToObject(status, "playing", music_player_->IsPlaying());
                    cJSON_AddBoolToObject(status, "paused", music_player_->IsPaused());
                }
                char* s = cJSON_PrintUnformatted(status);
                std::string result(s);
                free(s);
                cJSON_Delete(status);
                return result;
            });

        ESP_LOGI(TAG, "MCP 媒体工具(self.ivy.*)注册完成");
    }

    void InitializeAudioCodec() {
        if (hw_config_.audio_use_simplex) {
            audio_codec_ = new NoAudioCodecSimplex(
                hw_config_.audio_input_sample_rate, hw_config_.audio_output_sample_rate,
                hw_config_.audio_i2s_spk_gpio_bclk, hw_config_.audio_i2s_spk_gpio_lrck,
                hw_config_.audio_i2s_spk_gpio_dout, hw_config_.audio_i2s_mic_gpio_sck,
                hw_config_.audio_i2s_mic_gpio_ws, hw_config_.audio_i2s_mic_gpio_din);
        } else {
            audio_codec_ = new NoAudioCodecDuplex(
                hw_config_.audio_input_sample_rate, hw_config_.audio_output_sample_rate,
                hw_config_.audio_i2s_gpio_bclk, hw_config_.audio_i2s_gpio_ws,
                hw_config_.audio_i2s_gpio_dout, hw_config_.audio_i2s_gpio_din);
        }
    }

public:
    OttoRobot()
        : boot_button_(BOOT_BUTTON_GPIO), audio_codec_(nullptr), miot_client_(nullptr), music_player_(nullptr) {
        // 沿用上游 otto-robot (v2.4.2) 的硬件版本选择结构，但本板固定为无摄像头硬件
        // （24pin ST7789 + INMP441 + MAX98357），仅支持 OTTO_VERSION_NO_CAMERA。
        // 按 OTTO_HARDWARE_VERSION 选择 hw_config，便于后续直接 git merge 吸收上游板级改进。
#if OTTO_HARDWARE_VERSION == OTTO_VERSION_AUTO
        // 自动检测：本板无摄像头，直接按无摄像头配置初始化
        hw_config_ = OTTO_V1_CONFIG;
        ESP_LOGI(TAG, "硬件版本: 自动检测 -> 无摄像头版 (OTTO_V1_CONFIG)");
#elif OTTO_HARDWARE_VERSION == OTTO_VERSION_CAMERA
        // 本板不含摄像头，强制摄像头配置会在硬件上失败
        #error "otto-robot-v1 不支持 OTTO_VERSION_CAMERA，请改用上游 otto-robot 板"
#elif OTTO_HARDWARE_VERSION == OTTO_VERSION_NO_CAMERA
        hw_config_ = OTTO_V1_CONFIG;
        ESP_LOGI(TAG, "硬件版本: 无摄像头版 (OTTO_V1_CONFIG)");
#else
        #error "OTTO_HARDWARE_VERSION 设置无效，请使用 OTTO_VERSION_AUTO / OTTO_VERSION_CAMERA / OTTO_VERSION_NO_CAMERA"
#endif

        InitializeSpi();
        InitializeLcdDisplay();
        InitializeButtons();
        InitializePowerManager();
        InitializeAudioCodec();

        InitializeOttoController();
        RegisterIvyMediaTools();
        ws_control_server_ = nullptr;
        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override { return audio_codec_; }

    virtual Display* GetDisplay() override { return display_; }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight* backlight = nullptr;
        if (backlight == nullptr) {
            backlight =
                new PwmBacklight(hw_config_.display_backlight_pin, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        }
        return backlight;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        charging = power_manager_->IsCharging();
        discharging = !charging;
        level = power_manager_->GetBatteryLevel();
        return true;
    }

    void OnUserInteract() override {
        // 用户唤醒（按键切换对话/唤醒词）：停止本地 MIOT 音乐播放，避免与对话音频冲突
        if (music_player_ != nullptr && music_player_->IsPlaying()) {
            ESP_LOGI(TAG, "User interaction, stopping MIOT music playback");
            music_player_->StopPlay();
        }
        // 用户打断：停止当前舞蹈/动作并归位
        OttoControllerStopAll();
    }
};

DECLARE_BOARD(OttoRobot);
