#include <util/screen_layout.hpp>

#include <algorithm>
#include <cstring>

#include <util/fonts/u8g2_fonts.hpp>
#include <util/icons.hpp>

/*
 * Layout on the 200x200 panel; y values of text are baselines.
 *   header bar   black, y 0..21, white text on baseline 16
 *   content      y 22..174
 *   footer       2-px line at y 175, text on baseline 194
 * Status screen: label baseline 39, main value baseline 104, 2-px line at y 116, tile labels baseline 136,
 * tile values baseline 168.
 * Colours: the sprite is 1-bit with a palette, so colours are palette indices (BLACK, WHITE).
 */

namespace
{
	constexpr int MARGIN = 5;
	constexpr int HEADER_H = 22;
	constexpr int HEADER_BASE = 16;
	constexpr int FOOTER_LINE = 175;
	constexpr int FOOTER_BASE = 194;
	constexpr int TEXT_CAP = 14;	 // cap height of the text font
	constexpr int LINE_HEIGHT = 18;	 // line distance of wrapped text
	constexpr int ROW_H = 26;		 // rows of steps and menus

	constexpr int STATUS_LABEL_BASE = 39;
	constexpr int STATUS_BIG_BASE = 104;
	constexpr int STATUS_TILES_LINE = 116;
	constexpr int STATUS_TILE_LABEL_BASE = 136;
	constexpr int STATUS_TILE_VALUE_BASE = 168;

	constexpr int PROVISIONING_QR_SIZE = 100;  // version 2 (25 modules) at 4 px per module
	constexpr int PROVISIONING_QR_QUIET = 16;  // 4 modules of white between the header bar and the QR code
	constexpr int PROVISIONING_NAME_BASE = 167;

	const lgfx::U8g2font HELV_B14(u8g2_font_helvB14_tf);
	const lgfx::U8g2font HELV_B18(u8g2_font_helvB18_tf);
	const lgfx::U8g2font HELV_B24(u8g2_font_helvB24_tf);
	const lgfx::U8g2font FUB49(u8g2_font_fub49_tn);
	const lgfx::U8g2font FUB42(u8g2_font_fub42_tn);
	// Subscript digits (for "CO₂") as own 6 x 9 bitmaps, drawn pixel by pixel in the text colour without a background.
	// The U8g2 digits font drew a filled box white on black on the device (LovyanGFX 0.x), though not on the host.
	constexpr int SUB_W = 6, SUB_H = 9;
	const uint8_t SUB_DIGITS[10][SUB_H] = {
		{0x78, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0x78}, // 0
		{0x30, 0x70, 0xf0, 0x30, 0x30, 0x30, 0x30, 0x30, 0xfc}, // 1
		{0x78, 0xcc, 0x0c, 0x0c, 0x18, 0x30, 0x60, 0xc0, 0xfc}, // 2
		{0x78, 0xcc, 0x0c, 0x38, 0x0c, 0x0c, 0x0c, 0xcc, 0x78}, // 3
		{0x18, 0x38, 0x78, 0xd8, 0xd8, 0xfc, 0x18, 0x18, 0x18}, // 4
		{0xfc, 0xc0, 0xc0, 0xf8, 0x0c, 0x0c, 0x0c, 0xcc, 0x78}, // 5
		{0x78, 0xcc, 0xc0, 0xf8, 0xcc, 0xcc, 0xcc, 0xcc, 0x78}, // 6
		{0xfc, 0x0c, 0x18, 0x18, 0x30, 0x30, 0x60, 0x60, 0x60}, // 7
		{0x78, 0xcc, 0xcc, 0x78, 0xcc, 0xcc, 0xcc, 0xcc, 0x78}, // 8
		{0x78, 0xcc, 0xcc, 0xcc, 0x7c, 0x0c, 0x0c, 0xcc, 0x78}, // 9
	};
	constexpr int SUBSCRIPT_DROP = 4;					  // baseline of a subscript below the text baseline
#ifdef SCREEN_LAYOUT_FONT_COMPARISON
	const lgfx::U8g2font NCEN_B14(u8g2_font_ncenB14_tf);
	const lgfx::U8g2font NCEN_B18(u8g2_font_ncenB18_tf);
	const lgfx::U8g2font NCEN_B24(u8g2_font_ncenB24_tf);
#endif

