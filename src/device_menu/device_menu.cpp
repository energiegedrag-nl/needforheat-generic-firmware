#include <device_menu.hpp>

#include <atomic>

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#include <generic_esp_32.hpp>
#include <scheduler.hpp>
#include <util/buttons.hpp>
#include <util/buzzer.hpp>
#include <util/delay.hpp>
#include <util/screen.hpp>
#include <util/timer.hpp>

#ifdef M5STACK_COREINK
namespace DeviceMenu
{
    namespace
    {
        constexpr const char *TAG = "DeviceMenu";

        constexpr EventBits_t EV_UP = 1 << 0;
        constexpr EventBits_t EV_PRESS = 1 << 1;
        constexpr EventBits_t EV_DOWN = 1 << 2;
        constexpr EventBits_t EV_SUBMENU_DONE = 1 << 3;
        constexpr EventBits_t EV_CLOSE = 1 << 4;
        constexpr EventBits_t EV_RESTART = 1 << 5;
        constexpr EventBits_t EV_ALL = EV_UP | EV_PRESS | EV_DOWN | EV_SUBMENU_DONE | EV_CLOSE | EV_RESTART;

        /** Hold the dial pressed this long (s) to restart, also when the screen does not work. */
        constexpr int RESTART_HOLD_S = 10;

        enum class State
        {
            Idle,
            Main,
            Page,
            Confirm,
            Submenu,
            Qr
        };

        struct Entry
        {
            std::string label;
            std::function<void()> action;
        };

        EventGroupHandle_t s_events = nullptr;
        State s_state = State::Idle;
        std::atomic<bool> s_open{false};
        bool s_started = false;
        int s_highlight = 1;

        std::vector<Entry> s_entries; // the main menu as shown
        std::vector<Entry> s_appItems;
        std::vector<std::function<std::string()>> s_statusLines;
        Submenu s_submenu;
        bool s_hasSubmenu = false;

        std::string s_pageTitle;
        std::vector<std::string> s_page;
        std::string s_confirmTitle;
        std::string s_confirmYes;
        std::function<void()> s_confirmAction;

        void SetBits(EventBits_t bits)
        {
            if (s_events != nullptr)
                xEventGroupSetBits(s_events, bits);
        }

        void OnTimeout() { SetBits(EV_CLOSE); }
        Timer::Timer s_timer("DeviceMenu", OnTimeout, Timer::Timeout::MINUTE * 2);

        // Button callbacks run in the button task: only signal the menu task.
        void Up() { SetBits(EV_UP); }
        void Press() { SetBits(EV_PRESS); }
        void Down() { SetBits(EV_DOWN); }
        void HoldToRestart() { SetBits(EV_RESTART); }

        Screen &Display() { return Screen::Instance(); }

        /** Back to the overview; with a full refresh of the screen when asked (against ghosting). */
        void Leave(bool fullRefresh)
        {
            if (s_state == State::Submenu && s_submenu.close)
                s_submenu.close();
            s_state = State::Idle;
            s_open = false;
            s_timer.Stop();
            Display().CloseMenu();
            if (fullRefresh)
                Display().Clear();
            GenericESP32Firmware::RefreshStatus();
        }

        void DrawMain()
        {
            std::vector<std::string> lines{"Menu"};
            for (const auto &entry : s_entries)
                lines.push_back(entry.label);
            Display().DrawMenu(lines, s_highlight);
        }

        void ShowMain()
        {
            s_state = State::Main;
            DrawMain();
        }

        void DrawPage()
        {
            std::vector<std::string> lines{s_pageTitle};
            lines.insert(lines.end(), s_page.begin(), s_page.end());
            lines.push_back("terug");
            Display().DrawMenu(lines, s_highlight); // 0: nothing highlighted; up and down scroll
        }

        void OpenPage(const std::string &title, std::vector<std::string> lines)
        {
            s_state = State::Page;
            s_pageTitle = title;
            s_page = std::move(lines);
            s_highlight = 0;
            DrawPage();
        }

        void DrawConfirm()
        {
            Display().DrawMenu({s_confirmTitle, "Nee", s_confirmYes}, s_highlight);
        }

