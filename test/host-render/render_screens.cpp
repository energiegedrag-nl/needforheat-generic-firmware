// Host renderer for the e-ink screens: draws every screen of include/util/screen_layout.hpp with sample data,
// once per font set, and writes them as 1-bit PBM images (200x200), to check the layout before flashing.
// Built and run in CI (.github/workflows/build.yml, job screen-renders) against LovyanGFX on Linux; see README.md.
#include <util/screen_layout.hpp>

#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace
{
	using Step = ScreenLayout::Step;
	using S = ScreenLayout::StepState;

	struct Sample
	{
		const char *name;
		std::function<void(ScreenLayout &)> draw;
	};

	std::vector<Step> ProvisioningSteps(int stage)
	{
		return {
			{S::Done, "Bluetooth verbonden"},
			{S::Done, "Server: TEST"},
			{stage == 0 ? S::Active : stage == 1 ? S::Failed : S::Done, "Wifi: Thuisnetwerk-5G"},
			{stage == 2 ? S::Active : S::Todo, "Activeren"},
		};
	}

	ScreenLayout::Status Big(const char *label, const char *value, const char *unit)
	{
		ScreenLayout::Status s;
		s.date = "wo 18 nov";
		s.badge = "TEST";
		s.device = "P1";
		s.bigLabel = label;
		s.bigValue = value;
		s.bigUnit = unit;
		s.tiles = {{"gas m³", "1,2"}, {"DSMR", "5.0"}, {"lezer", "aan"}};
		s.footerLeft = "verstuurd 14:10";
		s.footerRight = "info";
		return s;
	}

	ScreenLayout::Status P1Base(const char *label, const char *value, const char *unit,
								std::vector<std::pair<std::string, std::string>> tiles, const char *supply)
	{
		ScreenLayout::Status s = Big(label, value, unit);
		s.tiles = std::move(tiles);
		s.footerRight = supply;
		return s;
	}

	const std::vector<Sample> &Samples()
	{
		static const std::vector<Sample> samples = {
			{"01-provisioning-qr", [](ScreenLayout &l)
			 { l.DrawProvisioningQR("NFH:EENCWPCNLYASGRLHRGV4333G", "NFH_3C4D5E", "living room", "v3.5.0"); }},
			{"02-steps-wifi", [](ScreenLayout &l)
			 { l.DrawSteps("Koppelen", "2 van 3", ProvisioningSteps(0), "Verbinden met wifi (tot 30 s)...",
						   "Houd de app open"); }},
			{"03-steps-wifi-failed", [](ScreenLayout &l)
			 { l.DrawSteps("Koppelen", "2 van 3", ProvisioningSteps(1),
						   "Klopt het wachtwoord? Is het 2,4 GHz? De module begint opnieuw.", ""); }},
			{"04-steps-activating", [](ScreenLayout &l)
			 { l.DrawSteps("Koppelen", "3 van 3", ProvisioningSteps(2), "Aanmelden bij de server...",
						   "Houd de app open"); }},
			{"05-activation-failed", [](ScreenLayout &l)
			 { l.DrawMessage("Activeren lukt niet",
							 {"Server niet bereikbaar. Nieuwe poging over 60 s.",
							  "Blijft het mislukken? Reset de wifi (knop) en koppel opnieuw."},
							 "code 0  api.tst.needforheat.nl"); }},
			{"06-info-qr", [](ScreenLayout &l)
			 { l.DrawQR("https://www.needforheat.nl/info/living-room-module", 10, "  Info? Scan!  "); }},
			{"07-status-living-room", [](ScreenLayout &l)
			 {
				 ScreenLayout::Status s;
				 s.date = "wo 18 nov";
				 s.badge = "TEST";
				 s.device = "WK";
				 s.bigLabel = "CO₂";
				 s.bigValue = "612";
				 s.bigUnit = "ppm";
				 s.tiles = {{"°C", "20,5"}, {"RV %", "48"}, {"pers.", "2"}};
				 s.footerLeft = "verstuurd 14:10";
				 s.footerRight = "info";
				 l.DrawStatus(s);
			 }},
			{"08-status-p1", [](ScreenLayout &l)
			 {
				 ScreenLayout::Status s;
				 s.date = "wo 18 nov";
				 s.badge = "TEST";
				 s.device = "P1";
				 s.bigLabel = "stroom";
				 s.bigValue = "0,42";
				 s.bigUnit = "kW";
				 s.tiles = {{"gas m³", "1,2"}, {"DSMR", "5.0"}, {"lezer", "aan"}};
				 s.footerLeft = "verstuurd 14:10";
				 s.footerRight = "info";
				 l.DrawStatus(s);
			 }},
			{"09-status-offline", [](ScreenLayout &l)
			 {
				 ScreenLayout::Status s;
				 s.date = "do 19 nov";
				 s.wifiConnected = false;
				 s.device = "P1";
				 s.bigLabel = "stroom";
				 s.bigValue = "-1,85";
				 s.bigUnit = "kW";
				 s.tiles = {{"gas m³", "0,0"}, {"DSMR", "5.0"}};
				 s.footerLeft = "128 wachten";
				 s.footerRight = "info";
				 l.DrawStatus(s);
			 }},
			{"10-menu", [](ScreenLayout &l)
			 {
				 l.DrawMenu({"SMARTPHONES:", "+ toevoegen", "Telefoon van Sam", "Pixel 8", "iPhone 15 Pro Max van iemand",
							 "Galaxy A54", "Fairphone 5", "terug"},
							6);
			 }},
			// The widest values that occur at home: they must fit with their unit.
			{"11-status-co2-high", [](ScreenLayout &l)
			 { l.DrawStatus(Big("CO₂", "1850", "ppm")); }},
			{"12-status-power-export", [](ScreenLayout &l)
			 { l.DrawStatus(Big("stroom", "-12,4", "kW")); }},
			{"13-status-power-high", [](ScreenLayout &l)
			 { l.DrawStatus(Big("stroom", "17,3", "kW")); }},
			{"14-status-temperature", [](ScreenLayout &l)
			 { l.DrawStatus(Big("binnen", "21,5", "°C")); }},
			{"15-status-gas-rate", [](ScreenLayout &l)
			 { l.DrawStatus(Big("gas", "1,25", "m³/h")); }},
			// P1-BASE v3 with the P1 reader firmware (needforheat-p1-reader-firmware, src/p1.cpp): screen 8 (P1 overview)
			// and screen 9 (P1 cable diagnosis), with the texts that firmware uses; supply indicator in the footer.
			{"16-p1base-overview", [](ScreenLayout &l)
			 { l.DrawStatus(P1Base("stroom", "0,42", "kW", {{"DSMR", "5.0"}, {"lezer", "aan"}, {"laden", "120mA"}}, "P1")); }},
			{"17-p1base-dsmr3-usb", [](ScreenLayout &l)
			 { l.DrawStatus(P1Base("stroom", "-1,85", "kW", {{"DSMR", "3.0"}, {"lezer", "uit"}, {"laden", "uit"}}, "USB")); }},
			{"18-p1base-no-data", [](ScreenLayout &l)
			 { l.DrawStatus(P1Base("geen data", "5,0", "V", {{"lezer", "uit"}, {"laden", "uit"}}, "accu")); }},
			{"19-p1base-dsmr2-on-battery", [](ScreenLayout &l)
			 { l.DrawStatus(P1Base("stroom", "0,42", "kW", {{"DSMR", "2.2"}, {"lezer", "aan"}, {"laden", "USB!"}}, "accu")); }},
			{"20-p1base-no-meter", [](ScreenLayout &l)
			 { l.DrawStatus(P1Base("geen meter", "0,0", "V", {{"lezer", "uit"}, {"laden", "uit"}}, "accu")); }},
			{"21-p1base-overloaded", [](ScreenLayout &l)
			 { l.DrawStatus(P1Base("P1 overbelast", "4,5", "V", {{"lezer", "aan"}, {"laden", "60mA"}}, "P1")); }},
		};
		return samples;
	}

	bool WritePbm(LGFX_Sprite &canvas, const std::string &path)
	{
		FILE *f = std::fopen(path.c_str(), "wb");
		if (f == nullptr)
			return false;
		const int w = canvas.width(), h = canvas.height();
		std::fprintf(f, "P4\n%d %d\n", w, h);
		std::vector<unsigned char> row((w + 7) / 8);
		for (int y = 0; y < h; y++)
		{
			std::fill(row.begin(), row.end(), 0);
			for (int x = 0; x < w; x++)
				if (canvas.readPixelValue(x, y) == ScreenLayout::BLACK) // PBM: 1 is black
					row[x / 8] |= 0x80 >> (x % 8);
			std::fwrite(row.data(), 1, row.size(), f);
		}
		std::fclose(f);
		return true;
	}
} // namespace

int main(int argc, char **argv)
{
	std::string outDir = argc > 1 ? argv[1] : ".";
	LGFX_Sprite canvas;
	canvas.setColorDepth(1);
	if (canvas.createSprite(ScreenLayout::W, ScreenLayout::H) == nullptr)
	{
		std::fprintf(stderr, "Cannot create the sprite.\n");
		return 1;
	}
	canvas.setPaletteColor(ScreenLayout::BLACK, 0x000000u);
	canvas.setPaletteColor(ScreenLayout::WHITE, 0xFFFFFFu);

	int failures = 0;
	const auto &sets = ScreenLayout::FontSets();
	for (size_t i = 0; i < sets.size(); i++)
	{
		for (const auto &sample : Samples())
		{
			ScreenLayout layout(canvas, sets[i]);
			sample.draw(layout);
			std::string path = outDir + "/" + sample.name + "-font" + std::to_string(i) + ".pbm";
			if (!WritePbm(canvas, path))
			{
				std::fprintf(stderr, "Cannot write %s\n", path.c_str());
				failures++;
			}
		}
		std::printf("font%zu: %s\n", i, sets[i].name);
	}
	return failures == 0 ? 0 : 1;
}
