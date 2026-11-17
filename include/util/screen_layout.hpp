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
		std::string clock;										// e.g. "14:20"; empty: not shown
		bool wifiConnected = true;
		std::string badge;										// e.g. "TEST" for a test server; empty: none
		std::string device;										// short device label in the header, e.g. "P1"
		std::string bigLabel;									// e.g. "CO2 ppm"
		std::string bigValue;									// e.g. "612"
		std::vector<std::pair<std::string, std::string>> tiles; // label, value (at most 3)
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
								   const std::string &footerLeft, const std::string &footerRight);
	std::string DrawSteps(const std::string &title, const std::string &titleRight, const std::vector<Step> &steps,
						  const std::string &message, const std::string &footer);
	std::string DrawMessage(const std::string &title, const std::vector<std::string> &paragraphs,
							const std::string &footer);
	std::string DrawStatus(const Status &status);
	std::string DrawMenu(const std::vector<std::string> &lines, int highlightedLine);

private:
	void Begin();
	void Header(const std::string &left, const std::string &right, int leftIndent = 0, const std::string &badge = "");
	/** Footer bar; returns the y of its top line (H when there is no footer). */
	int Footer(const std::string &left, const std::string &right);
	void WifiSymbol(int x, int y, bool connected, uint32_t colour);
	std::string Fit(const std::string &text, int maxWidth);
	int WrapText(const std::string &text, int x, int y, int maxWidth, int lineHeight, int maxY);

	LGFX_Sprite &m_canvas;
	const FontSet &m_fonts;
};