        void Confirm(const std::string &title, const std::string &yes, std::function<void()> action)
        {
            s_state = State::Confirm;
            s_confirmTitle = title;
            s_confirmYes = yes;
            s_confirmAction = std::move(action);
            s_highlight = 1; // "Nee" first: a slip of the dial does nothing
            DrawConfirm();
        }

        std::vector<std::string> StatusPage()
        {
            auto lines = GenericESP32Firmware::MenuStatusLines();
            for (const auto &line : s_statusLines)
            {
                auto text = line();
                if (!text.empty())
                    lines.push_back(text);
            }
            return lines;
        }

        void RunNow()
        {
            Display().CloseMenu();
            Display().DisplayMessage("Nu meten", {"De module meet en verstuurt nu.", "Zo meteen verschijnt het overzicht."}, "");
            GenericESP32Firmware::RequestWifiAttempt();
            Scheduler::RequestRunNow();
            vTaskDelay(Delay::Seconds(3));
            s_state = State::Idle;
            s_open = false;
            s_timer.Stop();
            GenericESP32Firmware::RefreshStatus();
        }

        void ResetWifi()
        {
            Display().CloseMenu();
            Display().DisplayMessage("Wifi opnieuw",
                                     {"De module start opnieuw en toont de QR-code.",
                                      "Scan die met de app. De metingen blijven bewaard."},
                                     "");
            Buzzer::Buzz(200);
            vTaskDelay(Delay::Seconds(5));
            GenericESP32Firmware::ResetWireless(); // restarts
        }

        void Restart()
        {
            Display().CloseMenu();
            Display().DisplayMessage("Herstarten", {"De module start opnieuw."}, "");
            vTaskDelay(Delay::Seconds(2));
            Display().Clear();
            esp_restart();
        }

        void BuildMain()
        {
            s_entries.clear();
            if (s_hasSubmenu)
                s_entries.push_back({s_submenu.label, []
                                     {
                                         s_state = State::Submenu;
                                         if (s_submenu.enter)
                                             s_submenu.enter();
                                     }});
            s_entries.push_back({"Status", []
                                 { OpenPage("Status", StatusPage()); }});
            s_entries.push_back({"Batterij", []
                                 { OpenPage("Batterij", GenericESP32Firmware::MenuBatteryLines()); }});
            s_entries.push_back({"Nu meten", RunNow});
            s_entries.push_back({"Scherm verversen", []
                                 { Leave(true); }});
            s_entries.push_back({"Info", []
                                 {
                                     s_state = State::Qr;
                                     Display().CloseMenu();
                                     GenericESP32Firmware::ShowInfoQR();
                                 }});
            for (const auto &item : s_appItems)
            {
                auto action = item.action;
                s_entries.push_back({item.label, [action]
                                     {
                                         s_state = State::Idle;
                                         s_open = false;
                                         s_timer.Stop();
                                         Display().CloseMenu();
                                         if (action)
                                             action();
                                         GenericESP32Firmware::RefreshStatus();
                                     }});
            }
            s_entries.push_back({"Wifi opnieuw", []
                                 { Confirm("Wifi opnieuw?", "Ja, wifi vergeten", ResetWifi); }});
            s_entries.push_back({"Herstarten", []
                                 { Confirm("Herstarten?", "Ja, herstarten", Restart); }});
            s_entries.push_back({"terug", []
                                 { Leave(false); }});
        }

        int Wrap(int value, int first, int last)
        {
            if (value < first)
                return last;
            if (value > last)
                return first;
            return value;
        }

