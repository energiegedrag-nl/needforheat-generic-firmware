#include <util/screen.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include <esp_log.h>

/*
 * Layout on the 200x200 panel (see the "NeedForHeat e-ink GUI" design):
 *   header bar   y 0..17, line at y 18
 *   content      y 19..181
 *   footer bar   line at y 182, y 183..199
 * Colours: the off-screen buffer is 1-bit with a palette, so colours are palette indices: 0 black, 1 white.
 */

namespace
{
	constexpr const char *TAG = "Screen";

	constexpr uint32_t BLACK = 0;
	constexpr uint32_t WHITE = 1;

	constexpr int W = 200;
	constexpr int H = 200;
	constexpr int HEADER_H = 18;
	constexpr int FOOTER_Y = 182;
	constexpr int MARGIN = 5;

	const lgfx::IFont *FONT_BAR = &fonts::FreeSansBold9pt7b;
	const lgfx::IFont *FONT_SMALL = &fonts::FreeSans9pt7b;
	const lgfx::IFont *FONT_BODY = &fonts::FreeSansBold9pt7b;
	const lgfx::IFont *FONT_TITLE = &fonts::FreeSansBold12pt7b;
	const lgfx::IFont *FONT_TILE = &fonts::FreeSansBold18pt7b;
	const lgfx::IFont *FONT_BIG_TEXT = &fonts::FreeSansBold24pt7b;
	const lgfx::IFont *FONT_BIG_DIGITS = &fonts::Font7; // 7-segment, 48 px: digits . : -

	bool OnlyDigits(const std::string &text)
	{
		return !text.empty() && std::all_of(text.begin(), text.end(), [](char c)
											{ return (c >= '0' && c <= '9') || c == '.' || c == ':' || c == '-' || c == ' '; });
	}
} // namespace

std::string Screen::s_infoURL;
std::string Screen::s_infoText;
int Screen::s_infoPadding = 0;

Screen &Screen::Instance()
{
	static Screen screen;
	return screen;
}

Screen::Screen() : m_canvas(&e_ink_display)
{
	e_ink_display.init_without_reset();
	e_ink_display.setEpdMode(epd_mode_t::epd_quality);

	m_canvas.setColorDepth(1);
	if (m_canvas.createSprite(W, H) == nullptr)
		ESP_LOGE(TAG, "Not enough memory for the screen buffer; the screen will not be updated.");
	m_canvas.setPaletteColor(BLACK, 0x000000u);
	m_canvas.setPaletteColor(WHITE, 0xFFFFFFu);
}

void Screen::Begin()
{
	m_canvas.fillScreen(WHITE);
	m_canvas.setTextColor(BLACK, WHITE);
	m_canvas.setTextSize(1);
	m_canvas.setTextWrap(false);
}

void Screen::Push(const std::string &screenKey)
{
	if (m_canvas.getBuffer() == nullptr)
		return;
	// Same layout as on screen: a fast partial refresh, with a full one every FAST_REFRESH_LIMIT times against
	// ghosting. Another layout: always a full refresh.
	bool fast = !m_screenKey.empty() && screenKey == m_screenKey && m_fastRefreshes < FAST_REFRESH_LIMIT;
	m_fastRefreshes = fast ? m_fastRefreshes + 1 : 0;
	m_screenKey = screenKey;
	e_ink_display.setEpdMode(fast ? epd_mode_t::epd_fastest : epd_mode_t::epd_quality);
	e_ink_display.startWrite();
	m_canvas.pushSprite(0, 0);
	e_ink_display.endWrite();
}

std::string Screen::Fit(const std::string &text, int maxWidth)
{
	if (m_canvas.textWidth(text.c_str()) <= maxWidth)
		return text;
	std::string shortened = text;
	while (!shortened.empty() && m_canvas.textWidth((shortened + "..").c_str()) > maxWidth)
		shortened.pop_back();
	return shortened + "..";
}

