#ifndef __POWER_MANAGER_H__
#define __POWER_MANAGER_H__

#include <driver/gpio.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_log.h>
#include <esp_timer.h>

class PowerManager {
private:
    // 电池电量区间-分压电阻为300k/100k（4:1），ESP32-S3 12dB ADC 斜率实测约0.834mV/LSB
    // 满电4.2V对应ADC约1260，低压3.3V对应约990
    static constexpr struct {
        uint16_t adc;
        uint8_t level;
    } BATTERY_LEVELS[] = {{990, 0}, {1260, 100}};
    static constexpr size_t BATTERY_LEVELS_COUNT = 2;
    static constexpr size_t ADC_VALUES_COUNT = 10;

    // 无充电检测引脚时，通过电压抬升推断充电状态
    static constexpr size_t ADC_AVG_HISTORY_COUNT = 6;  // 二次平滑窗口（秒）
    static constexpr uint16_t CHARGING_DELTA = 8;       // 充电时电压抬升阈值（ADC counts）
    static constexpr uint8_t CHARGING_CONFIRM_SEC = 5;  // 连续确认充电秒数
    static constexpr uint8_t CHARGING_RELEASE_SEC = 5;  // 连续下降解除充电秒数
    static constexpr uint16_t BASELINE_SMOOTH = 32;     // 静止基线EMA平滑系数

    esp_timer_handle_t timer_handle_ = nullptr;
    gpio_num_t charging_pin_;
    adc_unit_t adc_unit_;
    adc_channel_t adc_channel_;
    uint16_t adc_values_[ADC_VALUES_COUNT];
    size_t adc_values_index_ = 0;
    size_t adc_values_count_ = 0;
    uint8_t battery_level_ = 100;
    bool is_charging_ = false;
    inline static bool battery_update_paused_ = false;  // 静态标志：是否暂停电量更新

    adc_oneshot_unit_handle_t adc_handle_;

    // 充电推断状态
    uint16_t adc_average_ = 0;
    uint16_t avg_history_[ADC_AVG_HISTORY_COUNT] = {0};
    size_t avg_history_index_ = 0;
    size_t avg_history_count_ = 0;
    uint32_t baseline_adc_ = 0;
    bool baseline_initialized_ = false;
    uint8_t charging_confirm_ = 0;
    uint8_t charging_release_ = 0;

    void CheckBatteryStatus() {
      // 如果电量更新被暂停（动作进行中），则跳过更新
      if (battery_update_paused_) {
        return;
      }
      
      ReadBatteryAdcData();

      if (charging_pin_ == GPIO_NUM_NC) {
        UpdateChargingStatus();
      } else {
        is_charging_ = gpio_get_level(charging_pin_) == 0;
      }
    }

    void ReadBatteryAdcData() {
        int adc_value;
        ESP_ERROR_CHECK(adc_oneshot_read(adc_handle_, adc_channel_, &adc_value));

        adc_values_[adc_values_index_] = adc_value;
        adc_values_index_ = (adc_values_index_ + 1) % ADC_VALUES_COUNT;
        if (adc_values_count_ < ADC_VALUES_COUNT) {
            adc_values_count_++;
        }

        uint32_t average_adc = 0;
        for (size_t i = 0; i < adc_values_count_; i++) {
            average_adc += adc_values_[i];
        }
        average_adc /= adc_values_count_;
        adc_average_ = average_adc;

        CalculateBatteryLevel(average_adc);

        ESP_LOGI("PowerManager", "ADC值: %d 平均值: %ld 电量: %u%% 充电: %s", adc_value,
                 average_adc, battery_level_, is_charging_ ? "是" : "否");
    }

