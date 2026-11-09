#pragma once

#include <vector>

#include <flash_log.hpp>
#include <measurements.hpp>

namespace SecureUpload
{
    /**
     * Measurements that wait for upload, kept on flash so they survive reboots and power loss.
     *
     * Uses the data partition labelled "nfhbuf" (any subtype) if the device firmware's partition table has one;
     * without it, Available() is false and the upload queue keeps measurements in RAM as before.
     * Example partition line (128 KiB = 32 sectors; about 3 days of a living room module at ~40 KiB a day):
     *   nfhbuf,   data, 0x40,    0x3e0000, 0x20000,   (with OTA slots of 0x1e0000)
     */
    class PersistentStore
    {
    public:
        static PersistentStore &GetInstance();

        bool Available() const { return m_available; }

        /** Store a measurement; returns false if it could not be written (the caller keeps it in RAM then). */
        bool Add(const Measurements::Measurement &measurement);

        /** Up to max measurements, oldest first, with their records for Consume(). */
        size_t Peek(size_t max, std::vector<FlashLog::Record> &records, std::vector<Measurements::Measurement> &measurements);

        /** Mark records as uploaded (or as not worth retrying). */
        void Consume(const std::vector<FlashLog::Record> &records);

        size_t Pending();

    private:
        PersistentStore();
        bool m_available = false;
    };
} // namespace SecureUpload
