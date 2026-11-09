#include <generic_esp_32.hpp>

#include <esp_log.h>
#include <esp_system.h>
#include <esp_ota_ops.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_wifi.h>
#include <wifi_provisioning/manager.h>
#include <esp_sntp.h>
#include <esp32/rom/crc.h>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>

#include <cJSON.h>

#include <cstdlib>
#include <cstring>
#include <vector>

#include <util/error.hpp>
#include <util/nvs.hpp>
#include <util/format.hpp>
#include <util/delay.hpp>
#include <util/buttons.hpp>
#include <util/buzzer.hpp>
#include <util/screen.hpp>
#include <util/provisioning_code.hpp>
#include <specific_m5coreink/powerpin.h>
#include <specific_m5coreink/rtc.h>
#include <scheduler.hpp>
#include <generic_tasks.hpp>
#include <measurements.hpp>
#include <secure_upload.hpp>
#include <ota_firmware_updater.hpp>

#ifdef CONFIG_TWOMES_PROV_TRANSPORT_BLE
#include <wifi_provisioning/scheme_ble.h>
#endif // CONFIG_EXAMPLE_PROV_TRANSPORT_BLE

#ifdef CONFIG_TWOMES_PRESENCE_DETECTION
#include <control_panel.hpp>
#endif // CONFIG_TWOMES_PRESENCE_DETECTION

#ifdef ESP32DEV
#include "platform_esp32_dev.hpp"
#endif // ESP32DEV
#if M5STACK_COREINK
#include "platform_m5stack_coreink.hpp"
#include <specific_m5coreink/power_off_timeout.hpp>
#endif // M5STACK_COREINK

#define WIFI_CONNECTED_EVENT BIT0
#define WIFI_DISCONNECTED_EVENT BIT1

// Server selection. CONFIG_TWOMES_SERVER_HOSTNAME takes precedence, so a build can point to its own server
// without code changes, e.g. in platformio.ini:
//   build_flags = '-D CONFIG_TWOMES_SERVER_HOSTNAME="api.example.org"'
#if defined(CONFIG_TWOMES_SERVER_HOSTNAME)
constexpr const char *TWOMES_SERVER_HOSTNAME = CONFIG_TWOMES_SERVER_HOSTNAME;
constexpr const char *TWOMES_SERVER = "https://" CONFIG_TWOMES_SERVER_HOSTNAME;
#elif defined(CONFIG_TWOMES_TEST_SERVER)
constexpr const char *TWOMES_SERVER_HOSTNAME = "api.tst.energiegedrag.nl";
constexpr const char *TWOMES_SERVER = "https://api.tst.energiegedrag.nl";
#elif defined(CONFIG_TWOMES_PRODUCTION_SERVER)
constexpr const char *TWOMES_SERVER_HOSTNAME = "api.energiegedrag.nl";
constexpr const char *TWOMES_SERVER = "https://api.energiegedrag.nl";
#else
#error "No server configured: define CONFIG_TWOMES_SERVER_HOSTNAME, CONFIG_TWOMES_TEST_SERVER or CONFIG_TWOMES_PRODUCTION_SERVER"
#endif

// Other servers this device may be pointed to during provisioning (custom endpoint "nfh-config"), e.g. a
// test server next to production. Comma-separated host names; only these and TWOMES_SERVER_HOSTNAME are accepted:
//   build_flags = '-D CONFIG_TWOMES_ALT_SERVER_HOSTNAMES="api.tst.energiegedrag.nl"'
#if defined(CONFIG_TWOMES_ALT_SERVER_HOSTNAMES)
constexpr const char *TWOMES_ALT_SERVER_HOSTNAMES = CONFIG_TWOMES_ALT_SERVER_HOSTNAMES;
#else
constexpr const char *TWOMES_ALT_SERVER_HOSTNAMES = "";
#endif
constexpr const char *PROV_CONFIG_ENDPOINT = "nfh-config";

constexpr int WIFI_MAX_CONNECT_TRIES = 10;
constexpr int WIFI_MAX_WAIT_MS = 30 * 1000; // 30 seconds.

constexpr int NTP_MAX_SYNC_TRIES = 10;
constexpr int NTP_MAX_WAIT_MS = 2 * 1000; // 2 seconds.

