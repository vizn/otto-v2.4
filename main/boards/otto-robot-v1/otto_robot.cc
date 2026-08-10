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
        : boot_button_(BOOT_BUTTON_GPIO), audio_codec_(nullptr) {
        // 旧版 otto-robot 硬件（无摄像头、24pin ST7789、INMP441 + MAX98357）
        hw_config_ = OTTO_V1_CONFIG;

        InitializeSpi();
        InitializeLcdDisplay();
        InitializeButtons();
        InitializePowerManager();
        InitializeAudioCodec();

        InitializeOttoController();
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
};

DECLARE_BOARD(OttoRobot);