	/** Length of the UTF-8 subscript digit (U+2080..U+2089: E2 82 80..89) at pos, or 0. */
	size_t SubscriptAt(const std::string &text, size_t pos)
	{
		return pos + 2 < text.size() && static_cast<unsigned char>(text[pos]) == 0xE2 &&
					   static_cast<unsigned char>(text[pos + 1]) == 0x82 &&
					   static_cast<unsigned char>(text[pos + 2]) >= 0x80 && static_cast<unsigned char>(text[pos + 2]) <= 0x89
				   ? 3
				   : 0;
	}

	/** The text with subscript digits as normal digits. */
	std::string WithoutSubscripts(const std::string &text)
	{
		std::string plain;
		for (size_t i = 0; i < text.size(); i++)
		{
			if (SubscriptAt(text, i) > 0)
			{
				plain += static_cast<char>('0' + (static_cast<unsigned char>(text[i + 2]) - 0x80));
				i += 2;
			}
			else
			{
				plain += text[i];
			}
		}
		return plain;
	}

	bool IsNumber(const std::string &text)
	{
		return !text.empty() && std::all_of(text.begin(), text.end(), [](char c)
											{ return (c >= '0' && c <= '9') || c == '.' || c == ',' || c == ':' ||
													 c == '-' || c == '+' || c == ' '; });
	}
} // namespace

const std::vector<ScreenLayout::FontSet> &ScreenLayout::FontSets()
{
	// U8g2 fonts are drawn for pixels (clean at 1 bit per pixel) and have Latin-1 (° ² ³). fub49 has only digits and
	// . , : - +, so a main value that is not a number uses helvB24. The other sets are only built for the host
	// renderer (SCREEN_LAYOUT_FONT_COMPARISON): with them the firmware does not fit in the app partition.
	// Adafruit FreeFonts are rasterised at 141 dpi (9 pt is about 17.6 px) and only have ASCII.
	static const std::vector<FontSet> sets = {
		{"Helvetica Bold + FreeUniversal 49", &HELV_B14, &HELV_B18, &HELV_B24, &HELV_B24, &FUB49, &FUB42},
#ifdef SCREEN_LAYOUT_FONT_COMPARISON
		{"FreeSans Bold", &fonts::FreeSansBold9pt7b, &fonts::FreeSansBold12pt7b, &fonts::FreeSansBold18pt7b,
		 &fonts::FreeSansBold24pt7b, &fonts::FreeSansBold24pt7b, &fonts::FreeSansBold24pt7b},
		{"New Century Schoolbook Bold + FreeUniversal 42", &NCEN_B14, &NCEN_B18, &NCEN_B24, &NCEN_B24, &FUB42, &FUB42},
#endif
	};
	return sets;
}

const ScreenLayout::FontSet &ScreenLayout::Fonts()
{
	return FontSets().front();
}

ScreenLayout::ScreenLayout(LGFX_Sprite &canvas, const FontSet &fonts) : m_canvas(canvas), m_fonts(fonts) {}

void ScreenLayout::Begin()
{
	m_canvas.fillScreen(WHITE);
	// Text is drawn without a background: rows and bars fill their own background first, and a background box per
	// line would erase the descenders of the line above.
	m_canvas.setTextColor(BLACK);
	m_canvas.setTextSize(1);
	m_canvas.setTextWrap(false);
	m_canvas.setFont(m_fonts.text);
	m_canvas.setTextDatum(textdatum_t::baseline_left);
}

