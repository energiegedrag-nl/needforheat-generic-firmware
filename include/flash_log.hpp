#pragma once

/**
 * FlashLog: a power-loss-safe, append-only ring log of records on raw NOR flash.
 *
 * Purpose: keep measurements that the server has not confirmed yet, across reboots and power loss
 * (requirement: at least 24 hours of buffering). Every record is on flash as soon as append() returns;
 * it is only marked consumed after a successful upload.
 *
 * Plain C++17 without ESP-IDF dependencies, so it is unit tested on a PC against a simulated NOR flash
 * with random power cuts (test/host/flash_log_test.cpp). On the device, FlashDevice is implemented on an
 * esp_partition (see secure_upload/persistent_queue.cpp).
 *
 * Layout: the area is a ring of sectors. Each sector starts with a 16-byte header (magic, sequence number,
 * CRC); records follow, each with an 8-byte header and a payload padded to 4 bytes:
 *
 *   state(1) reserved(1) length(2, little endian) crc32(4) payload(length) padding
 *
 * NOR flash can only change bits from 1 to 0 without erasing a whole sector. The state byte uses that:
 *   0xFF  free, or a record whose write was interrupted (length != 0xFFFF): ignored, sector sealed
 *   0xFE  committed (programmed after header and payload are on flash)
 *   0xFC  consumed (uploaded)
 * Each transition clears exactly one bit, so an interrupted transition leaves either the old or the new state.
 * A sector is erased only when it is (re)used, and only after all its records were consumed, unless the ring
 * is full: then the oldest sector is dropped (and counted), like the RAM queue drops the oldest measurements.
 */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace FlashLogDetail
{
    inline uint32_t Crc32(const uint8_t *data, size_t length, uint32_t crc = 0)
    {
        crc = ~crc;
        for (size_t i = 0; i < length; i++)
        {
            crc ^= data[i];
            for (int k = 0; k < 8; k++)
                crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        return ~crc;
    }
} // namespace FlashLogDetail

/** Raw flash as the log sees it. Offsets are relative to the start of the area. */
class FlashDevice
{
public:
    virtual ~FlashDevice() = default;
    virtual size_t Size() const = 0;
    virtual size_t SectorSize() const = 0;
    virtual bool Read(size_t offset, void *buffer, size_t length) = 0;
    /** Program bytes; may only change bits from 1 to 0 (NOR semantics). */
    virtual bool Write(size_t offset, const void *data, size_t length) = 0;
    virtual bool EraseSector(size_t sectorIndex) = 0;
};

class FlashLog
{
public:
    static constexpr uint32_t SECTOR_MAGIC = 0x4C48464E; // "NFHL"
    static constexpr size_t SECTOR_HEADER_SIZE = 16;
    static constexpr size_t RECORD_HEADER_SIZE = 8;
    static constexpr uint8_t STATE_FREE = 0xFF;
    static constexpr uint8_t STATE_COMMITTED = 0xFE;
    static constexpr uint8_t STATE_CONSUMED = 0xFC;

    /** A record read from the log; pass it back to Consume() after a successful upload. */
    struct Record
    {
        size_t offset; // of the record header
        std::vector<uint8_t> data;
    };

    struct Stats
    {
        size_t pending = 0;   // committed, not consumed
        size_t dropped = 0;   // dropped because the ring was full (since Mount)
        size_t corrupted = 0; // skipped because of a CRC mismatch (since Mount)
    };

    explicit FlashLog(FlashDevice &flash) : m_flash(flash) {}

    /** Scan the area and recover the state after a reboot or power loss. Returns false if unusable. */
    bool Mount()
    {
        m_sectorSize = m_flash.SectorSize();
        m_sectorCount = m_sectorSize ? m_flash.Size() / m_sectorSize : 0;
        if (m_sectorCount < 2 || m_sectorSize < SECTOR_HEADER_SIZE + RECORD_HEADER_SIZE + 4)
            return false;
        m_seq.assign(m_sectorCount, 0);
        m_stats = Stats{};
        m_head = NONE;
        uint32_t headSeq = 0;
        for (size_t s = 0; s < m_sectorCount; s++)
        {
            uint32_t seq;
            if (ReadSectorHeader(s, seq))
            {
                m_seq[s] = seq;
                if (m_head == NONE || seq > headSeq)
                {
                    m_head = s;
                    headSeq = seq;
                }
            }
        }
        m_stats.pending = 0;
        for (size_t s = 0; s < m_sectorCount; s++)
            if (m_seq[s])
                m_stats.pending += ScanSector(s, nullptr, nullptr, 0);
        m_appendOffset = m_head == NONE ? m_sectorSize : AppendOffsetOf(m_head);
        m_mounted = true;
        return true;
    }

    /** Largest payload that fits in one record. */
    size_t MaxPayload() const { return m_sectorSize - SECTOR_HEADER_SIZE - RECORD_HEADER_SIZE; }

    /** Append a record; on return it is on flash. Returns false on a write error or a too large record. */
    bool Append(const void *data, size_t length)
    {
        if (!m_mounted || length == 0 || length > MaxPayload() || length > 0xFFFE)
            return false;
        size_t total = Align4(RECORD_HEADER_SIZE + length);
        if (m_head == NONE || m_appendOffset + total > m_sectorSize)
        {
            if (!OpenNextSector())
                return false;
        }
        size_t base = m_head * m_sectorSize + m_appendOffset;
        uint8_t header[RECORD_HEADER_SIZE];
        header[0] = STATE_FREE; // committed later
        header[1] = 0xFF;
        header[2] = static_cast<uint8_t>(length & 0xFF);
        header[3] = static_cast<uint8_t>(length >> 8);
        uint32_t crc = FlashLogDetail::Crc32(static_cast<const uint8_t *>(data), length);
        std::memcpy(&header[4], &crc, 4);
        // Advance first: whatever happens below, this space is not reused before the sector is erased.
        m_appendOffset += total;
        if (!m_flash.Write(base, header, sizeof header) || !m_flash.Write(base + RECORD_HEADER_SIZE, data, length))
            return false;
        uint8_t state = STATE_COMMITTED;
        if (!m_flash.Write(base, &state, 1))
            return false;
        m_stats.pending++;
        return true;
    }

    /** Read up to maxRecords pending records, oldest first. */
    std::vector<Record> Peek(size_t maxRecords)
    {
        std::vector<Record> out;
        if (!m_mounted || m_head == NONE)
            return out;
        for (size_t i = 1; i <= m_sectorCount && out.size() < maxRecords; i++)
        {
            size_t s = (m_head + i) % m_sectorCount; // oldest first, head last
            if (m_seq[s])
                ScanSector(s, &out, nullptr, maxRecords);
        }
        return out;
    }

    /** Mark records consumed; sectors that are then empty (and not the head) are erased. */
    void Consume(const std::vector<Record> &records)
    {
        std::vector<size_t> touched;
        for (const auto &r : records)
        {
            uint8_t state = STATE_CONSUMED;
            if (m_flash.Write(r.offset, &state, 1) && m_stats.pending)
                m_stats.pending--;
            size_t s = r.offset / m_sectorSize;
            if (touched.empty() || touched.back() != s)
                touched.push_back(s);
        }
        for (size_t s : touched)
        {
            if (s == m_head || !m_seq[s])
                continue;
            size_t live = 0;
            ScanSector(s, nullptr, &live, 0);
            if (live == 0 && InvalidateAndErase(s))
                m_seq[s] = 0;
        }
    }

    const Stats &GetStats() const { return m_stats; }
    size_t Capacity() const { return m_sectorCount * m_sectorSize; }

private:
    static constexpr size_t NONE = static_cast<size_t>(-1);
    static size_t Align4(size_t n) { return (n + 3) & ~static_cast<size_t>(3); }

    bool ReadSectorHeader(size_t s, uint32_t &seq)
    {
        uint8_t h[SECTOR_HEADER_SIZE];
        if (!m_flash.Read(s * m_sectorSize, h, sizeof h))
            return false;
        uint32_t magic, crc;
        std::memcpy(&magic, &h[0], 4);
        std::memcpy(&seq, &h[4], 4);
        std::memcpy(&crc, &h[8], 4);
        return magic == SECTOR_MAGIC && seq != 0 && seq != 0xFFFFFFFF && crc == FlashLogDetail::Crc32(h, 8);
    }

    /**
     * Walk the records of sector s. Collects pending records into out (if given, up to max), counts pending
     * records into live (if given). Returns the number of pending records seen.
     */
    size_t ScanSector(size_t s, std::vector<Record> *out, size_t *live, size_t max)
    {
        size_t pending = 0;
        size_t offset = SECTOR_HEADER_SIZE;
        std::vector<uint8_t> payload;
        while (offset + RECORD_HEADER_SIZE <= m_sectorSize)
        {
            size_t base = s * m_sectorSize + offset;
            uint8_t header[RECORD_HEADER_SIZE];
            if (!m_flash.Read(base, header, sizeof header))
                break;
            size_t length = header[2] | (header[3] << 8);
            if (header[0] == STATE_FREE && length == 0xFFFF)
                break; // free space
            if (length == 0 || length > MaxPayload() || offset + Align4(RECORD_HEADER_SIZE + length) > m_sectorSize)
                break; // garbage: the sector ends here
            if (header[0] == STATE_COMMITTED)
            {
                payload.resize(length);
                uint32_t crc;
                std::memcpy(&crc, &header[4], 4);
                if (!m_flash.Read(base + RECORD_HEADER_SIZE, payload.data(), length) ||
                    FlashLogDetail::Crc32(payload.data(), length) != crc)
                {
                    m_stats.corrupted++;
                }
                else
                {
                    pending++;
                    if (out && out->size() < max)
                        out->push_back(Record{base, payload});
                }
            }
            else if (header[0] != STATE_CONSUMED)
            {
                break; // interrupted write (0xFF with a length) or unknown state: the sector ends here
            }
            offset += Align4(RECORD_HEADER_SIZE + length);
        }
        if (live)
            *live = pending;
        return pending;
    }

    /** First free offset in sector s, or the sector size if it is full or sealed by an interrupted write. */
    size_t AppendOffsetOf(size_t s)
    {
        size_t offset = SECTOR_HEADER_SIZE;
        while (offset + RECORD_HEADER_SIZE <= m_sectorSize)
        {
            uint8_t header[RECORD_HEADER_SIZE];
            if (!m_flash.Read(s * m_sectorSize + offset, header, sizeof header))
                return m_sectorSize;
            size_t length = header[2] | (header[3] << 8);
            if (header[0] == STATE_FREE && length == 0xFFFF)
            {
                // Free only if the rest of the header is erased too.
                for (uint8_t b : header)
                    if (b != 0xFF)
                        return m_sectorSize;
                return offset;
            }
            if (header[0] == STATE_FREE || length == 0 || length > MaxPayload())
                return m_sectorSize; // interrupted: do not append here
            offset += Align4(RECORD_HEADER_SIZE + length);
        }
        return m_sectorSize;
    }

    /**
     * Zero the sector header before erasing: an interrupted erase sets bits back to 1 at random, which could
     * otherwise revive a valid-looking header with consumed records turned "committed" again. A zeroed-then-
     * partly-erased header practically never matches magic and CRC, so such a sector counts as free.
     */
    bool InvalidateAndErase(size_t s)
    {
        uint8_t zero[SECTOR_HEADER_SIZE] = {};
        m_flash.Write(s * m_sectorSize, zero, sizeof zero);
        return m_flash.EraseSector(s);
    }

    bool OpenNextSector()
    {
        size_t next = m_head == NONE ? 0 : (m_head + 1) % m_sectorCount;
        uint32_t seq = m_head == NONE ? 1 : m_seq[m_head] + 1;
        if (m_seq[next])
        {
            size_t live = 0;
            ScanSector(next, nullptr, &live, 0);
            m_stats.dropped += live;
            m_stats.pending -= live < m_stats.pending ? live : m_stats.pending;
        }
        m_seq[next] = 0;
        if (!InvalidateAndErase(next))
            return false;
        uint8_t h[SECTOR_HEADER_SIZE];
        std::memset(h, 0xFF, sizeof h);
        uint32_t magic = SECTOR_MAGIC;
        std::memcpy(&h[0], &magic, 4);
        std::memcpy(&h[4], &seq, 4);
        uint32_t crc = FlashLogDetail::Crc32(h, 8);
        std::memcpy(&h[8], &crc, 4);
        m_head = next;
        m_appendOffset = m_sectorSize; // if the header write fails, the next Append opens another sector
        if (!m_flash.Write(next * m_sectorSize, h, sizeof h))
            return false;
        m_seq[next] = seq;
        m_appendOffset = SECTOR_HEADER_SIZE;
        return true;
    }

    FlashDevice &m_flash;
    bool m_mounted = false;
    size_t m_sectorSize = 0;
    size_t m_sectorCount = 0;
    std::vector<uint32_t> m_seq; // 0 = free sector
    size_t m_head = NONE;
    size_t m_appendOffset = 0;
    Stats m_stats;
};