int Screen::WrapText(const std::string &text, int x, int y, int maxWidth, int lineHeight)
{
	m_canvas.setTextDatum(textdatum_t::top_left);
	std::string line;
	size_t pos = 0;
	while (pos <= text.size())
	{
		size_t space = text.find(' ', pos);
		std::string word = text.substr(pos, space == std::string::npos ? std::string::npos : space - pos);
		std::string candidate = line.empty() ? word : line + " " + word;
		if (!line.empty() && m_canvas.textWidth(candidate.c_str()) > maxWidth)
		{
			m_canvas.drawString(line.c_str(), x, y);
			y += lineHeight;
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
	{
		m_canvas.drawString(Fit(line, maxWidth).c_str(), x, y);
		y += lineHeight;
	}
	return y;
}

void Screen::Header(const std::string &left, const std::string &right, bool rightInverted)
{
	m_canvas.setFont(FONT_BAR);
	m_canvas.setTextColor(BLACK, WHITE);
	m_canvas.setTextDatum(textdatum_t::middle_left);
	int rightWidth = right.empty() ? 0 : m_canvas.textWidth(right.c_str()) + 8;
	m_canvas.drawString(Fit(left, W - 2 * MARGIN - rightWidth).c_str(), MARGIN, HEADER_H / 2);
	if (!right.empty())
	{
		if (rightInverted)
		{
			m_canvas.fillRect(W - rightWidth - 2, 1, rightWidth, HEADER_H - 2, BLACK);
			m_canvas.setTextColor(WHITE, BLACK);
		}
		m_canvas.setTextDatum(textdatum_t::middle_right);
		m_canvas.drawString(right.c_str(), W - MARGIN - (rightInverted ? 1 : 0), HEADER_H / 2);
		m_canvas.setTextColor(BLACK, WHITE);
	}
	m_canvas.fillRect(0, HEADER_H, W, 2, BLACK);
}

void Screen::Footer(const std::string &left, const std::string &right)
{
	if (left.empty() && right.empty())
		return;
	m_canvas.fillRect(0, FOOTER_Y, W, 2, BLACK);
	m_canvas.setFont(FONT_SMALL);
	m_canvas.setTextColor(BLACK, WHITE);
	int y = FOOTER_Y + (H - FOOTER_Y) / 2 + 1;
	int rightWidth = right.empty() ? 0 : m_canvas.textWidth(right.c_str()) + 8;
	m_canvas.setTextDatum(textdatum_t::middle_left);
	m_canvas.drawString(Fit(left, W - 2 * MARGIN - rightWidth).c_str(), MARGIN, y);
	if (!right.empty())
	{
		m_canvas.setTextDatum(textdatum_t::middle_right);
		m_canvas.drawString(right.c_str(), W - MARGIN, y);
	}
}

void Screen::DrawQR(const std::string &payload, int x, int y, int maxSize)
{
	// LovyanGFX picks the smallest QR version that fits and scales it by a whole number of pixels per module.
	m_canvas.qrcode(payload.c_str(), x, y, maxSize);
}

void Screen::Clear()
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);
	Begin();
	m_screenKey.clear();
	Push("clear");
}

void Screen::DisplayQR(const std::string &payload, int padding, const std::string &text)
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);
	Begin();
	int textHeight = text.empty() ? 0 : 34;
	int size = std::min(W - 2 * padding, H - 2 * padding - textHeight);
	DrawQR(payload, (W - size) / 2, padding, size);
	if (!text.empty())
	{
		m_canvas.setFont(FONT_TITLE);
		m_canvas.setTextDatum(textdatum_t::middle_center);
		m_canvas.drawString(Fit(text, W - 2 * MARGIN).c_str(), W / 2, padding + size + textHeight / 2);
	}
	Push("qr:" + payload);
}

void Screen::DisplayProvisioningQR(const std::string &payload, const std::string &deviceName,
								   const std::string &footerLeft, const std::string &footerRight)
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);
	Begin();
	Header("Koppelen", "scan in de app");
	// QR between header and name; whole pixels per module, white margin of at least 4 modules on e-ink.
	int top = HEADER_H + 6;
	int size = 136;
	DrawQR(payload, (W - size) / 2, top, size);
	m_canvas.setFont(FONT_BODY);
	m_canvas.setTextDatum(textdatum_t::middle_center);
	m_canvas.drawString(Fit(deviceName, W - 2 * MARGIN).c_str(), W / 2, top + size + 11);
	Footer(footerLeft, footerRight);
	Push("prov:" + payload);
}

void Screen::DisplaySteps(const std::string &title, const std::string &titleRight, const std::vector<Step> &steps,
						  const std::string &message, const std::string &footer)
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);
	Begin();
	Header(title, titleRight);
	constexpr int ROW_H = 26;
	int y = HEADER_H + 2;
	m_canvas.setFont(FONT_BODY);
	for (const auto &step : steps)
	{
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
		m_canvas.setTextColor(fg, bg);
		m_canvas.setTextDatum(textdatum_t::middle_left);
		m_canvas.drawString(Fit(step.label, W - bx - bs - 2 * MARGIN).c_str(), bx + bs + 8, y + ROW_H / 2);
		m_canvas.drawFastHLine(0, y + ROW_H - 1, W, BLACK);
		y += ROW_H;
	}
	m_canvas.setTextColor(BLACK, WHITE);
	if (!message.empty())
	{
		m_canvas.setFont(FONT_SMALL);
		WrapText(message, MARGIN, y + 6, W - 2 * MARGIN, 18);
	}
	Footer(footer, "");
	// Steps advance one at a time: a fast refresh while the title stays the same.
	Push("steps:" + title);
}

void Screen::DisplayMessage(const std::string &title, const std::vector<std::string> &paragraphs,
							const std::string &footer)
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);
	Begin();
	m_canvas.setFont(FONT_TITLE);
	int y = WrapText(title, MARGIN, 8, W - 2 * MARGIN, 26) + 4;
	m_canvas.setFont(FONT_SMALL);
	for (const auto &paragraph : paragraphs)
	{
		if (y > FOOTER_Y - 18)
			break;
		y = WrapText(paragraph, MARGIN, y, W - 2 * MARGIN, 18) + 6;
	}
	Footer(footer, "");
	Push("msg:" + title);
}

