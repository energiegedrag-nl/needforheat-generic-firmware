#pragma once
// Battery monitor for a single LiPo cell without a fuel gauge (M5Stack CoreInk, 390 mAh).
//
// A step between a plain voltage threshold and a coulomb counter:
//  1. the resting voltage (measured at boot, before Wi-Fi) gives the state of charge from a LiPo OCV table;
//  2. the energy model of the scheduler (rounds per hour x energy per round) gives the expected use per hour;
//  3. while on battery, the measured drop in charge over hours corrects that model (a factor kept in NVS);
//  4. remaining hours = usable energy left / (expected use per hour x factor), with warning levels and hysteresis.
// Header-only and without ESP-IDF, so the host tests can run it (test/host/battery_monitor_test.cpp).
// With a BatteryBASE (fuel gauge) this can be replaced by measured values; see TODO.md (batterybase).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace BatteryMonitor
{
	/** Battery and energy model of a device type. */
	struct Model
	{
		double capacity_J = 390.0 * 3.7 * 3.6; // 390 mAh at 3.7 V nominal = 5195 J
		double usable = 0.85;					// part of the capacity that the device can use (ageing, cut-off, cold)
		double perHour_J = 18.0;				// expected use per hour: rounds per hour x energy per round
		double emptySoc = 0.03;					// below this the cell is treated as empty (the CoreInk browns out)
	};

	/** Warning level, from good to bad. */
	enum class Level : int
	{
		Unknown = 0,
		Usb = 1,	  // on USB power (charging or full)
		Ok = 2,		  // more than a day left
		Day = 3,	  // at most 24 h left: charge soon
		Today = 4,	  // at most 6 h left: charge today
		Critical = 5, // at most 1 h left: full-screen warning
		Empty = 6	  // last screen before the device stops
	};

	/** What is kept in NVS between boots (the CoreInk switches off between rounds). */
	struct State
	{
		int64_t sampleTime = 0;	 // start of the current learning interval on battery (UTC s); 0: none
		double sampleSoc = -1;	 // state of charge at sampleTime
		double factor = 1.0;	 // measured use / modelled use, learned on battery
		double lastVoltage = 0;	 // last stored resting voltage, to see it rise when charging
		Level level = Level::Unknown;
	};

	/** Result of one update. */
	struct Reading
	{
		double voltage = 0;
		double soc = -1; // 0..1; -1: unknown
		bool usb = false;
		double hours = -1; // estimated hours left on battery; -1: unknown or on USB
		Level level = Level::Unknown;
		int bars = -1; // 0..4 for the icon; -1: no icon
	};

	/** Resting voltage of a 1S LiPo against state of charge (typical curve; the middle part is flat, the end steep). */
	inline double SocFromVoltage(double v)
	{
		static const double table[][2] = {
			{3.27, 0.00}, {3.61, 0.05}, {3.69, 0.10}, {3.71, 0.15}, {3.73, 0.20}, {3.75, 0.25}, {3.77, 0.30},
			{3.79, 0.35}, {3.80, 0.40}, {3.82, 0.45}, {3.84, 0.50}, {3.85, 0.55}, {3.87, 0.60}, {3.91, 0.65},
			{3.95, 0.70}, {3.98, 0.75}, {4.02, 0.80}, {4.08, 0.85}, {4.11, 0.90}, {4.15, 0.95}, {4.20, 1.00}};
		constexpr int n = sizeof(table) / sizeof(table[0]);
		if (v <= table[0][0])
			return 0.0;
		if (v >= table[n - 1][0])
			return 1.0;
		for (int i = 1; i < n; i++)
		{
			if (v <= table[i][0])
			{
				double f = (v - table[i - 1][0]) / (table[i][0] - table[i - 1][0]);
				return table[i - 1][1] + f * (table[i][1] - table[i - 1][1]);
			}
		}
		return 1.0;
	}

	/** On USB power: the charger keeps the cell near 4.2 V, or the resting voltage rises between two readings. */
	inline bool OnUsb(double voltage, const State &previous)
	{
		constexpr double USB_V = 4.15;
		constexpr double RISE_V = 0.05; // a cell on battery never rises this much between rounds
		if (voltage >= USB_V)
			return true;
		return previous.lastVoltage > 0 && voltage - previous.lastVoltage >= RISE_V;
	}

	/** Hours left on battery at this state of charge. */
	inline double HoursLeft(double soc, const Model &model, double factor)
	{
		double energy = std::max(0.0, soc - model.emptySoc) * model.capacity_J * model.usable;
		double perHour = model.perHour_J * std::max(0.25, factor);
		return perHour > 0 ? energy / perHour : -1;
	}

	/** Warning level for the hours left, with hysteresis: a better level needs 25% more than its threshold. */
	inline Level LevelFor(double hours, double soc, const Model &model, Level previous)
	{
		if (soc <= model.emptySoc)
			return Level::Empty;
		auto raw = [](double h)
		{
			if (h <= 1)
				return Level::Critical;
			if (h <= 6)
				return Level::Today;
			if (h <= 24)
				return Level::Day;
			return Level::Ok;
		};
		Level level = raw(hours);
		bool previousWasWarning = previous == Level::Day || previous == Level::Today || previous == Level::Critical ||
								  previous == Level::Empty;
		if (previousWasWarning && static_cast<int>(level) < static_cast<int>(previous))
		{
			// Better than before: only when clearly so (a resting voltage that recovers a little is no reason).
			Level withMargin = raw(hours / 1.25);
			if (static_cast<int>(withMargin) > static_cast<int>(level))
				level = withMargin;
			if (previous == Level::Empty && soc <= model.emptySoc * 2)
				level = Level::Empty;
		}
		return level;
	}

	/** Bars for the icon (0..4). */
	inline int BarsFor(double soc)
	{
		if (soc > 0.75)
			return 4;
		if (soc > 0.50)
			return 3;
		if (soc > 0.25)
			return 2;
		if (soc > 0.08)
			return 1;
		return 0;
	}

	/**
	 * Update the state with a resting voltage at time now (UTC s). Returns the reading; sets dirty when the state
	 * should be written to NVS (kept rare to spare the flash: level changes, learning steps, a charged battery).
	 */
	inline Reading Update(State &state, double voltage, int64_t now, const Model &model, bool &dirty)
	{
		dirty = false;
		Reading r;
		r.voltage = voltage;
		r.soc = SocFromVoltage(voltage);
		r.usb = OnUsb(voltage, state);
		r.bars = BarsFor(r.soc);

		if (r.usb)
		{
			r.level = Level::Usb;
			if (state.sampleSoc >= 0 || state.level != Level::Usb)
				dirty = true;
			state.sampleSoc = -1; // learning starts again when the plug is pulled
			state.sampleTime = 0;
		}
		else
		{
			constexpr int64_t LEARN_MIN_S = 2 * 3600;  // at least 2 hours between two points
			constexpr int64_t LEARN_MAX_S = 48 * 3600; // older points say little (charging in between, a long gap)
			constexpr double LEARN_MIN_DROP = 0.03;	   // and at least 3% less charge (the table is coarse)
			if (state.sampleSoc < 0 || now < state.sampleTime || now - state.sampleTime > LEARN_MAX_S ||
				r.soc > state.sampleSoc + 0.02)
			{
				state.sampleTime = now;
				state.sampleSoc = r.soc;
				dirty = true;
			}
			else if (now - state.sampleTime >= LEARN_MIN_S && state.sampleSoc - r.soc >= LEARN_MIN_DROP)
			{
				double hours = (now - state.sampleTime) / 3600.0;
				double used = (state.sampleSoc - r.soc) * model.capacity_J * model.usable / hours;
				double ratio = used / model.perHour_J;
				state.factor = std::min(4.0, std::max(0.5, 0.7 * state.factor + 0.3 * ratio));
				state.sampleTime = now;
				state.sampleSoc = r.soc;
				dirty = true;
			}
			r.hours = HoursLeft(r.soc, model, state.factor);
			r.level = LevelFor(r.hours, r.soc, model, state.level);
		}

		if (r.level != state.level)
			dirty = true;
		state.level = r.level;
		if (std::fabs(voltage - state.lastVoltage) >= 0.02)
		{
			state.lastVoltage = voltage;
			dirty = true;
		}
		return r;
	}

	/** "±18 u" or "±3 dagen", rounded so it does not suggest more precision than there is. */
	inline std::string HoursText(double hours)
	{
		if (hours < 0)
			return "?";
		if (hours >= 48)
			return "±" + std::to_string(static_cast<int>(hours / 24 + 0.5)) + " dagen";
		if (hours >= 2)
			return "±" + std::to_string(static_cast<int>(hours + 0.5)) + " u";
		return "<2 u";
	}

	/** Footer text for a warning (at most 16 characters); empty when there is nothing to say. */
	inline std::string FooterText(const Reading &r)
	{
		switch (r.level)
		{
		case Level::Day:
			return "opladen: " + HoursText(r.hours);
		case Level::Today:
			return "laad nu op: " + HoursText(r.hours);
		default:
			return "";
		}
	}

	/** "4.02,1795000000,0.81,1.20,3" for NVS: lastVoltage, sampleTime, sampleSoc, factor, level. */
	inline std::string Serialize(const State &s)
	{
		char text[96];
		std::snprintf(text, sizeof(text), "%.3f,%lld,%.4f,%.3f,%d", s.lastVoltage, static_cast<long long>(s.sampleTime),
					  s.sampleSoc, s.factor, static_cast<int>(s.level));
		return text;
	}

	/** Reads what Serialize wrote; false (and s unchanged) for anything else. */
	inline bool Parse(const std::string &text, State &s)
	{
		double v, soc, factor;
		long long t;
		int level;
		int used = 0;
		if (std::sscanf(text.c_str(), "%lf,%lld,%lf,%lf,%d%n", &v, &t, &soc, &factor, &level, &used) != 5 ||
			used != static_cast<int>(text.size()))
			return false;
		if (v < 0 || v > 5 || factor < 0.25 || factor > 8 || soc > 1 || level < 0 || level > static_cast<int>(Level::Empty))
			return false;
		s.lastVoltage = v;
		s.sampleTime = t;
		s.sampleSoc = soc;
		s.factor = factor;
		s.level = static_cast<Level>(level);
		return true;
	}
} // namespace BatteryMonitor
