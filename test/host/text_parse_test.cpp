// Host tests of include/util/text_parse.hpp and SemanticVersion: the parsers replace std::regex, so the cases
// below are the results of the old regular expressions (ECMAScript, as std::regex uses them).
#include <util/semantic_version.hpp>
#include <util/text_parse.hpp>

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

	// Old: v?(\d+)\.(\d+)\.(\d+)-?(.*) with regex_search.
	void Version(const std::string &text, bool found, unsigned long major = 0, unsigned long minor = 0,
				 unsigned long patch = 0, const std::string &extra = "")
	{
		unsigned long a = 99, b = 99, c = 99;
		std::string e = "unchanged";
		bool ok = TextParse::ParseVersion(text, a, b, c, e);
		Check(ok == found, "version found: " + text);
		if (ok && found)
			Check(a == major && b == minor && c == patch && e == extra,
				  "version " + text + " gives " + std::to_string(a) + "." + std::to_string(b) + "." +
					  std::to_string(c) + " '" + e + "'");
	}

	// Old: ^([0-9a-fA-F]{2})[:-]...{2})$ with regex_search.
	void Mac(const std::string &text, bool valid, const uint8_t expected[6] = nullptr)
	{
		uint8_t octets[6] = {1, 2, 3, 4, 5, 6};
		bool ok = TextParse::ParseMacAddress(text, octets);
		Check(ok == valid, "MAC valid: " + text);
		if (ok && valid)
			for (int i = 0; i < 6; i++)
				Check(octets[i] == expected[i], "MAC octet " + std::to_string(i) + ": " + text);
		if (!valid)
			Check(octets[0] == 1 && octets[5] == 6, "MAC unchanged when invalid: " + text);
	}

	// Old: "tag_name":"([^"]+)" with regex_search.
	void Tag(const std::string &json, bool found, const std::string &expected = "")
	{
		std::string value;
		bool ok = TextParse::FindJsonString(json, "tag_name", value);
		Check(ok == found, "tag found: " + json);
		if (ok && found)
			Check(value == expected, "tag " + json + " gives " + value);
	}
} // namespace

int main()
{
	Version("v3.5.0", true, 3, 5, 0, "");
	Version("3.5.0-rc1", true, 3, 5, 0, "rc1");
	Version("v1.5.23", true, 1, 5, 23, "");
	Version("release v12.0.7-beta.2", true, 12, 0, 7, "beta.2");
	Version("1.2", false);
	Version("x1.2.x3.4.5", true, 3, 4, 5, "");
	Version("v3.5.0\nfoo", true, 3, 5, 0, "");
	Version("1.12.3.4", true, 1, 12, 3, ".4");
	Version("", false);

	const uint8_t mac[6] = {0xAB, 0xFF, 0x0C, 0xFC, 0xBA, 0x5C};
	Mac("AB:FF:0C:FC:BA:5c", true, mac);
	Mac("ab-ff-0c-fc-ba-5c", true, mac);
	Mac("AB:FF:0C:FC:BA-5C", true, mac);
	Mac("AB:FF:0C:FC:BA:5", false);
	Mac("AB:FF:0C:FC:BA:5CX", false);
	Mac("AB.FF:0C:FC:BA:5C", false);
	Mac("AB:FF:0C:FC:BA:5C\n", false);
	Mac("AB:FF:0C:FC:BA:5G", false);

	Tag("{\"url\":\"x\",\"tag_name\":\"v3.5.0\",\"name\":\"y\"}", true, "v3.5.0");
	Tag("{\"tag_name\":\"\",\"x\":{\"tag_name\":\"v2.0.0\"}}", true, "v2.0.0");
	Tag("{\"name\":\"x\"}", false);
	Tag("{\"tag_name\": \"v1.0.0\"}", false);
	Tag("{\"tag_name\":\"v1.0.0", false);

	// SemanticVersion on top of ParseVersion.
	Check(SemanticVersion("v3.5.0").String() == "v3.5.0", "SemanticVersion v3.5.0");
	Check(SemanticVersion("3.5.0-rc1").String() == "v3.5.0-rc1", "SemanticVersion 3.5.0-rc1");
	Check(SemanticVersion("nonsense").String() == "v0.0.0", "SemanticVersion nonsense");
	Check(SemanticVersion("v3.10.0") > SemanticVersion("v3.9.7"), "SemanticVersion 3.10.0 > 3.9.7");
	Check(SemanticVersion("v4.0.0") > SemanticVersion("v3.99.99"), "SemanticVersion 4.0.0 > 3.99.99");
	Check(SemanticVersion("v3.5.0") == SemanticVersion("3.5.0-rc1"), "SemanticVersion ignores the extra part");
	SemanticVersion version("v1.5.23-rc1");
	Check(std::string(version.CString()) == "v1.5.23-rc1", "SemanticVersion CString");

	if (failures == 0)
		std::printf("text parse tests passed\n");
	return failures == 0 ? 0 : 1;
}
