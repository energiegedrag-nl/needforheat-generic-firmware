// Host unit tests for FlashLog (include/flash_log.hpp) against a simulated NOR flash with power cuts.
// Build and run:  g++ -std=c++17 -O2 -Wall -Wextra -I include test/host/flash_log_test.cpp -o flash_log_test && ./flash_log_test

#include <flash_log.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <set>
#include <stdexcept>
#include <string>

static int g_failures = 0;
#define CHECK(cond, ...)                                     \
    do                                                       \
    {                                                        \
        if (!(cond))                                         \
        {                                                    \
            std::printf("FAIL %s:%d: %s  ", __FILE__, __LINE__, #cond); \
            std::printf(__VA_ARGS__);                        \
            std::printf("\n");                               \
            g_failures++;                                    \
        }                                                    \
    } while (0)

struct PowerCut : std::runtime_error
{
    PowerCut() : std::runtime_error("power cut") {}
};

/** NOR flash: erased bytes are 0xFF, programming can only clear bits; optional power cut after N operations. */
class SimFlash : public FlashDevice
{
public:
    SimFlash(size_t sectors, size_t sectorSize) : m_mem(sectors * sectorSize, 0xFF), m_sectorSize(sectorSize) {}
    size_t Size() const override { return m_mem.size(); }
    size_t SectorSize() const override { return m_sectorSize; }
    bool Read(size_t offset, void *buffer, size_t length) override
    {
        if (offset + length > m_mem.size())
            return false;
        std::memcpy(buffer, &m_mem[offset], length);
        return true;
    }
    bool Write(size_t offset, const void *data, size_t length) override
    {
        if (offset + length > m_mem.size())
            return false;
        auto *d = static_cast<const uint8_t *>(data);
        for (size_t i = 0; i < length; i++)
        {
            Tick();
            if (m_cutNow)
            {
                // The byte being written may be half-programmed: some of its bits cleared.
                m_mem[offset + i] &= static_cast<uint8_t>(d[i] | (m_rng() & 0xFF));
                throw PowerCut();
            }
            m_mem[offset + i] &= d[i];
        }
        return true;
    }
    bool EraseSector(size_t s) override
    {
        Tick();
        if (m_cutNow)
        {
            // Interrupted erase: random bits set back to 1, the rest unchanged.
            for (size_t i = 0; i < m_sectorSize; i++)
                m_mem[s * m_sectorSize + i] |= static_cast<uint8_t>(m_rng() & m_rng() & 0xFF);
            throw PowerCut();
        }
        std::fill(m_mem.begin() + s * m_sectorSize, m_mem.begin() + (s + 1) * m_sectorSize, 0xFF);
        return true;
    }
    /** Cut power after `ops` more byte writes / erases (0 = never). */
    void ArmCut(size_t ops) { m_budget = ops; m_cutNow = false; }
    void Disarm() { m_budget = 0; m_cutNow = false; }
    std::mt19937 m_rng{12345};

private:
    void Tick()
    {
        if (m_budget && --m_budget == 0)
            m_cutNow = true;
    }
    std::vector<uint8_t> m_mem;
    size_t m_sectorSize;
    size_t m_budget = 0;
    bool m_cutNow = false;
};

static std::vector<uint8_t> Payload(uint32_t id)
{
    std::mt19937 r(id);
    std::vector<uint8_t> p(4 + r() % 60);
    std::memcpy(p.data(), &id, 4);
    for (size_t i = 4; i < p.size(); i++)
        p[i] = static_cast<uint8_t>(r());
    return p;
}

static uint32_t IdOf(const FlashLog::Record &r, bool &ok)
{
    uint32_t id = 0;
    ok = r.data.size() >= 4;
    if (ok)
        std::memcpy(&id, r.data.data(), 4);
    ok = ok && r.data == Payload(id);
    return id;
}

static void TestBasics()
{
    SimFlash flash(8, 4096);
    FlashLog log(flash);
    CHECK(log.Mount(), "mount empty");
    CHECK(log.GetStats().pending == 0, "empty");
    for (uint32_t id = 1; id <= 100; id++)
        CHECK(log.Append(Payload(id).data(), Payload(id).size()), "append %u", id);
    CHECK(log.GetStats().pending == 100, "pending %zu", log.GetStats().pending);
    auto batch = log.Peek(30);
    CHECK(batch.size() == 30, "peek %zu", batch.size());
    bool ok;
    for (size_t i = 0; i < batch.size(); i++)
        CHECK(IdOf(batch[i], ok) == i + 1 && ok, "order at %zu", i);
    log.Consume(batch);
    // Reboot: state recovered from flash only.
    FlashLog again(flash);
    CHECK(again.Mount(), "remount");
    CHECK(again.GetStats().pending == 70, "pending after reboot %zu", again.GetStats().pending);
    auto rest = again.Peek(1000);
    CHECK(rest.size() == 70 && IdOf(rest.front(), ok) == 31 && IdOf(rest.back(), ok) == 100, "rest");
    again.Consume(rest);
    CHECK(again.GetStats().pending == 0, "all consumed");
    CHECK(!again.Append("", 0), "empty record rejected");
    std::vector<uint8_t> big(again.MaxPayload() + 1, 1);
    CHECK(!again.Append(big.data(), big.size()), "too large rejected");
}

static void TestOverflowDropsOldest()
{
    SimFlash flash(4, 1024);
    FlashLog log(flash);
    log.Mount();
    uint32_t id = 0;
    for (; id < 2000; id++)
        log.Append(Payload(id + 1).data(), Payload(id + 1).size());
    auto all = log.Peek(100000);
    bool ok = true, okAll = true;
    uint32_t prev = 0;
    for (auto &r : all)
    {
        uint32_t i = IdOf(r, ok);
        okAll = okAll && ok && i > prev;
        prev = i;
    }
    CHECK(okAll, "ordered, intact");
    CHECK(prev == 2000, "newest kept (last %u)", prev);
    CHECK(log.GetStats().dropped > 0 && log.GetStats().pending == all.size(), "dropped %zu pending %zu",
          log.GetStats().dropped, log.GetStats().pending);
}

/** Random appends and consumes, with power cuts at random moments, many times on the same flash. */
static void TestPowerCuts(unsigned seed, int rounds)
{
    SimFlash flash(16, 4096); // 64 KiB; the test volume never fills the ring, so nothing is dropped
    flash.m_rng.seed(seed);
    std::mt19937 rng(seed);
    std::vector<uint32_t> pending; // acknowledged and not consumed, in order
    std::set<uint32_t> consumedDone; // Consume() returned for these
    std::set<uint32_t> inflightConsume; // a Consume() for these was interrupted: may be gone or not
    uint32_t inflightAppend = 0;         // an Append() for this id was interrupted: may be there or not
    uint32_t nextId = 1;
    size_t cuts = 0;
    for (int round = 0; round < rounds; round++)
    {
        FlashLog log(flash);
        flash.Disarm();
        CHECK(log.Mount(), "mount round %d", round);
        auto seen = log.Peek(1000000);
        std::vector<uint32_t> ids;
        bool ok = true;
        for (auto &r : seen)
        {
            ids.push_back(IdOf(r, ok));
            CHECK(ok, "intact payload, round %d", round);
        }
        CHECK(std::is_sorted(ids.begin(), ids.end()), "order, round %d", round);
        std::set<uint32_t> present(ids.begin(), ids.end());
        for (uint32_t id : pending)
            CHECK(present.count(id) || inflightConsume.count(id), "lost acknowledged record %u, round %d", id, round);
        std::set<uint32_t> expected(pending.begin(), pending.end());
        for (uint32_t id : ids)
        {
            CHECK(!consumedDone.count(id), "consumed record %u came back, round %d", id, round);
            CHECK(expected.count(id) || id == inflightAppend, "unexpected record %u, round %d", id, round);
        }
        pending = ids; // resynchronise with what is really there
        inflightConsume.clear();
        inflightAppend = 0;

        flash.ArmCut(1 + rng() % 3000);
        try
        {
            for (int op = 0; op < 400; op++)
            {
                if (rng() % 10 < 7)
                {
                    uint32_t id = nextId++;
                    auto p = Payload(id);
                    inflightAppend = id;
                    if (log.Append(p.data(), p.size()))
                        pending.push_back(id);
                    inflightAppend = 0;
                }
                else
                {
                    auto batch = log.Peek(1 + rng() % 40);
                    bool okc;
                    for (auto &r : batch)
                        inflightConsume.insert(IdOf(r, okc));
                    log.Consume(batch);
                    for (uint32_t id : inflightConsume)
                    {
                        consumedDone.insert(id);
                        pending.erase(std::remove(pending.begin(), pending.end(), id), pending.end());
                    }
                    inflightConsume.clear();
                }
            }
            flash.Disarm();
        }
        catch (const PowerCut &)
        {
            cuts++;
        }
    }
    std::printf("power-cut test seed %u: %d rounds, %zu cuts\n", seed, rounds, cuts);
}

int main()
{
    TestBasics();
    TestOverflowDropsOldest();
    for (unsigned seed = 1; seed <= 20; seed++)
        TestPowerCuts(seed, 300);
    if (g_failures)
    {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all FlashLog tests passed\n");
    return 0;
}