std::string ScreenLayout::Fit(const std::string &text, int maxWidth)
{
	if (m_canvas.textWidth(text.c_str()) <= maxWidth)
		return text;
	std::string shortened = text;
	while (!shortened.empty() && m_canvas.textWidth((shortened + "..").c_str()) > maxWidth)
	{
		shortened.pop_back();
		// Do not leave half a UTF-8 character behind.
		while (!shortened.empty() && (static_cast<unsigned char>(shortened.back()) & 0xC0) == 0x80)
			shortened.pop_back();
		if (!shortened.empty() && (static_cast<unsigned char>(shortened.back()) & 0xC0) == 0xC0)
			shortened.pop_back();
	}
	return shortened + "..";
}

int ScreenLayout::LabelWidth(const std::string &text)
{
	int width = 0;
	std::string run;
	for (size_t i = 0; i < text.size(); i++)
	{
		if (SubscriptAt(text, i) > 0)
		{
			width += m_canvas.textWidth(run.c_str());
			run.clear();
			width += SUB_W + 2;
			i += 2;
		}
		else
		{
			run += text[i];
		}
	}
	return width + m_canvas.textWidth(run.c_str());
}

void ScreenLayout::DrawLabelOrIcon(const std::string &text, int x, int baseline, int top, int maxWidth)
{
	constexpr const char *PREFIX = "icon:";
	if (text.rfind(PREFIX, 0) == 0)
	{
		const Icons::Icon *icon = Icons::Find(text.c_str() + std::strlen(PREFIX));
		if (icon != nullptr && icon->width <= maxWidth)
		{
			m_canvas.drawBitmap(x, top, icon->bits, icon->width, icon->height, BLACK);
			return;
		}
		DrawLabel(text.substr(std::strlen(PREFIX)), x, baseline, maxWidth); // unknown icon: its name as text
		return;
	}
	DrawLabel(text, x, baseline, maxWidth);
}

void ScreenLayout::DrawLabel(const std::string &text, int x, int y, int maxWidth)
{
	m_canvas.setTextDatum(textdatum_t::baseline_left);
	if (LabelWidth(text) > maxWidth)
	{
		// Too wide: shorten it as plain text ("CO2..").
		m_canvas.drawString(Fit(WithoutSubscripts(text), maxWidth).c_str(), x, y);
		return;
	}
	std::string run;
	auto flush = [&]()
	{
		m_canvas.drawString(run.c_str(), x, y);
		x += m_canvas.textWidth(run.c_str());
		run.clear();
	};
	for (size_t i = 0; i < text.size(); i++)
	{
		if (SubscriptAt(text, i) > 0)
		{
			flush();
			int d = static_cast<unsigned char>(text[i + 2]) - 0x80;
			uint32_t colour = m_canvas.getTextStyle().fore_rgb888 != 0 ? WHITE : BLACK;
			int top = y + SUBSCRIPT_DROP - SUB_H + 1;
			for (int row = 0; row < SUB_H; row++)
				for (int col = 0; col < SUB_W; col++)
					if (SUB_DIGITS[d][row] & (0x80 >> col))
						m_canvas.drawPixel(x + 1 + col, top + row, colour);
			x += SUB_W + 2;
			i += 2;
		}
		else
		{
			run += text[i];
		}
	}
	flush();
}

int ScreenLayout::WrapText(const std::string &text, int x, int y, int maxWidth, int lineHeight, int maxY)
{
	std::vector<std::string> lines;
	std::string line;
	size_t pos = 0;
	while (pos <= text.size())
	{
		size_t space = text.find(' ', pos);
		std::string word = text.substr(pos, space == std::string::npos ? std::string::npos : space - pos);
		std::string candidate = line.empty() ? word : line + " " + word;
		if (!line.empty() && m_canvas.textWidth(candidate.c_str()) > maxWidth)
		{
			lines.push_back(line);
			line = word;
		}
		else
		{
			line = candidate;
		}
		if (space == std::string::npos)
			break;
		pos = space + 1;
	}
	if (!line.empty())
		lines.push_back(line);

	m_canvas.setTextDatum(textdatum_t::baseline_left);
	for (size_t i = 0; i < lines.size() && y <= maxY; i++)
	{
		bool lastThatFits = y + lineHeight > maxY && i + 1 < lines.size();
		m_canvas.drawString(Fit(lastThatFits ? lines[i] + " ..." : lines[i], maxWidth).c_str(), x, y);
		y += lineHeight;
	}
	return y;
}