constexpr const char *TAG = "Twomes generic ESP32 firmware library";
constexpr const char *NVS_NAMESPACE = "twomes_storage";
constexpr const char *DEVICE_SERVICE_NAME_PREFIX = "TWOMES-";
constexpr const char *QR_CODE_PAYLOAD_TEMPLATE = "{\n\"ver\":\"v1\",\n\"name\":\"%s\",\n\"pop\":\"%s\",\n\"transport\":\"ble\"\n}";
constexpr const char *POST_DEVICE_PAYLOAD_TEMPLATE = "{\n\"name\":\"%s\",\n\"device_type\":\"%s\",\n\"activation_token\":\"%s\"\n}";
constexpr const char *ACTIVATION_POST_REQUEST_TEMPLATE = "{\"name\":\"%s\"}";
constexpr const char *POST_PROVISIONING_INFO_TEXT = "  Info? Scan!  ";
constexpr int LONG_BUTTON_PRESS_DURATION = 10 * 2; // Number of half seconds to wait: (10 s * 2 halfseconds)

constexpr int PRE_PROVISIONING_POWER_OFF_TIMEOUT_S = 15 * 60; // 15 minutes.

// Screen and QR definitions
constexpr int SCREEN_WIDTH = 200;
constexpr int SCREEN_HEIGHT = 200;
constexpr int QR_PADDING = 10;

namespace GenericESP32Firmware
{
    namespace
    {
        static std::string s_deviceTypeName;

        static EventGroupHandle_t s_wifiEventGroup;
        static bool s_wifiInitialized = false;
        static int s_wifiConnectRetries = 0;

        static bool s_postProvisioningNeeded = false;

        static gpio_num_t s_wifiResetLED = LED_WIFI_RESET;

#ifdef M5STACK_COREINK
        static Screen s_screen;

        static M5CoreInkSpecific::PowerOffTimeout s_powerOffTimeout(PRE_PROVISIONING_POWER_OFF_TIMEOUT_S);
#endif // M5STACK_COREINK

        // Forward declare.
        std::string GetBearer();

#ifdef M5STACK_COREINK
        void PowerOff()
        {
            // Buzz the buzzer 1000ms to signal power off.
            Buzzer::Buzz(1000);

            auto bearer = GetBearer();
            if (!bearer.empty())
            {
                // Device was provisioned and activated.
                s_screen.DisplayInfoQR();
            }
            // If the device was not provisioned and activated,
            // the provisioning QR will still be on screen.

            powerpin_reset();
        }
#endif // M5STACK_COREINK

        /**
         * Event handler for WiFi and provisioning events.
         */
        void WifiEventHandler(void *arg, esp_event_base_t eventBase, int32_t eventID, void *eventData)
        {
            if (eventBase == WIFI_EVENT)
            {
                switch (eventID)
                {
                case WIFI_EVENT_STA_START:
                    esp_wifi_start();
                    esp_wifi_connect();
                    break;
                case WIFI_EVENT_STA_DISCONNECTED:
                    if (s_wifiConnectRetries < WIFI_MAX_CONNECT_TRIES)
                    {
                        ESP_LOGI(TAG, "Connect to AP retry.");
                        esp_wifi_connect();
                        s_wifiConnectRetries++;
                    }
                    else
                    {
                        ESP_LOGD(TAG, "Disconnected.");
                        xEventGroupSetBits(s_wifiEventGroup, WIFI_DISCONNECTED_EVENT);
                    }
                    break;
                default:
                    break;
                }
            }
            else if (eventBase == IP_EVENT)
            {
                switch (eventID)
                {
                case IP_EVENT_STA_GOT_IP:
                    ESP_LOGD(TAG, "Connected with IP Address:" IPSTR,
                             IP2STR(&((ip_event_got_ip_t *)eventData)->ip_info.ip));
                    xEventGroupSetBits(s_wifiEventGroup, WIFI_CONNECTED_EVENT);
                    s_wifiConnectRetries = 0;
                    break;
                default:
                    break;
                }
            }
            else if (eventBase == WIFI_PROV_EVENT)
            {
                switch (eventID)
                {
                case WIFI_PROV_START:
                    ESP_LOGD(TAG, "Provisioning started");
                    break;
                case WIFI_PROV_CRED_RECV:
                    ESP_LOGD(TAG, "Received Wi-Fi credentials");
                    break;
                case WIFI_PROV_CRED_FAIL:
                    ESP_LOGE(TAG,
                             "Provisioning failed!\n\tReason : %s"
                             "\n\tRestarting in 5 seconds and try again...",
                             (*(wifi_prov_sta_fail_reason_t *)eventData == WIFI_PROV_STA_AUTH_ERROR) ? "Wi-Fi station authentication failed" : "Wi-Fi access-point not found");
                    vTaskDelay(Delay::Seconds(5));
                    ResetWireless();
                    break;
                case WIFI_PROV_CRED_SUCCESS:
                    ESP_LOGD(TAG, "Provisioning successful");
                    break;
                case WIFI_PROV_END:
                    // De-initialize manager once provisioning is finished.
                    wifi_prov_mgr_deinit();
                    break;
                default:
                    break;
                }
            }
        }

