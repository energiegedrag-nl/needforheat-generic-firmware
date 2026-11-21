#pragma once

#include <functional>
#include <string>
#include <vector>

/**
 * The device menu on the dial (wheel) of the M5Stack CoreInk, after provisioning.
 *
 * Press the dial on the overview: a menu with Smartphones (with presence detection), Status, Batterij, Nu meten,
 * Scherm verversen, Info, items of the device firmware, Wifi opnieuw, Herstarten and terug. Up and down move, a
 * press chooses; Wifi opnieuw and Herstarten ask for confirmation. Two minutes without the dial: back to the overview.
 *
 * Without a working screen: hold the dial pressed for 10 s to restart (a full screen refresh follows); the button on
 * top held for 10 s still resets Wi-Fi.
 *
 * The menu owns the three dial buttons (GPIO 37, 38, 39); the presence detection control panel is one of its
 * submenus (ControlPanel). On other boards these functions do nothing.
 */
namespace DeviceMenu
{
    /** Register the dial and start the menu (once, after provisioning). */
    void Start();

    /**
     * An item of the device firmware, shown before "Wifi opnieuw" (call before or after Start()). The action runs in
     * the menu task; when it returns, the overview is shown again.
     */
    void AddItem(const std::string &label, std::function<void()> action);

    /** An extra line on the Status page, e.g. the CO₂ calibration of the living room module (short: about 20 chars). */
    void AddStatusLine(std::function<std::string()> line);

    /**
     * A submenu that handles the dial itself while it is open (used by the presence detection control panel for
     * "Smartphones"). enter() opens it; up/press/down get the dial; when it is done it calls SubmenuDone().
     */
    struct Submenu
    {
        std::string label;
        std::function<void()> enter;
        std::function<void()> up;
        std::function<void()> press;
        std::function<void()> down;
        /** The menu closes while the submenu is open (time-out or "Scherm verversen"): reset its state. */
        std::function<void()> close;
    };
    void SetSubmenu(const Submenu &submenu);

    /** The open submenu is done ("terug" at its top): show the main menu again. */
    void SubmenuDone();

    /** Close the menu (also from a submenu's own time-out): show the overview. */
    void Close();

    /** True while the menu (or one of its submenus) is open. */
    bool IsOpen();
} // namespace DeviceMenu
