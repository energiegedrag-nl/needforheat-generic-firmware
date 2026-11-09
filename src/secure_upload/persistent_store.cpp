#include <persistent_store.hpp>

#include <esp_log.h>
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

constexpr const char *TAG = "Persistent store";
constexpr const char *PARTITION_LABEL = "nfhbuf";

namespace
{
    /** FlashDevice on an ESP-IDF data partition. */
    class PartitionFlash : public FlashDevice
    {
    public:
        explicit PartitionFlash(const esp_partition_t *partition) : m_partition(partition) {}
        size_t Size() const override { return m_partition->size; }
        size_t SectorSize() const override { return SPI_FLASH_SEC_SIZE; }
        bool Read(size_t offset, void *buffer, size_t length) override
        {
            return esp_partition_read(m_partition, offset, buffer, length) == ESP_OK;
        }
        bool Write(size_t offset, const void *data, size_t length) override
        {
            return esp_partition_write(m_partition, offset, data, length) == ESP_OK;
        }
        bool EraseSector(size_t sectorIndex) override
        {
            return esp_partition_erase_range(m_partition, sectorIndex * SPI_FLASH_SEC_SIZE, SPI_FLASH_SEC_SIZE) == ESP_OK;
        }

    private:
        const esp_partition_t *m_partition;
    };

    PartitionFlash *s_flash = nullptr;
    FlashLog *s_log = nullptr;
    SemaphoreHandle_t s_mutex = nullptr;

    struct Lock
    {
        Lock() { xSemaphoreTake(s_mutex, portMAX_DELAY); }
        ~Lock() { xSemaphoreGive(s_mutex); }
    };
} // namespace

namespace SecureUpload
{
    PersistentStore &PersistentStore::GetInstance()
    {
        static PersistentStore store;
        return store;
    }

    PersistentStore::PersistentStore()
    {
        auto partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, PARTITION_LABEL);
        if (partition == nullptr)
        {
            ESP_LOGI(TAG, "No \"%s\" partition: measurements are buffered in RAM only.", PARTITION_LABEL);
            return;
        }
        s_mutex = xSemaphoreCreateMutex();
        s_flash = new PartitionFlash(partition);
        s_log = new FlashLog(*s_flash);
        if (!s_log->Mount())
        {
            ESP_LOGE(TAG, "Partition \"%s\" is too small; buffering in RAM only.", PARTITION_LABEL);
            return;
        }
        m_available = true;
        ESP_LOGI(TAG, "Persistent buffer: %u KiB, %u measurements waiting from before this start.",
                 static_cast<unsigned>(s_log->Capacity() / 1024), static_cast<unsigned>(s_log->GetStats().pending));
    }

    bool PersistentStore::Add(const Measurements::Measurement &measurement)
    {
        if (!m_available)
            return false;
        auto data = measurement.Serialize();
        Lock lock;
        size_t droppedBefore = s_log->GetStats().dropped;
        bool ok = s_log->Append(data.data(), data.size());
        if (s_log->GetStats().dropped != droppedBefore)
            ESP_LOGW(TAG, "Buffer full: %u oldest measurements dropped.",
                     static_cast<unsigned>(s_log->GetStats().dropped - droppedBefore));
        return ok;
    }

    size_t PersistentStore::Peek(size_t max, std::vector<FlashLog::Record> &records,
                                 std::vector<Measurements::Measurement> &measurements)
    {
        records.clear();
        measurements.clear();
        if (!m_available)
            return 0;
        std::vector<FlashLog::Record> raw;
        {
            Lock lock;
            raw = s_log->Peek(max);
        }
        std::vector<FlashLog::Record> unreadable;
        for (auto &r : raw)
        {
            Measurements::Measurement m;
            if (Measurements::Measurement::Deserialize(r.data.data(), r.data.size(), m))
            {
                measurements.push_back(std::move(m));
                records.push_back(std::move(r));
            }
            else
            {
                unreadable.push_back(std::move(r));
            }
        }
        if (!unreadable.empty())
        {
            ESP_LOGE(TAG, "%u unreadable records skipped.", static_cast<unsigned>(unreadable.size()));
            Consume(unreadable);
        }
        return measurements.size();
    }

    void PersistentStore::Consume(const std::vector<FlashLog::Record> &records)
    {
        if (!m_available || records.empty())
            return;
        Lock lock;
        s_log->Consume(records);
    }

    size_t PersistentStore::Pending()
    {
        if (!m_available)
            return 0;
        Lock lock;
        return s_log->GetStats().pending;
    }
} // namespace SecureUpload