        /**
         * Activation secret, also the proof of possession for provisioning ("dat"), as decimal text.
         *
         * Devices that already have a 32-bit "dat" in NVS keep it: the server and any running provisioning
         * know that value. New devices get a 64-bit secret ("pop64"), which the QR code v2 has room for.
         */
        std::string GetDat()
        {
            uint32_t legacy;
            auto err = NVS::Get(NVS_NAMESPACE, "dat", legacy);
            if (err == ESP_OK)
                return std::to_string(legacy);
            if (err != ESP_ERR_NVS_NOT_FOUND)
                Error::Check(err, TAG);

            std::string pop;
            err = NVS::Get(NVS_NAMESPACE, "pop64", pop);
            if (err == ESP_OK && !pop.empty())
                return pop;

            ESP_LOGD(TAG, "No activation secret in NVS yet. Creating a 64-bit one.");
            uint64_t value = (static_cast<uint64_t>(esp_random()) << 32) | esp_random();
            if (value == 0)
                value = 1;
            pop = std::to_string(value);
            err = NVS::Set(NVS_NAMESPACE, "pop64", pop);
            Error::Check(err, TAG);
            return pop;
        }

        /** Content of the provisioning QR code: v2 ("NFH:" + Base32) unless the build asks for the old JSON. */
        std::string GetProvisioningQrPayload()
        {
            auto pop = GetDat();
#ifdef CONFIG_NFH_QR_V1_JSON
            return Format::String(QR_CODE_PAYLOAD_TEMPLATE, GetDeviceServiceName().c_str(), pop.c_str());
#else
            uint16_t deviceTypeHash = ~crc16_be((uint16_t)~0x0000, (const uint8_t *)s_deviceTypeName.c_str(), s_deviceTypeName.length());
            uint8_t mac[6] = {};
            esp_wifi_get_mac(WIFI_IF_STA, mac);
            return ProvisioningCode::Make(deviceTypeHash, &mac[3], std::strtoull(pop.c_str(), nullptr, 10));
#endif
        }

        /**
         * Get bearer from NVS.
         *
         * @returns bearer.
         */
        /** Host names this device may upload to: the built-in one first, then the alternatives. */
        std::vector<std::string> AllowedServerHostnames()
        {
            std::vector<std::string> hosts{TWOMES_SERVER_HOSTNAME};
            std::string alt = TWOMES_ALT_SERVER_HOSTNAMES;
            size_t start = 0;
            while (start < alt.size())
            {
                size_t comma = alt.find(',', start);
                std::string host = alt.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                if (!host.empty())
                    hosts.push_back(host);
                if (comma == std::string::npos)
                    break;
                start = comma + 1;
            }
            return hosts;
        }

        bool IsAllowedServer(const std::string &host)
        {
            for (const auto &h : AllowedServerHostnames())
                if (h == host)
                    return true;
            return false;
        }

        /**
         * Handler of the provisioning endpoint "nfh-config". The app sends {"server":"<host>"} before it sends the
         * Wi-Fi credentials; the session is already protected by the proof of possession from the QR code.
         * Only host names compiled into the firmware are accepted, so a device cannot be pointed anywhere else.
         * Answers {"ok":true,"server":"<host>"} or {"ok":false,"error":"...","allowed":[...]}.
         */
        esp_err_t ProvConfigHandler(uint32_t sessionId, const uint8_t *inbuf, ssize_t inlen, uint8_t **outbuf,
                                    ssize_t *outlen, void *privData)
        {
            std::string request(reinterpret_cast<const char *>(inbuf), inbuf ? inlen : 0);
            auto json = cJSON_Parse(request.c_str());
            const char *server = json ? cJSON_GetStringValue(cJSON_GetObjectItem(json, "server")) : nullptr;
            auto reply = cJSON_CreateObject();
            if (server != nullptr && IsAllowedServer(server))
            {
                NVS::Set(NVS_NAMESPACE, "server_host", std::string(server));
                cJSON_AddBoolToObject(reply, "ok", true);
                cJSON_AddStringToObject(reply, "server", server);
                ESP_LOGI(TAG, "Server set during provisioning: %s", server);
            }
            else
            {
                cJSON_AddBoolToObject(reply, "ok", false);
                cJSON_AddStringToObject(reply, "error", server ? "server not allowed" : "expected {\"server\":\"<host>\"}");
                auto allowed = cJSON_AddArrayToObject(reply, "allowed");
                for (const auto &h : AllowedServerHostnames())
                    cJSON_AddItemToArray(allowed, cJSON_CreateString(h.c_str()));
                ESP_LOGW(TAG, "Provisioning asked for a server that is not allowed: %s", server ? server : "(none)");
            }
            if (json)
                cJSON_Delete(json);
            char *text = cJSON_PrintUnformatted(reply);
            cJSON_Delete(reply);
            if (text == nullptr)
                return ESP_ERR_NO_MEM;
            *outlen = strlen(text);
            *outbuf = reinterpret_cast<uint8_t *>(strdup(text)); // freed by protocomm
            cJSON_free(text);
            return *outbuf ? ESP_OK : ESP_ERR_NO_MEM;
        }