void ScreenLayout::WifiSymbol(int x, int y, bool connected, uint32_t colour)
{
	// Three arcs above a dot, centred on (x + 8, y); crossed out when not connected.
	int cx = x + 8;
	m_canvas.fillCircle(cx, y - 1, 1, colour);
	if (connected)
	{
		for (int r : {6, 10, 14})
			m_canvas.fillArc(cx, y, r, r - 2, 225, 315, colour);
	}
	else
	{
		m_canvas.fillArc(cx, y, 6, 4, 225, 315, colour);
		for (int t = 0; t < 2; t++)
			m_canvas.drawLine(x + 1 + t, y - 14, x + 15 + t, y, colour);
	}
}

int ScreenLayout::BatteryIcon(int x, int battery)
{
	// Body 17 x 10 with a nub on the right, centred in the header bar.
	constexpr int BODY_W = 17, BODY_H = 10, NUB_W = 2, NUB_H = 4;
	int y = (HEADER_H - BODY_H) / 2;
	m_canvas.drawRect(x, y, BODY_W, BODY_H, WHITE);
	m_canvas.fillRect(x + BODY_W, y + (BODY_H - NUB_H) / 2, NUB_W, NUB_H, WHITE);
	if (battery >= 5)
	{
		// USB power: full body with a black lightning bolt.
		m_canvas.fillRect(x + 1, y + 1, BODY_W - 2, BODY_H - 2, WHITE);
		m_canvas.fillTriangle(x + 10, y + 1, x + 5, y + 5, x + 9, y + 5, BLACK);
		m_canvas.fillTriangle(x + 8, y + 4, x + 12, y + 4, x + 7, y + 8, BLACK);
	}
	else
	{
		// Bars of 2 px with 1 px between them, inside a 1 px margin.
		for (int i = 0; i < std::max(0, std::min(4, battery)); i++)
			m_canvas.fillRect(x + 2 + i * 3 + (i > 0 ? i : 0), y + 2, 2, BODY_H - 4, WHITE);
	}
	return BODY_W + NUB_W;
}

