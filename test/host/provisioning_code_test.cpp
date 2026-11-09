// Host tests for the provisioning QR code v2 (include/util/provisioning_code.hpp).
// The expected strings were computed independently (Python base64.b32encode); the app tests use the same vectors.
#include <util/provisioning_code.hpp>

#include <cstdio>

int main()
{
    int failures = 0;
    const uint8_t mac[3] = {0x3C, 0x4D, 0x5E};
    struct
    {
        uint64_t pop;
        const char *expected;
    } cases[] = {
        {0x0123456789ABCDEFull, "NFH:EENCWPCNLYASGRLHRGV4333G"},
        {3141592653ull, "NFH:EENCWPCNLYAAAAAAXNAOMTJV"}, // a legacy 32-bit DAT
    };
    for (auto &c : cases)
    {
        auto code = ProvisioningCode::Make(0x1A2B, mac, c.pop);
        if (code != c.expected)
        {
            std::printf("FAIL: %s != %s\n", code.c_str(), c.expected);
            failures++;
        }
    }
    const uint8_t rfc[] = {'f', 'o', 'o', 'b', 'a', 'r'}; // RFC 4648 test vector, without padding
    if (ProvisioningCode::Base32(rfc, sizeof(rfc)) != "MZXW6YTBOI")
    {
        std::printf("FAIL: Base32(foobar)\n");
        failures++;
    }
    std::printf(failures ? "%d failure(s)\n" : "all provisioning code tests passed\n", failures);
    return failures ? 1 : 0;
}