        std::string GetBearer()
        {
            std::string bearer;
            auto err = NVS::Get(NVS_NAMESPACE, "bearer", bearer);
            if (err == ESP_ERR_NVS_NOT_FOUND)
            {
                // The key does not exist.
                ESP_LOGD(TAG, "The bearer has not been initialized in NVS yet.");
                return bearer;
            }

            if (err != ESP_OK)
            {
                // Something went wrong besides the key not existing.
                Error::Check(err, TAG);
                return bearer;
            }

            return bearer;
        }

        /**
         * Set bearer in NVS.
         *
         * @param bearer Bearer to store in NVS.
         */
        void SetBearer(const std::string &bearer)
        {
            auto err = NVS::Set(NVS_NAMESPACE, "bearer", bearer);
            Error::CheckAppendName(err, TAG, "An error occured when setting bearer");
        }

        /**
         * Get Info URL from NVS.
         *
         * @returns Info URL.
         */
        std::string GetInfoURL()
        {
            std::string infoURL;
            auto err = NVS::Get(NVS_NAMESPACE, "info_url", infoURL);
            if (err == ESP_ERR_NVS_NOT_FOUND)
            {
                // The key does not exist.
                ESP_LOGD(TAG, "The info URL has not been initialized in NVS yet.");
                return infoURL;
            }

            if (err != ESP_OK)
            {
                // Something went wrong besides the key not existing.
                Error::Check(err, TAG);
                return infoURL;
            }

            return infoURL;
        }

        /**
         * Set info URL in NVS.
         *
         * @param infoURL Info URL to store in NVS.
         */
        void SetInfoURL(const std::string &infoURL)
        {
            auto err = NVS::Set(NVS_NAMESPACE, "info_url", infoURL);
            Error::CheckAppendName(err, TAG, "An error occured when setting info URL");
        }

        /**
         * Initialize wireless (WiFi).
         * This function does not turn the wireless radio on.
         *
         * @returns ESP error.
         */
        esp_err_t InitializeWireless()
        {
            ESP_LOGD(TAG, "Initializing wireless.");

            s_wifiEventGroup = xEventGroupCreate();

            auto err = esp_netif_init();
            if (Error::CheckAppendName(err, TAG, "An error occured when initializing netif"))
                return err;

            err = esp_event_loop_create_default();
            if (Error::CheckAppendName(err, TAG, "An error occured when creating event loop"))
                return err;

            err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiEventHandler, nullptr);
            if (Error::CheckAppendName(err, TAG, "An error occured when registering WIFI_EVENT handler"))
                return err;
            err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiEventHandler, nullptr);
            if (Error::CheckAppendName(err, TAG, "An error occured when registering IP_EVENT handler"))
                return err;
            err = esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &WifiEventHandler, nullptr);
            if (Error::CheckAppendName(err, TAG, "An error occured when registering WIFI_PROV_EVENT handler"))
                return err;

            // This function does not return an error.
            esp_netif_create_default_wifi_sta();

#ifdef CONFIG_TWOMES_PROV_TRANSPORT_SOFTAP
            esp_netif_create_default_wifi_ap();
