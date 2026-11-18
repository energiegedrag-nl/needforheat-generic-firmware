#pragma once

#include <cstdint>
#include <string>

/**
 * Small text parsers that replace std::regex: std::regex pulls the regex compiler and the C++ locale code into the
 * firmware (tens of KB of flash), and the CoreInk app partition is nearly full. Header-only and without ESP-IDF, so
 * test/host/text_parse_test.cpp checks them on the PC.
 */
namespace TextParse
{
	inline bool IsDigit(char c) { return c >= '0' && c <= '9'; }

	inline int HexValue(char c)
	{
		if (c >= '0' && c <= '9')
			return c - '0';
		if (c >= 'a' && c <= 'f')
			return c - 'a' + 10;
		if (c >= 'A' && c <= 'F')
			return c - 'A' + 10;
		return -1;
	}

	/**
	 * Parses a MAC address "AB:FF:0C:FC:BA:5C" or "ab-ff-0c-fc-ba-5c" (the whole string, two hex digits per octet,
	 * ':' or '-' between them). Returns false and leaves octets unchanged when the text is not such an address.
	 */
	inline bool ParseMacAddress(const std::string &text, uint8_t octets[6])
	{
		if (text.size() != 17)
			return false;
		uint8_t parsed[6];
		for (int i = 0; i < 6; i++)
		{
			int high = HexValue(text[i * 3]);
			int low = HexValue(text[i * 3 + 1]);
			if (high < 0 || low < 0)
				return false;
			if (i < 5 && text[i * 3 + 2] != ':' && text[i * 3 + 2] != '-')
				return false;
			parsed[i] = static_cast<uint8_t>(high * 16 + low);
		}
		for (int i = 0; i < 6; i++)
			octets[i] = parsed[i];
		return true;
	}

	/**
	 * Finds the first non-empty string value of a key in compact JSON text, e.g. "tag_name":"v3.5.0" (no spaces
	 * around the colon, no escaped quotes in the value). Returns false when there is none.
	 */
	inline bool FindJsonString(const std::string &json, const std::string &key, std::string &value)
	{
		const std::string start = "\"" + key + "\":\"";
		size_t from = 0;
		while ((from = json.find(start, from)) != std::string::npos)
		{
			size_t begin = from + start.size();
			size_t end = json.find('"', begin);
			if (end == std::string::npos)
				return false;
			if (end > begin)
			{
				value = json.substr(begin, end - begin);
				return true;
			}
			from = begin;
		}
		return false;
	}

	/**
	 * Finds the first "major.minor.patch" in the text (an optional "v" before it is allowed) and returns the
	 * numbers and the rest after it, without one leading '-': "v1.5.23-rc1" gives 1, 5, 23, "rc1".
	 * Returns false when there is no such version in the text.
	 */
	inline bool ParseVersion(const std::string &text, unsigned long &major, unsigned long &minor, unsigned long &patch,
							 std::string &extra)
	{
		for (size_t i = 0; i < text.size(); i++)
		{
			unsigned long numbers[3] = {0, 0, 0};
			size_t pos = i;
			bool ok = true;
			for (int n = 0; n < 3 && ok; n++)
			{
				if (n > 0)
				{
					if (pos >= text.size() || text[pos] != '.')
					{
						ok = false;
						break;
					}
					pos++;
				}
				size_t digits = pos;
				while (pos < text.size() && IsDigit(text[pos]))
					numbers[n] = numbers[n] * 10 + static_cast<unsigned long>(text[pos++] - '0');
				ok = pos > digits;
			}
			if (!ok)
				continue;
			if (pos < text.size() && text[pos] == '-')
				pos++;
			major = numbers[0];
			minor = numbers[1];
			patch = numbers[2];
			extra = text.substr(pos, text.find_first_of("\r\n", pos) - pos); // up to the end of the line
			return true;
		}
		return false;
	}
} // namespace TextParse
