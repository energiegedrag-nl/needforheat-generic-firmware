#pragma once

#include <string>
#include <vector>
#include <util/local_time.hpp>

#include <driver/gpio.h>

#include <util/http_util.hpp>
#ifdef M5STACK_COREINK
#include <util/screen.hpp>
#endif

// API path prefix of the NeedForHeat server. Default "/v3", the current server API.
// Servers that still publish the device endpoints under /v2 can be reached with:
//   build_flags = '-D CONFIG_TWOMES_API_PREFIX="/v2"'
#ifndef CONFIG_TWOMES_API_PREFIX
#define CONFIG_TWOMES_API_PREFIX "/v3"
#endif
constexpr const char *ENDPOINT_VARIABLE_UPLOAD = CONFIG_TWOMES_API_PREFIX "/upload";
constexpr const char *ENDPOINT_FIXED_INTERVAL_UPLOAD = CONFIG_TWOMES_API_PREFIX "/device/measurements/fixed-interval"; // Deprecated.
constexpr const char *ENDPOINT_DEVICE_ACTIVATION = CONFIG_TWOMES_API_PREFIX "/device/activate";

namespace GenericESP32Firmware
{
    /**
     * Initialize generic ESP32 firmware.
     *
     * The firmware enables provisioning with the 'warmtewachter' app
     * and sending measurement data to the twomes backend API.
     *
     * @param deviceTypeName Name of a valid device type.
     */
    void Initialize(const std::string &deviceTypeName);

    /**
     * Short name of the device for people, e.g. "Woonkamer" or "P1-lezer": at most about 9 characters, shown in the
     * header of the provisioning screen next to the firmware version (a box can hold several CoreInks, each for its
     * own sensor or BASE). The same short name as in needforheat-manuals (devices/<type>/short_names.json). Dutch, the
     * default language of campaigns: before activation the device does not know its campaign. Call before Initialize.
     */
    void SetShortName(const std::string &shortName);

    /** Deprecated: use SetShortName. */
    inline void SetDisplayName(const std::string &displayName) { SetShortName(displayName); }

    /**
     * Get the device's service name; prefixe and last 3 octets of Wi-Fi MAC-address.
     *
     * @returns The device's service name.
     */
    std::string GetDeviceServiceName();

    /**
     * Initialize time synchronization via NTP and sync with internal clock.
     *
     * This will sync every hour by default, controlled by CONFIG_LWIP_SNTP_UPDATE_DELAY.
     */
    void InitializeTimeSync();

    /**
     * Configure the LED to blink when ResetWireless() is called.
     * 
     * This function only needs to be called when using CONFIG_TWOMES_CUSTOM_GPIO.
     * The GPIO will NOT be configured for you.
     * 
     * @param gpioNum LED GPIO.
     */
    void SetResetWirelessLED(gpio_num_t gpioNum);

    /**
     * Reset wireless settings and delete the bearer to force re-activation.
     */
    void ResetWireless();

    /**
     * Blink an LED on the device.
     *
     * @param gpioNum GPIO number where LED is connected.
     * @param amount Amount of times to flash the LED.
     */
    void BlinkLED(gpio_num_t gpioNum, int amount);

    /**
     * Host name of the server this device uploads to: the one chosen during provisioning (if allowed),
     * else the one built into the firmware.
     */
    std::string GetServerHostname();

    /**
     * Local time offset for what the screen shows, as the server sent it with the last upload. Stored in NVS only
     * when it changed. Until the first one arrives, the built-in Dutch rule applies (LocalTime).
     */
    void SetDisplayOffset(const LocalTime::ServerOffset &offset);

#ifdef M5STACK_COREINK
    /**
     * Show the overview screen on the e-ink display (see Screen::Status), e.g. after each measurement.
     * Fills in what the generic firmware knows: Wi-Fi state, the date, a TEST badge for a test server and,
     * when footerLeft is empty, the time of the last upload or the number of measurements not sent yet.
     */
    void DisplayStatus(Screen::Status status);

    /** A message screen (title, wrapped paragraphs, footer), e.g. for progress of a device action. */
    void DisplayMessage(const std::string &title, const std::vector<std::string> &paragraphs, const std::string &footer);

    /**
     * A deliberate action that is only possible **before provisioning**, e.g. calibrating a sensor outdoors.
     * On the provisioning screen: hold the dial pressed for 5 s (GPIO 38), read the question, and turn the dial down
     * (GPIO 39) within 30 s to confirm. A hint to unplug on (apparent) USB power (the action is meant for battery power, e.g. in a box
     * outdoors, not at a computer). During the action the provisioning power-off timeout is paused; afterwards the
     * provisioning screen returns. run() is called in its own task and may take long (it shows its own progress with
     * DisplayMessage). Not shown on the screen: participants receive provisioned devices and cannot reach it.
     * Call before Initialize.
     */
    void SetPreProvisioningAction(const std::string &title, const std::string &question, void (*run)());

    /** Draw the last overview again with fresh upload state (called after each upload); nothing before the first. */
    void RefreshStatus();

    /** Lines of the Status page of the device menu (DeviceMenu): name, firmware, server, Wi-Fi, uploads. */
    std::vector<std::string> MenuStatusLines();

    /** Lines of the Batterij page of the device menu: voltage, charge, power supply, hours left. */
    std::vector<std::string> MenuBatteryLines();

    /** The info QR code (link to the manual) on the screen, for "Info" in the device menu. */
    void ShowInfoQR();
#endif

    /**
     * Send an HTTPS POST request to the backend API.
     *
     * @param endpoint API endpoint.
     * @param dataSend Data to send in the POST request.
     * @param headersSend Headers to send in the POST request.
     * @param dataReceive Response data from the POST request.
     * @param headersReceive Response headers from the POST request.
     * @param useBearer Optional parameter (default true) to use bearer or not.
     *
     * @returns HTTP status code.
     */
    int PostHTTPSToBackend(const std::string &endpoint,
                           HTTPUtil::buffer_t &dataSend,
                           HTTPUtil::headers_t &headersSend,
                           HTTPUtil::buffer_t &dataReceive,
                           HTTPUtil::headers_t &headersReceive,
                           bool useBearer);
} // namespace GenericESP32Firmware