#endif // CONFIG_TWOMES_PROV_TRANSPORT_SOFTAP

            wifi_init_config_t wifiInitConfig = WIFI_INIT_CONFIG_DEFAULT();
            err = esp_wifi_init(&wifiInitConfig);
            if (Error::CheckAppendName(err, TAG, "An error occured when initializing wifi"))
                return err;

            err = esp_wifi_set_mode(WIFI_MODE_STA);
            if (Error::CheckAppendName(err, TAG, "An error occured when setting wifi mode"))
                return err;

            return ESP_OK;
        }

        /**
         * Initialize GPIO based on defined platform.
         * Supported platforms:
         *   - ESP32DEV
         *   - M5STACK_COREINK
         *
         * @returns ESP error.
         */
        esp_err_t InitializeGPIO()
        {
#ifndef CONFIG_TWOMES_CUSTOM_GPIO
            ESP_LOGD(TAG, "Initializing GPIO.");

#ifdef ESP32DEV
            auto err = ESP32Dev::InitializeGPIO();
#endif // ESP32DEV

#ifdef M5STACK_COREINK
            auto err = M5StackCoreInk::InitializeGPIO();

            err = Buttons::ButtonPressHandler::AddButton(BUTTON_ON_OFF, "On-off button", 0, nullptr, PowerOff);
            if (Error::CheckAppendName(err, TAG, "An error occured when adding BUTTON_WIFI_RESET to handler"))
                return err;
#endif // M5STACK_COREINK
            if (Error::CheckAppendName(err, TAG, "An error occured when initializing GPIO"))
                return err;

            err = Buttons::ButtonPressHandler::AddButton(BUTTON_WIFI_RESET, "Wi-Fi reset", 0, nullptr, ResetWireless);
            if (Error::CheckAppendName(err, TAG, "An error occured when adding BUTTON_WIFI_RESET to handler"))
                return err;
#endif // CONFIG_TWOMES_CUSTOM_GPIO

            return ESP_OK;
        }

        /**
         * Initialize provisioning based on defined configuration.
         *
         * @returns ESP error.
         */
        esp_err_t InitializeProvisioning()
        {
            ESP_LOGD(TAG, "Initializing provisioning.");

            wifi_prov_mgr_config_t wifiProvConfig;
#ifdef CONFIG_TWOMES_PROV_TRANSPORT_BLE
            wifiProvConfig.scheme = wifi_prov_scheme_ble;
#endif // CONFIG_TWOMES_PROV_TRANSPORT_SOFTAP
#ifdef CONFIG_TWOMES_PROV_TRANSPORT_SOFTAP
            wifiProvConfig.scheme = wifi_prov_scheme_softap;
#endif // CONFIG_TWOMES_PROV_TRANSPORT_SOFTAP
            wifiProvConfig.scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE;
            wifiProvConfig.app_event_handler = WIFI_PROV_EVENT_HANDLER_NONE;

            auto err = wifi_prov_mgr_init(wifiProvConfig);
            if (Error::Check(err, TAG))
                return err;

            // Log the QR code.
            auto qrCodePayload = GetProvisioningQrPayload();
            ESP_LOGI(TAG,
                     "QR Code Payload: \n"
                     "\n\n%s\n\n",
                     qrCodePayload.c_str());

            return ESP_OK;
        }

        /**
         * Initialize and set the timezone.
         */
        void InitializeTimezone(const char *timezone)
        {
            ESP_LOGD(TAG, "Initializing timezone.");

            setenv("TZ", timezone, 0);
            tzset();
        }

        /**
         * Callback that is called when a timesync event happens.
         */
        void TimeSyncNotificationCallback(struct timeval *tv)
        {
            ESP_LOGI(TAG, "Time sync event notification received. The current time is: %s", ctime(&tv->tv_sec));

#ifdef M5STACK_COREINK
            // Update the RTC time.
            rtc_syncronize_rtc_time();
#endif // M5STACK_COREINK
        }

        /**
         * Determine if post provisioning is needed.
         *
         * This function will restart the ESP if needed.
         *
         * @returns true if post provisionings steps are needed.
         */
        bool PostProvisioningNeeded()
        {
            uint32_t postProvisioningDone = 0;
            NVS::Get(NVS_NAMESPACE, "ppDone", postProvisioningDone);
            if (postProvisioningDone == 0)
            {
                ESP_LOGD(TAG, "Post provisioning needs to happen");
                return true;
            }
            return false;
        }

        /**
         * This function will run only when the device is just provisioned.
         */
        void PostProvisioning()
        {
            // Immediately sync time.
            InitializeTimeSync();

            // Log the booted firmware version to the backend.
            auto appDescription = esp_ota_get_app_description();
            OTAFirmwareUpdater::LogFirmwareToBackend("booted_fw", appDescription->version);

            ESP_LOGD(TAG, "Sending a heartbeat once.");
            // Add a measurement formatter for the heartbeat property.
            Measurements::Measurement::AddFormatter("heartbeat", "%d");

            Measurements::Measurement measurement("heartbeat", 0, time(nullptr));
            SecureUpload::Queue::GetInstance().AddMeasurement(measurement);
            GenericTasks::UploadTask(nullptr);

            // Run all tasks in the scheduler once.
            Scheduler::RunAll();

            uint32_t ppDone = 1;
            auto err = NVS::Set(NVS_NAMESPACE, "ppDone", ppDone);
            Error::CheckAppendName(err, TAG, "An error occurred when setting NVS key ppDone");

            ESP_LOGD(TAG, "Post provisioning tasks ran");
        }

        /**
         * Start Wi-Fi.
         *
         * @returns ESP error.
         */
        esp_err_t StartWireless()
        {
            auto err = esp_wifi_start();
            Error::CheckAppendName(err, TAG, "An error occured when starting Wi-Fi.");

            if (s_wifiInitialized)
                return ESP_OK;

            for (int tries = 1; tries <= WIFI_MAX_CONNECT_TRIES; tries++)
            {
                auto bits = xEventGroupWaitBits(s_wifiEventGroup,
                                                WIFI_CONNECTED_EVENT,
                                                true,
                                                true,
                                                Delay::MilliSeconds(WIFI_MAX_WAIT_MS));
                // If Wi-Fi is connected, we are done.
                if (bits & WIFI_CONNECTED_EVENT)
                    break;

                ESP_LOGD(TAG,
                         "Failed to connect to Wi-Fi (%d/%d) at %s",
                         tries,
                         WIFI_MAX_CONNECT_TRIES,
                         esp_log_system_timestamp());

                if (tries == WIFI_MAX_CONNECT_TRIES)
                {
                    // Restart device after maximum tries.

                    ESP_LOGD(TAG, "Minimum free heap size: %d bytes", esp_get_minimum_free_heap_size());

                    ESP_LOGE(TAG, "Could still not get connection; restarting now.");

                    fflush(stdout);
                    esp_restart();
                }
            }

            return err;
        }

        /**
         * Start provisioning based on defined configuration.
         *
         * @returns ESP error.
         */
        esp_err_t StartProvisioning()
        {
            ESP_LOGD(TAG, "Starting provisioning.");

            // Prov manager was initialized in InitializeProvisioning().

            bool isProvisioned = false;
            auto err = wifi_prov_mgr_is_provisioned(&isProvisioned);
            if (Error::Check(err, TAG))
                return err;

            if (isProvisioned)
            {
                // Manager is no longer needed. De-initialize it.
                wifi_prov_mgr_deinit();

                ESP_LOGD(TAG, "Device is already provisioned.");

                return ESP_OK;
            }

#if M5STACK_COREINK
            auto qrCodePayload = GetProvisioningQrPayload();
            s_screen.DisplayQR(qrCodePayload, QR_PADDING);

            s_powerOffTimeout.Start();
#endif // M5STACK_COREINK

#ifdef CONFIG_TWOMES_PROV_TRANSPORT_BLE
            /* This step is only useful when scheme is wifi_prov_scheme_ble. This will
             * set a custom 128 bit UUID which will be included in the BLE advertisement
             * and will correspond to the primary GATT service that provides provisioning
             * endpoints as GATT characteristics. Each GATT characteristic will be
             * formed using the primary service UUID as base, with different auto assigned
             * 12th and 13th bytes (assume counting starts from 0th byte). The client side
             * applications must identify the endpoints by reading the User Characteristic
             * Description descriptor (0x2901) for each characteristic, which contains the
             * endpoint name of the characteristic.
             * LSB <---> MSB */
            uint8_t serviceUUID[] = {0xb4, 0xdf, 0x5a, 0x1c,
                                     0x3f, 0x6b, 0xf4, 0xbf,
                                     0xea, 0x4a, 0x82, 0x03,
                                     0x04, 0x90, 0x1a, 0x02};
            err = wifi_prov_scheme_ble_set_service_uuid(serviceUUID);
            if (Error::Check(err, TAG))
                return err;
#endif // CONFIG_TWOMES_PROV_TRANSPORT_BLE

            wifi_prov_security_t security = WIFI_PROV_SECURITY_1;
            auto datStr = GetDat();
            auto serviceName = GetDeviceServiceName();
            // Extra endpoint for configuration from the app (which server to use); create before starting.
            err = wifi_prov_mgr_endpoint_create(PROV_CONFIG_ENDPOINT);
            Error::Check(err, TAG);

            err = wifi_prov_mgr_start_provisioning(security, datStr.c_str(), serviceName.c_str(), datStr.c_str());
            if (Error::Check(err, TAG))
                return err;

            err = wifi_prov_mgr_endpoint_register(PROV_CONFIG_ENDPOINT, ProvConfigHandler, nullptr);
            Error::Check(err, TAG);

            // Wait until Wi-Fi connection is made.
            xEventGroupWaitBits(s_wifiEventGroup, WIFI_CONNECTED_EVENT, true, true, portMAX_DELAY);

            // WiFi was initialized during provisioning.
            s_wifiInitialized = true;

            // Signal that provisioning just happened.
            s_postProvisioningNeeded = true;

            return ESP_OK;
        }

        /**
         * Activate the device with the backend server.
         * If a bearer exists, this function returns immediately.
         */
        void ActivateDevice()
        {
            ESP_LOGD(TAG, "Activating device.");

            auto bearer = GetBearer();
            if (!bearer.empty())
            {
                // The bearer already exists.
                ESP_LOGD(TAG, "Existing bearer was found. Device is activated.");
                return;
            }

            HTTPUtil::buffer_t deviceActivationRequestData = Format::String(ACTIVATION_POST_REQUEST_TEMPLATE, GetDeviceServiceName().c_str());
            HTTPUtil::headers_t headersSend;
            headersSend["Authorization"] = Format::String("Bearer %s", GetDat().c_str());

            HTTPUtil::buffer_t dataReceive;
            HTTPUtil::headers_t headersReceive;
            auto statusCode = PostHTTPSToBackend(ENDPOINT_DEVICE_ACTIVATION,
                                                 deviceActivationRequestData,
                                                 headersSend,
                                                 dataReceive,
                                                 headersReceive,
                                                 false);
            if (statusCode != 200)
            {
                // Statuscode was not 200 (OK). Something went wrong
                ESP_LOGE(TAG, "Statuscode %u was received while attempting device activation.", statusCode);
                return;
            }

            if (dataReceive.empty())
            {
                // No bearer was received as a response.
                ESP_LOGE(TAG, "No bearer was received while attempting device activation.");
                return;
            }

            ESP_LOGD(TAG, "HTTP response:\n%s", dataReceive.c_str());

            auto json = cJSON_Parse(dataReceive.c_str());
            if (json == nullptr)
            {
                ESP_LOGE(TAG, "An error occured when parsing JSON.");
                return;
            }

            auto authorizationToken = cJSON_GetStringValue(cJSON_GetObjectItem(json, "authorization_token"));
            if (authorizationToken == nullptr)
            {
                ESP_LOGE(TAG, "No authorization_token in the activation response.");
                cJSON_Delete(json);
                return;
            }
            SetBearer(authorizationToken);

            // Optional: API v2 returned device_type.info_url; API v3 does not. Keep the default then.
            auto infoURL = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(json, "device_type"), "info_url"));
            if (infoURL != nullptr)
                SetInfoURL(infoURL);

            cJSON_Delete(json);
        }
    } // namespace

    void Initialize(const std::string &deviceTypeName)
    {
#ifdef M5STACK_COREINK
        // The first thing we need to do is enable battery power,
        // otherwise booting by the RTC signal will not succeed.
        powerpin_set();
#endif // M5STACK_COREINK

#ifdef CONFIG_TWOMES_OTA_FIRMWARE_UPDATE
        OTAFirmwareUpdater::CheckUpdateFinishedSuccessfully();
#endif // CONFIG_TWOMES_OTA_FIRMWARE_UPDATE

        s_deviceTypeName = deviceTypeName;

        ESP_LOGD(TAG, "Initializing Generic ESP32 Firmware, Version: %s", esp_ota_get_app_description()->version);

        NVS::Initialize();

        InitializeTimezone("UTC");

#ifdef M5STACK_COREINK
        rtc_initialize();
        rtc_syncronize_sys_time();
#endif // M5STACK_COREINK

        auto err = InitializeWireless();
        Error::CheckAppendName(err, TAG, "An error occured inside GenericFirmware::<unnamed>::InitializeWireless()");

#ifndef CONFIG_TWOMES_CUSTOM_GPIO
        err = InitializeGPIO();
        Error::CheckAppendName(err, TAG, "An error occured inside GenericFirmware::<unnamed>::InitializeGPIO()");
#endif // CONFIG_TWOMES_CUSTOM_GPIO

        err = InitializeProvisioning();
        Error::CheckAppendName(err, TAG, "An error occured inside GenericFirmware::<unnamed>::InitializeProvisioning()");

        err = StartProvisioning();
        Error::CheckAppendName(err, TAG, "An error occured inside GenericFirmware::<unnamed>::StartProvisioning()");

#ifdef M5STACK_COREINK
        // Cancel power off timeout since provisioning is done.
        s_powerOffTimeout.Cancel();
#endif // M5STACK_COREINK

        err = StartWireless();
        Error::CheckAppendName(err, TAG, "An error occured inside GenericFirmware::<unnamed>::StartWireless()");

#ifdef ESP32DEV
        // Sync time immediately because there is no RTC.
        InitializeTimeSync();
#endif

        ActivateDevice();
        if (GetBearer().empty())
        {
            // Activation failed (e.g. server unreachable). Uploads cannot work without a bearer,
            // so try again after a pause by restarting, as the firmware implicitly did before.
            ESP_LOGE(TAG, "Device activation failed; restarting in 60 s to try again.");
            vTaskDelay(Delay::Seconds(60));
            esp_restart();
        }

#ifdef CONFIG_TWOMES_PRESENCE_DETECTION
        ControlPanel::initialzeButtons();
#endif // CONFIG_TWOMES_PRESENCE

#ifdef M5STACK_COREINK
        // Show information about what this device does on the screen.
        s_screen.SetInfoQRDetails(GetInfoURL(), QR_PADDING, POST_PROVISIONING_INFO_TEXT);
        s_screen.DisplayInfoQR();
#endif // M5STACK_COREINK

        if (PostProvisioningNeeded())
            PostProvisioning();

        ESP_LOGI(TAG, "Finished initialization.");
    }

    std::string GetDeviceServiceName()
    {
        uint16_t deviceTypeHash = ~crc16_be((uint16_t)~0x0000, (const uint8_t *)s_deviceTypeName.c_str(), s_deviceTypeName.length());

        uint8_t eth_mac[6];
        auto err = esp_wifi_get_mac(WIFI_IF_STA, eth_mac);
        Error::CheckAppendName(err, TAG, "An error occured when getting ETH MAC");

        return Format::String("%04X-%02X%02X%02X", deviceTypeHash, eth_mac[3], eth_mac[4], eth_mac[5]);
    }

    void InitializeTimeSync()
    {
        ESP_LOGD(TAG, "Synchronizing time.\n");

        auto now = time(nullptr);
        ESP_LOGD(TAG, "Time before synchronization: %s", ctime(&now));

        if (sntp_enabled())
            return;

        ESP_LOGD(TAG, "Initializing SNTP.");

        sntp_setoperatingmode(SNTP_OPMODE_POLL);
        sntp_setservername(0, "pool.ntp.org");
        sntp_set_time_sync_notification_cb(TimeSyncNotificationCallback);
#ifdef CONFIG_SNTP_TIME_SYNC_METHOD_SMOOTH
        sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);