void ScreenLayout::Header(const std::string &left, const std::string &right, int leftIndent, const std::string &badge,
						  int battery)
{
	m_canvas.fillRect(0, 0, W, HEADER_H, BLACK);
	m_canvas.setFont(m_fonts.text);
	m_canvas.setTextColor(WHITE);
	constexpr int GAP = 8;
	constexpr int ICON_W = 19; // BatteryIcon

	// Left text first (it matters most), the right text gets the rest of the bar, the badge sits after the left text.
	// The battery icon sits just left of the right text. When the date, a badge, the icon and the right text (device
	// label) do not fit together, the right text is left out: it matters least.
	int x = MARGIN + leftIndent;
	int rightTextW = right.empty() ? 0 : std::min(m_canvas.textWidth(right.c_str()), W / 2);
	int iconNeeds = battery >= 0 ? ICON_W + GAP / 2 : 0;
	bool showRight = !right.empty();
	if (showRight && (battery >= 0 || !badge.empty()))
	{
		int leftW = left.empty() ? 0 : m_canvas.textWidth(left.c_str()) + GAP;
		int badgeW = badge.empty() ? 0 : m_canvas.textWidth(badge.c_str()) + 10 + GAP;
		showRight = x + leftW + badgeW + iconNeeds + rightTextW <= W - MARGIN;
	}
	int rightNeeds = (showRight ? rightTextW + GAP : 0) + iconNeeds;
	if (battery >= 0)
		BatteryIcon(W - MARGIN - (showRight ? rightTextW + GAP / 2 : 0) - ICON_W, battery);
	if (!left.empty())
	{
		std::string fitted = Fit(left, W - MARGIN - x - rightNeeds);
		m_canvas.setTextDatum(textdatum_t::baseline_left);
		m_canvas.drawString(fitted.c_str(), x, HEADER_BASE);
		x += m_canvas.textWidth(fitted.c_str()) + GAP;
	}
	if (!badge.empty())
	{
		// Badge (e.g. TEST) in a white box.
		std::string fitted = Fit(badge, W - MARGIN - x - rightNeeds - 10);
		int boxW = m_canvas.textWidth(fitted.c_str()) + 10;
		m_canvas.fillRect(x, 1, boxW, HEADER_H - 2, WHITE);
		m_canvas.setTextColor(BLACK);
		m_canvas.setTextDatum(textdatum_t::baseline_left);
		m_canvas.drawString(fitted.c_str(), x + 5, HEADER_BASE);
		m_canvas.setTextColor(WHITE);
		x += boxW + GAP;
	}
	if (showRight)
	{
		m_canvas.setTextDatum(textdatum_t::baseline_right);
		m_canvas.drawString(Fit(right, W - MARGIN - x - iconNeeds).c_str(), W - MARGIN, HEADER_BASE);
	}
	m_canvas.setTextColor(BLACK);
	m_canvas.setTextDatum(textdatum_t::baseline_left);
}

int ScreenLayout::Footer(const std::string &left, const std::string &right)
{
	if (left.empty() && right.empty())
		return H;
	m_canvas.fillRect(0, FOOTER_LINE, W, 2, BLACK);
	m_canvas.setFont(m_fonts.text);
	m_canvas.setTextColor(BLACK);
	// Both sides may start with "icon:<name>" (include/util/icons.hpp), followed by a space and text: e.g. the upload
	// icon and "14:10" on the left, the info icon on the right.
	auto split = [](const std::string &s, std::string &text) -> const Icons::Icon *
	{
		constexpr const char *PREFIX = "icon:";
		text = s;
		if (s.rfind(PREFIX, 0) != 0)
			return nullptr;
		size_t space = s.find(' ');
		std::string name = s.substr(std::strlen(PREFIX), space == std::string::npos ? std::string::npos : space - std::strlen(PREFIX));
		const Icons::Icon *icon = Icons::Find(name.c_str());
		if (icon != nullptr)
			text = space == std::string::npos ? "" : s.substr(space + 1);
		return icon;
	};
	auto iconY = [](const Icons::Icon *icon) { return FOOTER_LINE + 2 + (H - FOOTER_LINE - 2 - icon->height) / 2; };

	int rightWidth = 0;
	if (!right.empty())
	{
		std::string text;
		const Icons::Icon *icon = split(right, text);
		int xr = W - MARGIN;
		if (!text.empty())
		{
			std::string fitted = Fit(text, W / 2 - MARGIN);
			m_canvas.setTextDatum(textdatum_t::baseline_right);
			m_canvas.drawString(fitted.c_str(), xr, FOOTER_BASE);
			xr -= m_canvas.textWidth(fitted.c_str()) + (icon != nullptr ? 4 : 0);
		}
		if (icon != nullptr)
		{
			xr -= icon->width;
			m_canvas.drawBitmap(xr, iconY(icon), icon->bits, icon->width, icon->height, BLACK);
		}
		rightWidth = W - MARGIN - xr + 8;
	}
	m_canvas.setTextDatum(textdatum_t::baseline_left);
	int x = MARGIN;
	std::string text;
	if (const Icons::Icon *icon = split(left, text))
	{
		m_canvas.drawBitmap(x, iconY(icon), icon->bits, icon->width, icon->height, BLACK);
		x += icon->width + 5;
	}
	m_canvas.drawString(Fit(text, W - MARGIN - x - rightWidth).c_str(), x, FOOTER_BASE);
	return FOOTER_LINE;
}

