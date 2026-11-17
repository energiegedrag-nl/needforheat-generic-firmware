#pragma once

#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include <util/strings.hpp>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define LGFX_USE_V1
#include <LovyanGFX.h>
#include <LGFX_AUTODETECT.hpp>

#include <util/screen_layout.hpp>

/**
 * E-ink screen of the M5Stack CoreInk (200x200, black and white).
 *
 * Every screen is drawn in a 1-bit off-screen buffer (5 KB) and sent to the panel in one go: no half-drawn
 * screens or shifted text. A different screen gets a full refresh (no ghosting); small changes, such as the
 * menu cursor or new measurement values, get a fast partial refresh, with a full refresh after every
 * FAST_REFRESH_LIMIT fast ones. Layout: header bar, content, footer bar (design: "NeedForHeat e-ink GUI").
 */
class Screen
{
public:
	// The screens themselves are drawn by ScreenLayout (also used by the host renderer in test/host-render).
	using StepState = ScreenLayout::StepState;
	using Step = ScreenLayout::Step;
	using Status = ScreenLayout::Status;

	/** The one screen object of the device (one panel, one buffer, one refresh state); created on first use. */
	static Screen &Instance();

	Screen();
	Screen(const Screen &) = delete;
	Screen &operator=(const Screen &) = delete;

	/** Clear the screen (full refresh). */
	void Clear();

	/**
	 * Show a QR code with an optional text below it (e.g. the info QR).
	 *
	 * @param payload QR code payload.
	 * @param padding Minimum amount of white pixels around the QR code.
	 * @param text Optional text to show under the QR code.
	 */
	void DisplayQR(const std::string &payload, int padding, const std::string &text = "");

	/** Provisioning QR: header, QR code, device name below it, footer (e.g. device type, version). */
	void DisplayProvisioningQR(const std::string &payload, const std::string &deviceName,
							   const std::string &footerLeft, const std::string &footerRight);

	/** Progress screen: a list of steps, a message below them and a footer. */
	void DisplaySteps(const std::string &title, const std::string &titleRight, const std::vector<Step> &steps,
					  const std::string &message, const std::string &footer);

	/** Message screen, e.g. an error with the next step. Paragraphs are wrapped. */
	void DisplayMessage(const std::string &title, const std::vector<std::string> &paragraphs,
						const std::string &footer);

	/**
	 * Overview screen. Repeated calls with the same layout use a fast refresh.
	 * Skipped while a menu is in use (drawn less than MENU_IDLE_S seconds ago), so a measurement does not
	 * replace the menu under someone's fingers.
	 */
	void DisplayStatus(const Status &status);

	void SetInfoQRDetails(const std::string &payload, int padding, const std::string &text = "");

	/** Show the info QR; does nothing if no details were set. */
	void DisplayInfoQR();

	/**
	 * Display a menu. Moving the highlight within the same menu uses a fast refresh.
	 *
	 * @param lines first item: title, last item: back.
	 * @param highlightedLine index of the highlighted item.
	 */
	void DrawMenu(std::vector<std::string> lines, int highlightedLine);

	/**
	 * Wait until the panel has finished refreshing and put it to sleep; call before cutting power, so the image
	 * stays sharp. If the power stays on (e.g. on USB), the next screen wakes the panel up again.
	 */
	void PrepareForPowerOff();

private:
	static constexpr int FAST_REFRESH_LIMIT = 10;
	static constexpr int MENU_IDLE_S = 120;

	enum class Refresh
	{
		Full,
		Fast
	};

	void Push(const std::string &screenKey);

	// Storage for Info QR.
	static std::string s_infoURL;
	static std::string s_infoText;
	static int s_infoPadding;

	LGFX e_ink_display;
	LGFX_Sprite m_canvas;
	bool m_asleep = false; // panel put to sleep by PrepareForPowerOff(); woken up by the next Push()
	int m_fastRefreshes = 0;
	TickType_t m_lastMenuTick = 0;
	std::string m_screenKey; // identifies the layout on screen, to choose between a full and a fast refresh
	std::recursive_mutex m_mutex;
};
