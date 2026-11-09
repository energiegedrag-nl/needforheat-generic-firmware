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
	/** One line of a progress screen (e.g. provisioning). */
	enum class StepState
	{
		Todo,
		Active,
		Done,
		Failed
	};
	struct Step
	{
		StepState state;
		std::string label;
	};

	/** Overview screen of a device: one big value, up to three tiles, status in header and footer. */
	struct Status
	{
		std::string clock;										// e.g. "14:20"; empty: not shown
		bool wifiConnected = true;
		std::string badge;										// e.g. "TEST" for a test server; empty: none
		std::string bigLabel;									// e.g. "CO2 ppm"
		std::string bigValue;									// e.g. "612"
		std::vector<std::pair<std::string, std::string>> tiles; // label, value (at most 3)
		std::string footerLeft;									// e.g. "sent 14:10, 0 waiting"
		std::string footerRight;
	};

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

	/** Wait until the panel has finished refreshing and put it to sleep; call before cutting power. */
	void PrepareForPowerOff();

private:
	static constexpr int FAST_REFRESH_LIMIT = 10;
	static constexpr int MENU_IDLE_S = 120;

	enum class Refresh
	{
		Full,
		Fast
	};

	void Begin();
	void Header(const std::string &left, const std::string &right, bool rightInverted = false);
	void Footer(const std::string &left, const std::string &right);
	void Push(const std::string &screenKey);
	std::string Fit(const std::string &text, int maxWidth);
	int WrapText(const std::string &text, int x, int y, int maxWidth, int lineHeight);
	void DrawQR(const std::string &payload, int x, int y, int maxSize);

	// Storage for Info QR.
	static std::string s_infoURL;
	static std::string s_infoText;
	static int s_infoPadding;

	LGFX e_ink_display;
	LGFX_Sprite m_canvas;
	int m_fastRefreshes = 0;
	TickType_t m_lastMenuTick = 0;
	std::string m_screenKey; // identifies the layout on screen, to choose between a full and a fast refresh
	std::recursive_mutex m_mutex;
};