std::string ScreenLayout::DrawBlank()
{
	Begin();
	return "clear";
}

std::string ScreenLayout::DrawQR(const std::string &payload, int padding, const std::string &text)
{
	Begin();
	constexpr int TEXT_H = 30;
	int textHeight = text.empty() ? 0 : TEXT_H;
	int size = std::min(W - 2 * padding, H - 2 * padding - textHeight);
	// LovyanGFX picks the smallest QR version that fits, scales it by whole pixels per module and centres it in a
	// white square of this size, which adds to the white padding (quiet zone).
	m_canvas.qrcode(payload.c_str(), (W - size) / 2, padding, size);
	if (!text.empty())
	{
		m_canvas.setFont(m_fonts.title);
		m_canvas.setTextDatum(textdatum_t::baseline_center);
		m_canvas.drawString(Fit(text, W - 2 * MARGIN).c_str(), W / 2, H - 9);
	}
	return "qr:" + payload;
}

std::string ScreenLayout::DrawProvisioningQR(const std::string &payload, const std::string &deviceName,
											 const std::string &displayName, const std::string &version)
{
	Begin();
	// Which device this is (a box can hold several CoreInks) and its firmware version in the header (relevant until
	// the device is packed); what to do in the footer. Without a short name: the old "Koppelen / via app".
	// The name matters most: when both do not fit, the version is written compactly ("v1.2.0-rc.6" -> "1.2.0r6").
	std::string shownVersion = version;
	if (!displayName.empty())
	{
		m_canvas.setFont(m_fonts.text);
		constexpr int GAP = 8;
		if (MARGIN + m_canvas.textWidth(displayName.c_str()) + GAP + m_canvas.textWidth(shownVersion.c_str()) > W - MARGIN)
		{
			if (!shownVersion.empty() && shownVersion[0] == 'v')
				shownVersion.erase(0, 1);
			auto rc = shownVersion.find("-rc.");
			if (rc != std::string::npos)
				shownVersion.replace(rc, 4, "r");
		}
	}
	Header(displayName.empty() ? "Koppelen" : displayName, displayName.empty() ? "via app" : shownVersion);
	// A QR code needs 4 modules of white around it: the header bar above it counts as black.
	m_canvas.qrcode(payload.c_str(), (W - PROVISIONING_QR_SIZE) / 2, HEADER_H + PROVISIONING_QR_QUIET,
					PROVISIONING_QR_SIZE);
	m_canvas.setFont(m_fonts.text);
	m_canvas.setTextDatum(textdatum_t::baseline_center);
	m_canvas.drawString(Fit(deviceName, W - 2 * MARGIN).c_str(), W / 2, PROVISIONING_NAME_BASE);
	Footer(displayName.empty() ? "" : "Scan met app", displayName.empty() ? version : "");
	return "prov:" + payload;
}