#endif
        sntp_init();

        for (int tries = 1; tries <= NTP_MAX_SYNC_TRIES; tries++)
        {
            if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED)
                break;

            ESP_LOGD(TAG, "Waiting for system time to be set... (%d/%d)",
                     tries,
                     NTP_MAX_SYNC_TRIES);

            vTaskDelay(Delay::MilliSeconds(NTP_MAX_WAIT_MS));
        }
    }

    void SetResetWirelessLED(gpio_num_t gpioNum)
    {
        s_wifiResetLED = gpioNum;
    }

    void ResetWireless()
    {
        // Blink LED if it is set.
        // When CONFIG_TWOMES_CUSTOM_GPIO is used, the default value will be GPIO_NUM_MAX.
        if (s_wifiResetLED != GPIO_NUM_MAX)
            BlinkLED(s_wifiResetLED, 5);

        auto err = NVS::Erase(NVS_NAMESPACE, "bearer");
        Error::CheckAppendName(err, TAG, "An error occured when erasing bearer");

        // Remove key so we do post provisioning steps.
        NVS::Erase(NVS_NAMESPACE, "ppDone");

        esp_wifi_restore();
        esp_restart();
    }

    void BlinkLED(gpio_num_t gpioNum, int amount)
    {
        auto level = gpio_get_level(gpioNum);

        for (int i = 0; i < amount * 2; i++)
        {
            // Flip level
            level ^= 1;

            gpio_set_level(gpioNum, level);
            vTaskDelay(200 / portTICK_PERIOD_MS);
        }
    }

    std::string GetServerHostname()
    {
        std::string host;
        if (NVS::Get(NVS_NAMESPACE, "server_host", host) == ESP_OK && !host.empty() && IsAllowedServer(host))
            return host;
        return TWOMES_SERVER_HOSTNAME;
    }

    int PostHTTPSToBackend(const std::string &endpoint,
                           HTTPUtil::buffer_t &dataSend,
                           HTTPUtil::headers_t &headersSend,
                           HTTPUtil::buffer_t &dataReceive,
                           HTTPUtil::headers_t &headersReceive,
                           bool useBearer = false)
    {
        std::string url = "https://" + GetServerHostname() + endpoint;

        esp_http_client_config_t config{};
        config.url = url.c_str();
        config.cert_pem = isrg_root_pem_start;
        config.method = HTTP_METHOD_POST;
        config.transport_type = HTTP_TRANSPORT_OVER_SSL;
        config.is_async = false;

        headersSend["accept"] = "*/*";
        headersSend["Content-Type"] = "application/json";
        if (useBearer)
            headersSend["Authorization"] = "Bearer " + GetBearer();

        ESP_LOGD(TAG, "Sending data to backend with length: %d, data:\n%s", dataSend.size(), dataSend.c_str());

        ESP_LOGD(TAG, "Remaining heap space: %d", esp_get_free_heap_size());

        return HTTPUtil::HTTPRequest(config, headersReceive, dataReceive, headersSend, dataSend);
    }
} // namespace GenericESP32Firmware
