#pragma once

#include <cstdint>
#include <ctime>

/**
 * Dutch local time (CET/CEST) for what the screen shows. The system clock and the RTC stay in UTC (TZ=UTC), as do
 * all timestamps that are uploaded; only texts for people use this. Header-only and without ESP-IDF, so
 * test/host/local_time_test.cpp checks it on the PC.
 *
 * EU rule (since 1996): summer time from the last Sunday of March 01:00 UTC to the last Sunday of October 01:00 UTC.
 */
namespace LocalTime
{
	/** Days since 1970-01-01 of a date in the proleptic Gregorian calendar (H. Hinnant's days_from_civil). */
	inline int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d)
	{
		y -= m <= 2;
		const int64_t era = (y >= 0 ? y : y - 399) / 400;
		const unsigned yoe = static_cast<unsigned>(y - era * 400);
		const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
		const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
		return era * 146097 + static_cast<int64_t>(doe) - 719468;
	}

	/** Date of a day number since 1970-01-01 (H. Hinnant's civil_from_days). */
	inline void CivilFromDays(int64_t z, int64_t &y, unsigned &m, unsigned &d)
	{
		z += 719468;
		const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
		const unsigned doe = static_cast<unsigned>(z - era * 146097);
		const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
		const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
		const unsigned mp = (5 * doy + 2) / 153;
		d = doy - (153 * mp + 2) / 5 + 1;
		m = mp < 10 ? mp + 3 : mp - 9;
		y = static_cast<int64_t>(yoe) + era * 400 + (m <= 2);
	}

	/** Floor division of seconds into days (also before 1970). */
	inline int64_t DayOf(int64_t t) { return t >= 0 ? t / 86400 : -((-t + 86399) / 86400); }

	/** Broken-down time (as gmtime would give) of a time that is already shifted to the wanted zone. */
	inline void BreakDown(int64_t t, struct tm &parts)
	{
		int64_t day = DayOf(t);
		int64_t second = t - day * 86400;
		int64_t y;
		unsigned m, d;
		CivilFromDays(day, y, m, d);
		parts = {};
		parts.tm_year = static_cast<int>(y - 1900);
		parts.tm_mon = static_cast<int>(m) - 1;
		parts.tm_mday = static_cast<int>(d);
		parts.tm_hour = static_cast<int>(second / 3600);
		parts.tm_min = static_cast<int>(second / 60 % 60);
		parts.tm_sec = static_cast<int>(second % 60);
		parts.tm_wday = static_cast<int>(((day % 7) + 7 + 4) % 7);
		parts.tm_yday = static_cast<int>(day - DaysFromCivil(y, 1, 1));
	}

	/** UTC time of 01:00 on the last Sunday of month m (31 days: March or October) of year y. */
	inline int64_t LastSunday0100Utc(int64_t y, unsigned m)
	{
		int64_t days = DaysFromCivil(y, m, 31);
		int64_t weekday = ((days % 7) + 7 + 4) % 7; // 1970-01-01 was a Thursday; 0 = Sunday
		return (days - weekday) * 86400 + 3600;
	}

	/** Offset of Dutch local time from UTC in seconds at the given UTC time: 3600 (winter) or 7200 (summer). */
	inline int OffsetNetherlands_s(time_t utc)
	{
		int64_t t = static_cast<int64_t>(utc);
		int64_t year;
		unsigned m, d;
		CivilFromDays(DayOf(t), year, m, d);
		bool summer = t >= LastSunday0100Utc(year, 3) && t < LastSunday0100Utc(year, 10);
		return summer ? 7200 : 3600;
	}

	/** Broken-down Dutch local time, for strftime. */
	inline void Netherlands(time_t utc, struct tm &local)
	{
		BreakDown(static_cast<int64_t>(utc) + OffsetNetherlands_s(utc), local);
	}
} // namespace LocalTime