std::string ScreenLayout::DrawSteps(const std::string &title, const std::string &titleRight,
									const std::vector<Step> &steps, const std::string &message,
									const std::string &footer)
{
	Begin();
	Header(title, titleRight);
	int bottom = Footer(footer, "");
	int y = HEADER_H;
	m_canvas.setFont(m_fonts.text);
	for (const auto &step : steps)
	{
		if (y + ROW_H > bottom)
			break;
		bool active = step.state == StepState::Active;
		uint32_t fg = active ? WHITE : BLACK;
		uint32_t bg = active ? BLACK : WHITE;
		m_canvas.fillRect(0, y, W, ROW_H, bg);
		// Box with a mark: check (done), arrow (active), cross (failed), empty (to do).
		int bx = MARGIN, by = y + 5, bs = 16;
		m_canvas.drawRect(bx, by, bs, bs, fg);
		m_canvas.drawRect(bx + 1, by + 1, bs - 2, bs - 2, fg);
		switch (step.state)
		{
		case StepState::Done:
			for (int t = 0; t < 2; t++)
			{
				m_canvas.drawLine(bx + 3, by + 8 + t, bx + 6, by + 11 + t, fg);
				m_canvas.drawLine(bx + 6, by + 11 + t, bx + 12, by + 4 + t, fg);
			}
			break;
		case StepState::Active:
			m_canvas.fillTriangle(bx + 5, by + 4, bx + 5, by + 12, bx + 11, by + 8, fg);
			break;
		case StepState::Failed:
			for (int t = 0; t < 2; t++)
			{
				m_canvas.drawLine(bx + 4 + t, by + 4, bx + 11 + t, by + 11, fg);
				m_canvas.drawLine(bx + 11 + t, by + 4, bx + 4 + t, by + 11, fg);
			}
			break;
		default:
			break;
		}
		m_canvas.setTextColor(fg);
		m_canvas.setTextDatum(textdatum_t::baseline_left);
		int tx = bx + bs + 7;
		m_canvas.drawString(Fit(step.label, W - tx - MARGIN).c_str(), tx, y + (ROW_H + TEXT_CAP) / 2);
		m_canvas.drawFastHLine(0, y + ROW_H - 1, W, BLACK);
		y += ROW_H;
	}
	m_canvas.setTextColor(BLACK);
	if (!message.empty())
		WrapText(message, MARGIN, y + 5 + TEXT_CAP, W - 2 * MARGIN, LINE_HEIGHT, bottom - 4);
	// Steps advance one at a time: a fast refresh while the title stays the same.
	return "steps:" + title;
}

std::string ScreenLayout::DrawMessage(const std::string &title, const std::vector<std::string> &paragraphs,
									  const std::string &footer)
{
	Begin();
	Header(title, "");
	int bottom = Footer(footer, "");
	m_canvas.setFont(m_fonts.text);
	int y = HEADER_H + 6 + TEXT_CAP;
	for (const auto &paragraph : paragraphs)
	{
		if (y > bottom - 4)
			break;
		y = WrapText(paragraph, MARGIN, y, W - 2 * MARGIN, LINE_HEIGHT, bottom - 4) + 6;
	}
	return "msg:" + title;
}

