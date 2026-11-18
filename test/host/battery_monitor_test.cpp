// Host tests of include/util/battery_monitor.hpp.
#include <util/battery_monitor.hpp>

#include <cmath>
#include <cstdio>
#include <string>

using namespace BatteryMonitor;

static int failures = 0;

static void Check(bool ok, const char *what)
{
	if (!ok)
	{
		std::printf("FAIL: %s\n", what);
		failures++;
	}
}

static bool Near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }

int main()
{
	// State of charge from the resting voltage.
	Check(Near(SocFromVoltage(4.25), 1.0), "above full");
	Check(Near(SocFromVoltage(4.20), 1.0), "full");
	Check(Near(SocFromVoltage(3.84), 0.50), "half");
	Check(Near(SocFromVoltage(3.835), 0.4875), "interpolated (three quarters between 3.82 and 3.84 V)");
	Check(Near(SocFromVoltage(3.27), 0.0), "empty");
	Check(Near(SocFromVoltage(3.0), 0.0), "below empty");
	Check(SocFromVoltage(3.13) == 0.0, "3.13 V (first home test) is empty");

	// USB: near 4.2 V, or a clear rise between two readings.
	State s;
	Check(OnUsb(4.18, s), "usb by voltage");
	Check(!OnUsb(4.05, s), "no usb without history");
	s.lastVoltage = 3.80;
	Check(OnUsb(3.86, s), "usb by rise");
	Check(!OnUsb(3.82, s), "small rise is noise");

	Model m; // 5195 J, 85% usable, 18 J/h
	// Full battery: (1 - 0.03) * 5195 * 0.85 / 18 = 238 h.
	Check(Near(HoursLeft(1.0, m, 1.0), 0.97 * m.capacity_J * 0.85 / 18.0, 1e-6) && HoursLeft(1.0, m, 1.0) > 230, "hours full (about 10 days)");
	Check(Near(HoursLeft(0.02, m, 1.0), 0.0), "hours below empty");
	Check(Near(HoursLeft(0.5, m, 2.0), HoursLeft(0.5, m, 1.0) / 2), "factor halves the hours");

	// Levels and hysteresis.
	Check(LevelFor(100, 0.5, m, Level::Unknown) == Level::Ok, "ok");
	Check(LevelFor(20, 0.5, m, Level::Ok) == Level::Day, "day");
	Check(LevelFor(5, 0.5, m, Level::Day) == Level::Today, "today");
	Check(LevelFor(0.5, 0.5, m, Level::Today) == Level::Critical, "critical");
	Check(LevelFor(10, 0.02, m, Level::Ok) == Level::Empty, "empty by charge");
	Check(LevelFor(6.5, 0.5, m, Level::Today) == Level::Today, "no flapping back to day at 6.5 h");
	Check(LevelFor(8, 0.5, m, Level::Today) == Level::Day, "back to day at 8 h");
	Check(LevelFor(1.1, 0.5, m, Level::Critical) == Level::Critical, "no flapping out of critical");
	Check(LevelFor(26, 0.5, m, Level::Day) == Level::Day, "no flapping to ok at 26 h");
	Check(LevelFor(31, 0.5, m, Level::Day) == Level::Ok, "ok again at 31 h");

	Check(BarsFor(0.9) == 4 && BarsFor(0.6) == 3 && BarsFor(0.3) == 2 && BarsFor(0.1) == 1 && BarsFor(0.05) == 0, "bars");

	// Update: on USB, then on battery with learning of the real use.
	State st;
	bool dirty = false;
	Reading r = Update(st, 4.19, 1000, m, dirty);
	Check(r.usb && r.level == Level::Usb && dirty, "update on usb");
	r = Update(st, 4.10, 2000, m, dirty); // plug pulled: below 4.15 and not rising
	Check(!r.usb && r.level == Level::Ok && st.sampleTime == 2000 && dirty, "plug pulled: learning starts");
	double soc0 = st.sampleSoc;
	r = Update(st, 4.095, 2000 + 600, m, dirty);
	Check(st.sampleTime == 2000 && Near(st.factor, 1.0), "too soon to learn");
	// 4 hours later the charge dropped from ~0.88 to 0.80: (0.08 * 5195 * 0.85 / 4) = 88 J/h, ratio ~4.9 -> capped step.
	r = Update(st, 4.02, 2000 + 4 * 3600, m, dirty);
	double ratio = (soc0 - 0.80) * m.capacity_J * m.usable / 4.0 / m.perHour_J;
	Check(Near(st.factor, std::min(4.0, 0.7 + 0.3 * ratio), 1e-9), "factor learned");
	Check(st.sampleTime == 2000 + 4 * 3600 && Near(st.sampleSoc, 0.80), "new learning point");
	Check(dirty, "learning step is written");
	double hoursWithFactor = r.hours;
	Check(Near(hoursWithFactor, HoursLeft(0.80, m, st.factor)), "hours use the factor");
	r = Update(st, 4.019, 2000 + 4 * 3600 + 600, m, dirty);
	Check(!dirty, "unchanged reading is not written (spares the flash)");
	r = Update(st, 4.18, 2000 + 5 * 3600, m, dirty);
	Check(r.usb && st.sampleSoc < 0 && st.factor > 1.0, "usb again: learning point dropped, factor kept");

	// Footer texts fit (about 16 characters next to nothing else).
	Reading day;
	day.level = Level::Day;
	day.hours = 18.4;
	Check(FooterText(day) == "opladen: ±18 u", "footer day");
	Reading today;
	today.level = Level::Today;
	today.hours = 4.6;
	Check(FooterText(today) == "laad nu op: ±5 u", "footer today");
	today.hours = 1.5;
	Check(FooterText(today) == "laad nu op: <2 u", "footer today, less than 2 h");
	Reading ok;
	ok.level = Level::Ok;
	Check(FooterText(ok).empty(), "no footer when ok");
	Check(HoursText(100) == "±4 dagen", "days");
	// "±" is two bytes in UTF-8; count characters.
	auto chars = [](const std::string &t)
	{
		int n = 0;
		for (unsigned char c : t)
			n += (c & 0xC0) != 0x80;
		return n;
	};
	Check(chars(FooterText(today)) <= 16 && chars(FooterText(day)) <= 16, "footer length");

	// NVS round trip.
	State a;
	a.lastVoltage = 3.912;
	a.sampleTime = 1795000000;
	a.sampleSoc = 0.6512;
	a.factor = 1.234;
	a.level = Level::Day;
	State b;
	Check(Parse(Serialize(a), b), "parse serialized");
	Check(Near(b.lastVoltage, 3.912) && b.sampleTime == 1795000000 && Near(b.sampleSoc, 0.6512) &&
			  Near(b.factor, 1.234) && b.level == Level::Day,
		  "round trip");
	State c;
	Check(!Parse("", c) && !Parse("3.9,1,0.5,1.0", c) && !Parse("3.9,1,0.5,1.0,3x", c) && !Parse("3.9,1,0.5,99,3", c),
		  "parse rejects garbage");

	if (failures)
	{
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("battery_monitor_test: all passed\n");
	return 0;
}
