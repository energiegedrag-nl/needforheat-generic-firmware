#pragma once

#include <string>
#include <utility>
#include <vector>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

/**
 * Drawing of the screens of the 200x200 1-bit e-ink panel (M5Stack CoreInk) on an off-screen sprite.
 *
 * No panel and no FreeRTOS here, so the same code runs on the device and in the host renderer
 * (test/host-render), which writes every screen as an image to check the layout before flashing.
 *
 * Readability rules for the 1-bit panel (no grey, no anti-aliasing): bold fonts only, labels and status text at
 * least a 9 pt bold font (cap height about 13 px), measured values larger. Text that does not fit is shortened
 * with ".."; fit the content with fewer words rather than with smaller fonts.
 */
class ScreenLayout
{
public:
	static constexpr int W = 200;
	static constexpr int H = 200;

	/** Palette indices of the 1-bit sprite. */
	static constexpr uint32_t BLACK = 0;
	static constexpr uint32_t WHITE = 1;

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
		std::string date;										// e.g. "wo 18 nov" (not a clock: the screen does not refresh every minute); empty: not shown
		bool wifiConnected = true;
		std::string badge;										// e.g. "TEST" for a test server; empty: none
		std::string device;										// not shown on the overview (since 18-11); kept for callers
		int battery = -1;										// header icon: 0..4 bars, 5 USB power; -1 none
		std::string bigLabel;									// e.g. "CO₂" (subscript digits allowed)
		std::string bigValue;									// e.g. "612"
		std::string bigUnit;									// e.g. "ppm": smaller, right after the value
		std::vector<std::pair<std::string, std::string>> tiles; // label or "icon:<name>" (e.g. icon:temperature), value (at most 3)
		std::string footerLeft;									// e.g. "verstuurd 14:10"
		std::string footerRight;								// e.g. "info"
	};

	/** The fonts of one look; the device uses Fonts(), the host renderer compares all sets. */
	struct FontSet
	{
		const char *name;
		const lgfx::IFont *text;	  // bars, labels, menus, messages (smallest text on screen)
		const lgfx::IFont *title;	  // titles, the text below a QR code
		const lgfx::IFont *value;	  // tile values
		const lgfx::IFont *bigText;	  // main value when it is not a number
		const lgfx::IFont *bigNumber; // main value when it is a number (digits . , : - +)
		const lgfx::IFont *bigNumber2; // the same, narrower: when the value and unit do not fit with bigNumber
	};

	/** The font sets that can be compared; the first is the one in use (see Fonts()). */
	static const std::vector<FontSet> &FontSets();

	/** The font set in use on the device. */
	static const FontSet &Fonts();

	explicit ScreenLayout(LGFX_Sprite &canvas, const FontSet &fonts = Fonts());

	// Each Draw function draws a whole screen and returns its layout key: the same key as the screen that is
	// on the panel means only values changed, so a fast refresh is enough.

	std::string DrawBlank();
	std::string DrawQR(const std::string &payload, int padding, const std::string &text);
	std::string DrawProvisioningQR(const std::string &payload, const std::string &deviceName,
								   const std::string &displayName, const std::string &version);
	std::string DrawSteps(const std::string &title, const std::string &titleRight, const std::vector<Step> &steps,
						  const std::string &message, const std::string &footer);
	std::string DrawMessage(const std::string &title, const std::vector<std::string> &paragraphs,
							const std::string &footer);
	std::string DrawStatus(const Status &status);
	std::string DrawMenu(const std::vector<std::string> &lines, int highlightedLine);

private:
	void Begin();
	void Header(const std::string &left, const std::string &right, int leftIndent = 0, const std::string &badge = "",
				int battery = -1);
	/** Battery icon (white on the black header), 0..4 bars or 5 for USB power; returns its width. */
	int BatteryIcon(int x, int battery);
	/** Footer bar; returns the y of its top line (H when there is no footer). */
	int Footer(const std::string &left, const std::string &right);
	void WifiSymbol(int x, int y, bool connected, uint32_t colour);
	std::string Fit(const std::string &text, int maxWidth);
	/**
	 * Draws a label left-aligned on a baseline with subscript digits (U+2080..U+2089, e.g. "CO₂") in a smaller font
	 * below the baseline; shortened like Fit() when it is too wide. Uses the current font for the other text.
	 */
	void DrawLabel(const std::string &text, int x, int y, int maxWidth);
	/** A label, or an icon when the text is "icon:<name>" (include/util/icons.hpp); top is used for the icon. */
	void DrawLabelOrIcon(const std::string &text, int x, int baseline, int top, int maxWidth);
	int LabelWidth(const std::string &text);
	int WrapText(const std::string &text, int x, int y, int maxWidth, int lineHeight, int maxY);

	LGFX_Sprite &m_canvas;
	const FontSet &m_fonts;
};
