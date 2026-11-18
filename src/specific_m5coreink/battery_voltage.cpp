#include <specific_m5coreink/battery_voltage.hpp>

#include <ctime>
#include <mutex>
#include <string>

#include <driver/adc.h>
#include <esp_adc_cal.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <secure_upload.hpp>
#include <measurements.hpp>
#include <util/nvs.hpp>

constexpr const char *BATTERY_MEASUREMENT_PROPERTY_NAME = "battery_voltage__V";
constexpr const char *BATTERY_SOC_PROPERTY_NAME = "battery_soc__0";		 // 0..1, like rel_humidity__0
constexpr const char *BATTERY_RUNTIME_PROPERTY_NAME = "battery_runtime__h"; // only on battery

namespace M5CoreInkSpecific
{
    namespace
    {
        constexpr const char *TAG = "Battery";
        constexpr const char *NVS_NAMESPACE = "twomes_storage";
        constexpr const char *NVS_KEY = "battery"; // BatteryMonitor::Serialize; written rarely (see Update)
        constexpr int64_t BOOT_READING_VALID_US = 120 * 1000000LL;

        static bool s_initialized = false;
        static esp_adc_cal_characteristics_t s_adcChars;

        std::mutex s_mutex;
        BatteryMonitor::Model s_model;
        BatteryMonitor::State s_state;
        bool s_stateLoaded = false;
        BatteryMonitor::Reading s_reading;
        bool s_haveReading = false;
        int64_t s_bootReadingUs = -1; // esp_timer time of the reading at boot; -1: none

        auto secureUploadQueue = SecureUpload::Queue::GetInstance();

        void InitializeADC()
        {
            if (s_initialized)
                return;

            // Setup ADC1 channel 7 (gpio35).
            adc1_config_channel_atten(ADC1_CHANNEL_7, ADC_ATTEN_11db);

            // Calculate characteristic of adc1.
            esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 3600, &s_adcChars);

            // Add formatters for the battery properties.
            Measurements::Measurement::AddFormatter(BATTERY_MEASUREMENT_PROPERTY_NAME, "%.2f");
            Measurements::Measurement::AddFormatter(BATTERY_SOC_PROPERTY_NAME, "%.2f");
            Measurements::Measurement::AddFormatter(BATTERY_RUNTIME_PROPERTY_NAME, "%.1f");

            s_initialized = true;
        }

        /** Battery voltage in V; the average of a few ADC readings (the ADC of the ESP32 is noisy). */
        float ReadVoltage()
        {
            InitializeADC();
            constexpr int SAMPLES = 8;
            uint32_t sum_mV = 0;
            for (int i = 0; i < SAMPLES; i++)
                sum_mV += esp_adc_cal_raw_to_voltage(adc1_get_raw(ADC1_CHANNEL_7), &s_adcChars);
            // Voltage divider on the CoreInk: 20.0k over 5.1k.
            return static_cast<float>(sum_mV) / SAMPLES * 25.1f / 5.1f / 1000.0f;
        }

        /** Update the monitor with a voltage; the caller holds s_mutex. */
        void UpdateLocked(double voltage)
        {
            if (!s_stateLoaded)
            {
                std::string text;
                if (NVS::Get(NVS_NAMESPACE, NVS_KEY, text) == ESP_OK && !BatteryMonitor::Parse(text, s_state))
                    ESP_LOGW(TAG, "Ignoring unreadable battery state in NVS: %s", text.c_str());
                s_stateLoaded = true;
            }

            time_t now = time(nullptr);
            bool dirty = false;
            if (now < 1704067200) // before 2024: the clock is not set yet, so do not learn (or store) anything
            {
                BatteryMonitor::State copy = s_state;
                s_reading = BatteryMonitor::Update(copy, voltage, 0, s_model, dirty);
                dirty = false;
            }
            else
                s_reading = BatteryMonitor::Update(s_state, voltage, now, s_model, dirty);
            s_haveReading = true;

            if (dirty)
            {
                auto err = NVS::Set(NVS_NAMESPACE, NVS_KEY, BatteryMonitor::Serialize(s_state));
                if (err != ESP_OK)
                    ESP_LOGW(TAG, "Could not store the battery state: %s", esp_err_to_name(err));
            }
            ESP_LOGD(TAG, "%.3f V, charge %.0f%%, %s, %.1f h left, level %d, factor %.2f", voltage, s_reading.soc * 100,
                     s_reading.usb ? "USB" : "battery", s_reading.hours, static_cast<int>(s_reading.level), s_state.factor);
        }
    } // namespace

    void BatteryAtBoot()
    {
        float voltage = ReadVoltage();
        std::lock_guard<std::mutex> lock(s_mutex);
        UpdateLocked(voltage);
        s_bootReadingUs = esp_timer_get_time();
    }

    bool GetBatteryReading(BatteryMonitor::Reading &reading)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_haveReading)
            return false;
        reading = s_reading;
        return true;
    }

    void SetBatteryUsePerHour(double joulesPerHour)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (joulesPerHour > 0)
            s_model.perHour_J = joulesPerHour;
    }

    void BatteryVoltageTask(void *taskInfo)
    {
        // Measure battery voltage (with the radio on: a little lower than at rest).
        float batteryVoltage = ReadVoltage();

        BatteryMonitor::Reading reading;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            // A device that stays on (USB power, or rounds too close together to switch off) has no fresh resting
            // voltage from the boot: use this one then.
            if (s_bootReadingUs < 0 || esp_timer_get_time() - s_bootReadingUs > BOOT_READING_VALID_US)
                UpdateLocked(batteryVoltage);
            reading = s_reading;
        }

        // Send data to queue.
        Measurements::Measurement batteryVoltageMeasurement(BATTERY_MEASUREMENT_PROPERTY_NAME, batteryVoltage);
        secureUploadQueue.AddMeasurement(batteryVoltageMeasurement);

        Measurements::Measurement socMeasurement(BATTERY_SOC_PROPERTY_NAME, static_cast<float>(reading.soc));
        secureUploadQueue.AddMeasurement(socMeasurement);

        if (!reading.usb && reading.hours >= 0)
        {
            Measurements::Measurement runtimeMeasurement(BATTERY_RUNTIME_PROPERTY_NAME, static_cast<float>(reading.hours));
            secureUploadQueue.AddMeasurement(runtimeMeasurement);
        }
    }
} // namespace M5CoreInkSpecific
