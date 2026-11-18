// Host tests of include/util/upload_policy.hpp and include/util/local_time.hpp.
#include <util/local_time.hpp>
#include <util/upload_policy.hpp>

#include <cstdio>
#include <string>

namespace
{
	int failures = 0;

	void Check(bool ok, const std::string &what)
	{
		if (!ok)
		{
			std::printf("FAIL %s\n", what.c_str());
			failures++;
		}
	}

	std::string Local(time_t utc)
	{
		struct tm local = {};
		LocalTime::Netherlands(utc, local);
		char text[20];
		std::strftime(text, sizeof text, "%Y-%m-%d %H:%M", &local);
		return text;
	}

	// UTC epoch seconds of a date and time, independent of the PC's time zone.
	time_t Utc(int y, unsigned m, unsigned d, int hh, int mm)
	{
		return static_cast<time_t>(LocalTime::DaysFromCivil(y, m, d) * 86400 + hh * 3600 + mm * 60);
	}
} // namespace

int main()
{
	// Upload answers: keep data unless the upload itself is wrong.
	for (int code : {-1, 0, 401, 403, 404, 408, 429, 500, 502, 503})
		Check(UploadPolicy::IsRetryable(code), "retry " + std::to_string(code));
	for (int code : {400, 413, 422})
		Check(!UploadPolicy::IsRetryable(code), "drop " + std::to_string(code));

	// Activation: only 401/403 are refusals; 500 (device not registered yet on API v3) and no connection are not.
	Check(UploadPolicy::IsActivationRefused(401) && UploadPolicy::IsActivationRefused(403), "refused 401/403");
	for (int code : {-1, 0, 404, 500, 502})
		Check(!UploadPolicy::IsActivationRefused(code), "not refused " + std::to_string(code));

	Check(UploadPolicy::ActivationRetryDelay_s(0) == 60, "delay 0");
	Check(UploadPolicy::ActivationRetryDelay_s(1) == 60, "delay 1");
	Check(UploadPolicy::ActivationRetryDelay_s(2) == 120, "delay 2");
	Check(UploadPolicy::ActivationRetryDelay_s(5) == 960, "delay 5");
	Check(UploadPolicy::ActivationRetryDelay_s(6) == 1800, "delay 6");
	Check(UploadPolicy::ActivationRetryDelay_s(1000) == 1800, "delay max");

	// Calendar arithmetic.
	Check(LocalTime::DaysFromCivil(1970, 1, 1) == 0, "epoch");
	Check(LocalTime::DaysFromCivil(2000, 3, 1) == 11017, "2000-03-01");
	Check(Utc(2026, 11, 18, 12, 0) == 1795003200, "2026-11-18 12:00 UTC");

	// Winter: UTC+1. Summer: UTC+2.
	Check(Local(Utc(2026, 11, 18, 13, 10)) == "2026-11-18 14:10", "winter");
	Check(Local(Utc(2026, 7, 1, 12, 0)) == "2026-07-01 14:00", "summer");
	Check(Local(Utc(2026, 12, 31, 23, 30)) == "2027-01-01 00:30", "new year");

	// 2026: summer time from Sunday 29 March 01:00 UTC to Sunday 25 October 01:00 UTC.
	Check(Local(Utc(2026, 3, 29, 0, 59)) == "2026-03-29 01:59", "before spring change");
	Check(Local(Utc(2026, 3, 29, 1, 0)) == "2026-03-29 03:00", "spring change");
	Check(Local(Utc(2026, 10, 25, 0, 59)) == "2026-10-25 02:59", "before autumn change");
	Check(Local(Utc(2026, 10, 25, 1, 0)) == "2026-10-25 02:00", "autumn change");
	// 2027: 28 March and 31 October (October ends on a Sunday).
	Check(Local(Utc(2027, 3, 28, 1, 0)) == "2027-03-28 03:00", "spring 2027");
	Check(Local(Utc(2027, 10, 31, 0, 59)) == "2027-10-31 02:59", "before autumn 2027");
	Check(Local(Utc(2027, 10, 31, 1, 0)) == "2027-10-31 02:00", "autumn 2027");

	// Offset from the server: used until the next change, then its next offset for at most 240 days, then the rule.
	LocalTime::ServerOffset none;
	Check(LocalTime::Offset_s(Utc(2026, 11, 18, 12, 0), none) == 3600, "no server offset: rule");
	LocalTime::ServerOffset s;
	Check(LocalTime::Parse("3600,1806195600,7200", s) && s.valid && s.offset_s == 3600 && s.nextChange == 1806195600 &&
			  s.nextOffset_s == 7200, "parse");
	Check(LocalTime::Serialize(s) == "3600,1806195600,7200", "serialize");
	Check(LocalTime::Offset_s(Utc(2027, 3, 28, 0, 59), s) == 3600, "server: before change");
	Check(LocalTime::Offset_s(Utc(2027, 3, 28, 1, 0), s) == 7200, "server: after change");
	Check(LocalTime::Offset_s(Utc(2028, 1, 15, 12, 0), s) == 3600, "server: too old, rule");
	// The EU ends summer time: the server says winter time, no next change.
	LocalTime::ServerOffset fixed;
	Check(LocalTime::Parse("3600,0,3600", fixed), "parse fixed");
	Check(LocalTime::Offset_s(Utc(2030, 7, 1, 12, 0), fixed) == 3600, "server: no changes any more");
	Check(Local(Utc(2026, 11, 18, 13, 10)) == "2026-11-18 14:10", "local still winter");
	struct tm parts = {};
	LocalTime::Local(Utc(2030, 7, 1, 12, 0), fixed, parts);
	Check(parts.tm_hour == 13, "local with fixed server offset");
	LocalTime::ServerOffset bad;
	Check(!LocalTime::Parse("", bad) && !bad.valid, "parse empty");
	Check(!LocalTime::Parse("3600,1806195600", bad), "parse two values");
	Check(!LocalTime::Parse("3600,1806195600,7200x", bad), "parse trailing");
	Check(!LocalTime::Parse("99999,0,3600", bad), "parse implausible");
	// Screen header date in Dutch, from local time (late evening UTC is already the next day in the Netherlands).
	struct tm day = {};
	LocalTime::Local(Utc(2026, 11, 18, 13, 10), fixed, day);
	Check(LocalTime::DateNl(day) == "wo 18 nov", "date: wednesday 18 november");
	LocalTime::Local(Utc(2026, 12, 31, 23, 30), fixed, day);
	Check(LocalTime::DateNl(day) == "vr 1 jan", "date: new year in local time");
	Check(std::string(LocalTime::WeekdayNl(day)) == "vr", "weekday");
	Check(LocalTime::DateNl(day, false) == "1 jan", "date without weekday");
	// Footer: last upload today, earlier this week, or longer ago (local times).
	struct tm now = {}, sent = {};
	LocalTime::Local(Utc(2026, 11, 18, 13, 10), fixed, now); // wo 18 nov 14:10
	LocalTime::Local(Utc(2026, 11, 18, 13, 0), fixed, sent);
	Check(LocalTime::SentNl(sent, now, true) == "verstuurd 14:00", "sent today");
	LocalTime::Local(Utc(2026, 11, 17, 22, 50), fixed, sent); // di 17 nov 23:50
	Check(LocalTime::SentNl(sent, now, true) == "verstuurd di", "sent yesterday");
	LocalTime::Local(Utc(2026, 11, 11, 9, 0), fixed, sent);
	Check(LocalTime::SentNl(sent, now, true) == "verstuurd 11 nov", "sent a week ago");
	Check(LocalTime::SentNl(sent, now, false) == "verstuurd 11 nov", "today unknown");
	Check(LocalTime::SentNl(sent, now, true).size() <= 16, "footer length");

	if (failures)
	{
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("upload_policy_test: all passed\n");
	return 0;
}