std::string ScreenLayout::DrawStatus(const Status &status)
{
	Begin();

	// Header: Wi-Fi symbol and date left, badge (e.g. TEST) in the middle, device label right.
	Header(status.date, "", 22, status.badge, status.battery); // no device label: clear from the screen in use
	WifiSymbol(MARGIN - 1, HEADER_BASE + 1, status.wifiConnected, WHITE);
	int bottom = Footer(status.footerLeft, status.footerRight);

	// Main value with its label; the font gets smaller only if the value does not fit.
	m_canvas.setFont(m_fonts.text);
	m_canvas.setTextDatum(textdatum_t::baseline_left);
	DrawLabelOrIcon(status.bigLabel, MARGIN, STATUS_LABEL_BASE, HEADER_H + 4, W - 2 * MARGIN);
	// Value and unit share the baseline; the unit is smaller and right after the value. The value font gets smaller
	// only when value and unit do not fit.
	constexpr int UNIT_GAP = 5;
	int unitWidth = 0;
	if (!status.bigUnit.empty())
	{
		m_canvas.setFont(m_fonts.title);
		unitWidth = std::min(LabelWidth(status.bigUnit), W / 3) + UNIT_GAP;
	}
	int valueMax = W - 2 * MARGIN - unitWidth;
	const lgfx::IFont *candidates[] = {IsNumber(status.bigValue) ? m_fonts.bigNumber : m_fonts.bigText,
									   IsNumber(status.bigValue) ? m_fonts.bigNumber2 : m_fonts.bigText,
									   m_fonts.bigText, m_fonts.value};
	for (auto font : candidates)
	{
		m_canvas.setFont(font);
		if (m_canvas.textWidth(status.bigValue.c_str()) <= valueMax)
			break;
	}
	std::string value = Fit(status.bigValue, valueMax);
	m_canvas.drawString(value.c_str(), MARGIN - 1, STATUS_BIG_BASE);
	if (!status.bigUnit.empty())
	{
		int unitX = MARGIN - 1 + m_canvas.textWidth(value.c_str()) + UNIT_GAP;
		m_canvas.setFont(m_fonts.title);
		DrawLabel(status.bigUnit, unitX, STATUS_BIG_BASE, W - MARGIN - unitX);
	}

	// Tiles.
	size_t n = std::min<size_t>(status.tiles.size(), 3);
	if (n > 0)
	{
		int tileW = W / static_cast<int>(n);
		m_canvas.fillRect(0, STATUS_TILES_LINE, W, 2, BLACK);
		for (size_t i = 0; i < n; i++)
		{
			int tx = static_cast<int>(i) * tileW;
			if (i > 0)
				m_canvas.fillRect(tx - 1, STATUS_TILES_LINE, 2, bottom - STATUS_TILES_LINE, BLACK);
			int textX = tx + (i > 0 ? 4 : MARGIN);
			// The last tile ends at the screen edge, not at a divider: it also gets the rounding remainder of W / n.
			int maxW = (i + 1 == n ? W : tx + tileW) - textX - 2;
			m_canvas.setFont(m_fonts.text);
			DrawLabelOrIcon(status.tiles[i].first, textX, STATUS_TILE_LABEL_BASE, STATUS_TILES_LINE + 4, maxW);
			for (auto font : {m_fonts.value, m_fonts.title, m_fonts.text})
			{
				m_canvas.setFont(font);
				if (m_canvas.textWidth(status.tiles[i].second.c_str()) <= maxW)
					break;
			}
			m_canvas.drawString(Fit(status.tiles[i].second, maxW).c_str(), textX, STATUS_TILE_VALUE_BASE);
		}
	}

	// The layout key ignores the values: new measurements get a fast refresh.
	return "status:" + status.bigLabel + ":" + status.bigUnit + ":" + std::to_string(n) + ":" + status.badge + ":" + status.device;
}

std::string ScreenLayout::DrawMenu(const std::vector<std::string> &lines, int highlightedLine)
{
	Begin();
	if (lines.empty())
		return "menu";
	Header(lines[0], "");

	int items = static_cast<int>(lines.size()) - 1;
	int available = H - HEADER_H;
	int rowH = std::max(20, std::min(ROW_H, items > 0 ? available / items : ROW_H));
	int visible = std::max(1, available / rowH);
	// Scroll so the highlighted item is visible.
	int first = 1;
	if (highlightedLine >= first + visible)
		first = highlightedLine - visible + 1;

	m_canvas.setFont(m_fonts.text);
	int y = HEADER_H;
	for (int i = first; i < static_cast<int>(lines.size()) && i < first + visible; i++)
	{
		bool highlighted = i == highlightedLine;
		uint32_t fg = highlighted ? WHITE : BLACK;
		uint32_t bg = highlighted ? BLACK : WHITE;
		m_canvas.fillRect(0, y, W, rowH, bg);
		m_canvas.setTextColor(fg);
		m_canvas.setTextDatum(textdatum_t::baseline_left);
		std::string label = (highlighted ? "> " : "") + lines[i];
		m_canvas.drawString(Fit(label, W - 2 * MARGIN).c_str(), MARGIN, y + (rowH + TEXT_CAP) / 2);
		m_canvas.drawFastHLine(0, y + rowH - 1, W, BLACK);
		y += rowH;
	}
	m_canvas.setTextColor(BLACK);

	// Moving the highlight within the same menu: fast refresh.
	std::string key = "menu";
	for (const auto &line : lines)
		key += "|" + line;
	return key;
}
