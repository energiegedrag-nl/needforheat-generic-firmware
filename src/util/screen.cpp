#include <util/screen.hpp>

#include <string>
#include <vector>

#include <esp_log.h>

/*
 * The panel, its refresh policy and the lock. The screens are drawn by ScreenLayout (util/screen_layout.hpp)
 * in the off-screen sprite, then pushed to the panel in one go.
 */

namespace
{
	constexpr const char *TAG = "Screen";
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
	if (m_canvas.createSprite(ScreenLayout::W, ScreenLayout::H) == nullptr)
		ESP_LOGE(TAG, "Not enough memory for the screen buffer; the screen will not be updated.");
	m_canvas.setPaletteColor(ScreenLayout::BLACK, 0x000000u);
	m_canvas.setPaletteColor(ScreenLayout::WHITE, 0xFFFFFFu);
}

void Screen::Push(const std::string &screenKey)
{
	if (m_canvas.getBuffer() == nullptr)
		return;
	if (m_asleep)
	{
		// The power stayed on after PrepareForPowerOff() (e.g. on USB).
		e_ink_display.wakeup();
		m_asleep = false;
	}
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

void Screen::Clear()
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);
	ScreenLayout layout(m_canvas);
	auto key = layout.DrawBlank();
	m_screenKey.clear();
	Push(key);
}

void Screen::DisplayQR(const std::string &payload, int padding, const std::string &text)
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);
	ScreenLayout layout(m_canvas);
	Push(layout.DrawQR(payload, padding, text));
}

void Screen::DisplayProvisioningQR(const std::string &payload, const std::string &deviceName,
								   const std::string &displayName, const std::string &version)
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);
	ScreenLayout layout(m_canvas);
	Push(layout.DrawProvisioningQR(payload, deviceName, displayName, version));
}

void Screen::DisplaySteps(const std::string &title, const std::string &titleRight, const std::vector<Step> &steps,
						  const std::string &message, const std::string &footer)
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);
	ScreenLayout layout(m_canvas);
	Push(layout.DrawSteps(title, titleRight, steps, message, footer));
}

void Screen::DisplayMessage(const std::string &title, const std::vector<std::string> &paragraphs,
							const std::string &footer)
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);
	ScreenLayout layout(m_canvas);
	Push(layout.DrawMessage(title, paragraphs, footer));
}

void Screen::DisplayStatus(const Status &status)
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);
	if (m_menuOpen && m_screenKey.rfind("menu", 0) == 0 && xTaskGetTickCount() - m_lastMenuTick < pdMS_TO_TICKS(MENU_IDLE_S * 1000))
		return;
	ScreenLayout layout(m_canvas);
	Push(layout.DrawStatus(status));
}

void Screen::DrawMenu(std::vector<std::string> menuLines, int highlightedLine)
{
	if (menuLines.empty())
		return;
	std::lock_guard<std::recursive_mutex> lock(m_mutex);
	ScreenLayout layout(m_canvas);
	auto key = layout.DrawMenu(menuLines, highlightedLine);
	m_lastMenuTick = xTaskGetTickCount();
	m_menuOpen = true;
	Push(key);
}

void Screen::CloseMenu()
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);
	m_menuOpen = false;
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
	if (!m_asleep)
	{
		e_ink_display.sleep();
		m_asleep = true;
	}
}