    // 通过电压趋势推断充电状态：USB充电时电池端电压会被抬升，持续高于静止基线即视为充电
    void UpdateChargingStatus() {
      avg_history_[avg_history_index_] = adc_average_;
      avg_history_index_ = (avg_history_index_ + 1) % ADC_AVG_HISTORY_COUNT;
      if (avg_history_count_ < ADC_AVG_HISTORY_COUNT) {
        avg_history_count_++;
      }

      uint32_t sum = 0;
      for (size_t i = 0; i < avg_history_count_; i++) {
        sum += avg_history_[i];
      }
      uint16_t recent_avg = sum / avg_history_count_;

      if (!baseline_initialized_) {
        baseline_adc_ = recent_avg;
        baseline_initialized_ = true;
      }

      if (is_charging_) {
        // 解除充电：电量已满，或电压回落到基线附近（拔电/停止充电）
        if (battery_level_ >= 100 || recent_avg <= baseline_adc_ + CHARGING_DELTA / 2) {
          if (++charging_release_ >= CHARGING_RELEASE_SEC) {
            is_charging_ = false;
            charging_release_ = 0;
            baseline_adc_ = recent_avg;  // 重新以当前电压为基线
          }
        } else {
          charging_release_ = 0;
        }
      } else {
        // 静止基线慢速跟随当前电压
        baseline_adc_ = (baseline_adc_ * (BASELINE_SMOOTH - 1) + recent_avg) / BASELINE_SMOOTH;

        // 电压明显高于基线且持续 → 判定充电
        if (recent_avg > baseline_adc_ + CHARGING_DELTA) {
          if (++charging_confirm_ >= CHARGING_CONFIRM_SEC) {
            is_charging_ = true;
            charging_confirm_ = 0;
          }
        } else {
          charging_confirm_ = 0;
        }
      }
    }

    void CalculateBatteryLevel(uint32_t average_adc) {
        if (average_adc <= BATTERY_LEVELS[0].adc) {
            battery_level_ = 0;
        } else if (average_adc >= BATTERY_LEVELS[BATTERY_LEVELS_COUNT - 1].adc) {
            battery_level_ = 100;
        } else {
            float ratio = static_cast<float>(average_adc - BATTERY_LEVELS[0].adc) /
                          (BATTERY_LEVELS[1].adc - BATTERY_LEVELS[0].adc);
            battery_level_ = ratio * 100;
        }
    }

public:
    PowerManager(gpio_num_t charging_pin, adc_unit_t adc_unit = ADC_UNIT_2,
                 adc_channel_t adc_channel = ADC_CHANNEL_3)
        : charging_pin_(charging_pin), adc_unit_(adc_unit), adc_channel_(adc_channel) {

      if (charging_pin_ != GPIO_NUM_NC) {
        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pin_bit_mask = (1ULL << charging_pin_);
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&io_conf);
        ESP_LOGI("PowerManager", "充电检测引脚配置完成: GPIO%d", charging_pin_);
      } else {
        ESP_LOGI("PowerManager", "充电检测引脚未配置，不进行充电状态检测");
      }

        esp_timer_create_args_t timer_args = {
            .callback =
                [](void* arg) {
                    PowerManager* self = static_cast<PowerManager*>(arg);
                    self->CheckBatteryStatus();
                },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "battery_check_timer",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_handle_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(timer_handle_, 1000000));  // 1秒

        InitializeAdc();
    }

    void InitializeAdc() {
      adc_oneshot_unit_init_cfg_t init_config = {
          .unit_id = adc_unit_,
          .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
          .ulp_mode = ADC_ULP_MODE_DISABLE,
      };
      ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle_));

      adc_oneshot_chan_cfg_t chan_config = {
          .atten = ADC_ATTEN_DB_12,
          .bitwidth = ADC_BITWIDTH_12,
      };

      ESP_ERROR_CHECK(
          adc_oneshot_config_channel(adc_handle_, adc_channel_, &chan_config));
    }

    ~PowerManager() {
        if (timer_handle_) {
            esp_timer_stop(timer_handle_);
            esp_timer_delete(timer_handle_);
        }
        if (adc_handle_) {
            adc_oneshot_del_unit(adc_handle_);
        }
    }

    bool IsCharging() { return is_charging_; }

    uint8_t GetBatteryLevel() { return battery_level_; }

    // 暂停/恢复电量更新（用于动作执行时屏蔽更新）
    static void PauseBatteryUpdate() { battery_update_paused_ = true; }
    static void ResumeBatteryUpdate() { battery_update_paused_ = false; }
};
#endif  // __POWER_MANAGER_H__