void Screen::DisplayStatus(const Status &status)
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);
	Begin();

	// Header: Wi-Fi symbol and clock left, badge (e.g. TEST) right.
	int x = MARGIN;
	for (int r = 0; r < 3; r++)
	{
		int radius = 4 + r * 4;
		if (status.wifiConnected || r == 0)
			m_canvas.drawArc(x + 8, 16, radius, radius - 1, 225, 315, BLACK);
	}
	if (!status.wifiConnected)
		m_canvas.drawLine(x + 1, 3, x + 15, 16, BLACK);
	m_canvas.setFont(FONT_BAR);
	m_canvas.setTextDatum(textdatum_t::middle_left);
	if (!status.clock.empty())
		m_canvas.drawString(status.clock.c_str(), x + 22, HEADER_H / 2);
	Header("", status.badge, true);

	// Big value.
	m_canvas.setFont(FONT_BODY);
	m_canvas.setTextDatum(textdatum_t::top_left);
	m_canvas.drawString(Fit(status.bigLabel, W - 2 * MARGIN).c_str(), MARGIN, HEADER_H + 6);
	bool digits = OnlyDigits(status.bigValue);
	m_canvas.setFont(digits ? FONT_BIG_DIGITS : FONT_BIG_TEXT);
	m_canvas.drawString(Fit(status.bigValue, W - 2 * MARGIN).c_str(), MARGIN, HEADER_H + 28);

	// Tiles.
	size_t n = std::min<size_t>(status.tiles.size(), 3);
	if (n > 0)
	{
		int top = 106;
		int tileW = W / static_cast<int>(n);
		m_canvas.fillRect(0, top, W, 2, BLACK);
		for (size_t i = 0; i < n; i++)
		{
			int tx = static_cast<int>(i) * tileW;
			if (i > 0)
				m_canvas.fillRect(tx - 1, top, 2, FOOTER_Y - top, BLACK);
			m_canvas.setFont(FONT_SMALL);
			m_canvas.setTextDatum(textdatum_t::top_left);
			m_canvas.drawString(Fit(status.tiles[i].first, tileW - 2 * MARGIN).c_str(), tx + MARGIN, top + 6);
			m_canvas.setFont(FONT_TILE);
			m_canvas.drawString(Fit(status.tiles[i].second, tileW - 2 * MARGIN).c_str(), tx + MARGIN, top + 30);
		}
	}
	Footer(status.footerLeft, status.footerRight);

	// The layout key ignores the values: new measurements get a fast refresh.
	Push("status:" + status.bigLabel + ":" + std::to_string(n) + ":" + status.badge);
}

void Screen::DrawMenu(std::vector<std::string> menuLines, int highlightedLine)
{
	if (menuLines.empty())
		return;
	std::lock_guard<std::recursive_mutex> lock(m_mutex);
	Begin();
	Header(menuLines[0], "");

	int items = static_cast<int>(menuLines.size()) - 1;
	int available = H - HEADER_H - 2;
	int rowH = std::max(20, std::min(26, items > 0 ? available / items : 26));
	int visible = std::max(1, available / rowH);
	// Scroll so the highlighted item is visible.
	int first = 1;
	if (highlightedLine >= first + visible)
		first = highlightedLine - visible + 1;

	m_canvas.setFont(FONT_BODY);
	int y = HEADER_H + 2;
	for (int i = first; i < static_cast<int>(menuLines.size()) && i < first + visible; i++)
	{
		bool highlighted = i == highlightedLine;
		uint32_t fg = highlighted ? WHITE : BLACK;
		uint32_t bg = highlighted ? BLACK : WHITE;
		m_canvas.fillRect(0, y, W, rowH, bg);
		m_canvas.setTextColor(fg, bg);
		m_canvas.setTextDatum(textdatum_t::middle_left);
		std::string label = (highlighted ? "> " : "") + menuLines[i];
		m_canvas.drawString(Fit(label, W - 2 * MARGIN).c_str(), MARGIN, y + rowH / 2);
		m_canvas.drawFastHLine(0, y + rowH - 1, W, BLACK);
		y += rowH;
	}
	m_canvas.setTextColor(BLACK, WHITE);

	// Moving the highlight within the same menu: fast refresh.
	std::string key = "menu";
	for (const auto &line : menuLines)
		key += "|" + line;
	Push(key);
}

void Screen::SetInfoQRDetails(const std::string &payload, int padding, const std::string &text)
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);
	s_infoURL = payload;
	s_infoPadding = padding;
	s_infoText = text;
}

void Screen::DisplayInfoQR()
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);
	if (s_infoURL.empty())
		return;
	DisplayQR(s_infoURL, s_infoPadding, s_infoText);
}

void Screen::PrepareForPowerOff()
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);
	// Let the last refresh finish before the power is cut, so the image stays sharp and the panel's
	// high voltage is switched off properly (instead of cutting power halfway a refresh).
	e_ink_display.waitDisplay();
	e_ink_display.sleep();
}