        void Handle(EventBits_t button)
        {
            s_timer.StartOrReset();
            switch (s_state)
            {
            case State::Idle:
                // Any turn or press on the overview opens the menu.
                s_open = true;
                BuildMain();
                s_highlight = 1;
                ShowMain();
                break;
            case State::Main:
            {
                const int n = static_cast<int>(s_entries.size());
                if (button == EV_UP || button == EV_DOWN)
                {
                    s_highlight = Wrap(s_highlight + (button == EV_DOWN ? 1 : -1), 1, n);
                    DrawMain();
                }
                else if (s_highlight >= 1 && s_highlight <= n)
                    s_entries[s_highlight - 1].action();
                break;
            }
            case State::Page:
            {
                const int last = static_cast<int>(s_page.size()) + 1; // "terug"
                if (button == EV_UP || button == EV_DOWN)
                {
                    s_highlight = Wrap(s_highlight + (button == EV_DOWN ? 1 : -1), 0, last);
                    DrawPage();
                }
                else
                {
                    s_highlight = 1;
                    ShowMain();
                }
                break;
            }
            case State::Confirm:
                if (button == EV_UP || button == EV_DOWN)
                {
                    s_highlight = s_highlight == 1 ? 2 : 1;
                    DrawConfirm();
                }
                else if (s_highlight == 2 && s_confirmAction)
                    s_confirmAction();
                else
                {
                    s_highlight = 1;
                    ShowMain();
                }
                break;
            case State::Qr:
                ShowMain();
                break;
            case State::Submenu:
                if (button == EV_UP && s_submenu.up)
                    s_submenu.up();
                else if (button == EV_DOWN && s_submenu.down)
                    s_submenu.down();
                else if (button == EV_PRESS && s_submenu.press)
                    s_submenu.press();
                break;
            }
        }

        void MenuTask(void *)
        {
            while (true)
            {
                auto bits = xEventGroupWaitBits(s_events, EV_ALL, pdTRUE, pdFALSE, portMAX_DELAY);
                if (bits & EV_RESTART)
                {
                    // Works without the screen: two beeps, then restart; the overview is drawn in full after boot.
                    ESP_LOGI(TAG, "Dial held for %d s: restarting.", RESTART_HOLD_S);
                    Buzzer::Buzz(150, 2);
                    esp_restart();
                }
                if (bits & EV_CLOSE)
                {
                    if (s_state != State::Idle)
                        Leave(false);
                    continue;
                }
                if (bits & EV_SUBMENU_DONE)
                {
                    if (s_state == State::Submenu)
                    {
                        s_timer.StartOrReset();
                        ShowMain();
                    }
                    continue;
                }
                for (EventBits_t button : {EV_UP, EV_PRESS, EV_DOWN})
                    if (bits & button)
                        Handle(button);
            }
        }
    } // namespace

    void Start()
    {
        if (s_started)
            return;
        s_started = true;
        s_events = xEventGroupCreate();
        for (auto pin : {GPIO_NUM_37, GPIO_NUM_38, GPIO_NUM_39})
        {
            gpio_config_t config = {};
            config.intr_type = GPIO_INTR_NEGEDGE;
            config.mode = GPIO_MODE_INPUT;
            config.pin_bit_mask = 1ULL << pin;
            config.pull_up_en = GPIO_PULLUP_ENABLE;
            gpio_config(&config);
        }
        Buttons::ButtonPressHandler::AddButton(GPIO_NUM_37, "Menu up", 0, Up, nullptr);
        Buttons::ButtonPressHandler::AddButton(GPIO_NUM_38, "Menu press", 0, Press, HoldToRestart, RESTART_HOLD_S);
        Buttons::ButtonPressHandler::AddButton(GPIO_NUM_39, "Menu down", 0, Down, nullptr);
        if (xTaskCreatePinnedToCore(MenuTask, "DeviceMenu", 6144, nullptr, 1, nullptr, APP_CPU_NUM) != pdPASS)
            ESP_LOGE(TAG, "Could not start the menu task");
    }

    void AddItem(const std::string &label, std::function<void()> action)
    {
        s_appItems.push_back({label, std::move(action)});
    }

    void AddStatusLine(std::function<std::string()> line)
    {
        s_statusLines.push_back(std::move(line));
    }

    void SetSubmenu(const Submenu &submenu)
    {
        s_submenu = submenu;
        s_hasSubmenu = true;
    }

    void SubmenuDone()
    {
        SetBits(EV_SUBMENU_DONE);
    }

    void Close()
    {
        SetBits(EV_CLOSE);
    }

    bool IsOpen()
    {
        return s_open;
    }
} // namespace DeviceMenu
#else  // not M5STACK_COREINK: no dial, no menu
namespace DeviceMenu
{
    void Start() {}
    void AddItem(const std::string &, std::function<void()>) {}
    void AddStatusLine(std::function<std::string()>) {}
    void SetSubmenu(const Submenu &) {}
    void SubmenuDone() {}
    void Close() {}
    bool IsOpen() { return false; }
} // namespace DeviceMenu
#endif // M5STACK_COREINK
