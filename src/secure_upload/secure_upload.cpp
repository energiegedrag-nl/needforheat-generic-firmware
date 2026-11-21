#include <secure_upload.hpp>

#include <esp_log.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <cJSON.h>

#include <cstdlib>
#include <ctime>
#include <vector>

#include <generic_esp_32.hpp>
#include <persistent_store.hpp>
#include <util/upload_policy.hpp>

constexpr const char *TAG = "Secure upload";

constexpr int UPLOAD_QUEUE_MAX = 50;
// Measurements per upload from the persistent buffer, and batches per upload moment (to catch up after an outage).
constexpr size_t PERSISTENT_BATCH = 100;
constexpr int PERSISTENT_BATCHES_PER_UPLOAD = 10;

namespace SecureUpload
{
	Queue &Queue::GetInstance()
	{
		static Queue q;
		return q;
	}

	Queue::Queue()
		: m_measurementQueue(xQueueCreate(UPLOAD_QUEUE_MAX, sizeof(Measurements::Measurement *)))
	{
		ESP_LOGD(TAG, "Queue created.");
	}

	namespace
	{
		/** Time of the last successful upload in this boot (0: none yet). */
		time_t s_lastSuccessfulUpload = 0;
		volatile int s_lastStatusCode = 0;

		/**
		 * A timestamp is plausible if its year lies between the firmware build year - 1 and + 10.
		 * Measurements taken before the first NTP sync (1970) or with a garbage RTC value (e.g. 2036)
		 * are not uploaded: they would end up as wrong data in the analysis.
		 */
		bool IsPlausibleTime(time_t timestamp)
		{
			static const int buildYear = std::atoi(&__DATE__[7]); // __DATE__ = "Mmm dd yyyy"
			struct tm utc = {};
			gmtime_r(&timestamp, &utc);
			int year = utc.tm_year + 1900;
			return year >= buildYear - 1 && year <= buildYear + 10;
		}

		/**
		 * Take the local time offset from an upload response (API: utc_offset_s, next_offset_change,
		 * next_utc_offset_s), for the time on the screen. Older servers do not send it: nothing changes then.
		 */
		void TakeDisplayOffset(const HTTPUtil::buffer_t &response)
		{
			auto json = cJSON_Parse(response.c_str());
			if (json == nullptr)
				return;
			auto offset = cJSON_GetObjectItem(json, "utc_offset_s");
			auto nextChange = cJSON_GetObjectItem(json, "next_offset_change");
			auto nextOffset = cJSON_GetObjectItem(json, "next_utc_offset_s");
			if (cJSON_IsNumber(offset) && LocalTime::PlausibleOffset(static_cast<int64_t>(offset->valuedouble)))
			{
				LocalTime::ServerOffset s;
				s.valid = true;
				s.offset_s = static_cast<int32_t>(offset->valuedouble);
				s.nextOffset_s = s.offset_s;
				if (cJSON_IsNumber(nextChange) && cJSON_IsNumber(nextOffset) && nextChange->valuedouble > 0 &&
					LocalTime::PlausibleOffset(static_cast<int64_t>(nextOffset->valuedouble)))
				{
					s.nextChange = static_cast<int64_t>(nextChange->valuedouble);
					s.nextOffset_s = static_cast<int32_t>(nextOffset->valuedouble);
				}
				GenericESP32Firmware::SetDisplayOffset(s);
			}
			cJSON_Delete(json);
		}

		/**
		 * The server answered 410: the researcher released this device for another participant. Drop the persistent
		 * buffer (those measurements would land with nobody), forget the pairing and Wi-Fi and restart: the device then
		 * shows the provisioning QR code. See UploadPolicy::IsReleased.
		 */
		[[noreturn]] void ForgetPairing()
		{
			ESP_LOGW(TAG, "Device released for another participant (410): forgetting pairing and buffered measurements.");
			auto &store = PersistentStore::GetInstance();
			if (store.Available())
			{
				std::vector<FlashLog::Record> records;
				std::vector<Measurements::Measurement> measurements;
				while (store.Peek(64, records, measurements) > 0)
				{
					store.Consume(records);
					records.clear();
					measurements.clear();
				}
			}
			GenericESP32Firmware::ResetWireless(); // erases the bearer and Wi-Fi, restarts
			while (true)
				;
		}

		/**
		 * Post measurements to the backend in one upload. Returns the HTTP status code (<= 0: no connection).
		 */
		int PostMeasurements(const std::vector<Measurements::Measurement *> &items)
		{
			auto uploadObject = cJSON_CreateObject();
			cJSON_AddItemToObject(uploadObject, "device_time", cJSON_CreateNumber(time(nullptr)));
			auto measurements = cJSON_CreateArray();
			cJSON_AddItemToObject(uploadObject, "measurements", measurements);
			for (auto measurement : items)
				cJSON_AddItemToArray(measurements, measurement->GetJSON());

			// Compact JSON (smaller upload); cJSON allocates the string, so it must be freed.
			char *json = cJSON_PrintUnformatted(uploadObject);
			cJSON_Delete(uploadObject);
			if (json == nullptr)
				return -1;

			HTTPUtil::buffer_t dataSend(json);
			cJSON_free(json);
			HTTPUtil::buffer_t dataReceive;
			HTTPUtil::headers_t headersSend;
			HTTPUtil::headers_t headersReceive;
			int statusCode = GenericESP32Firmware::PostHTTPSToBackend(ENDPOINT_VARIABLE_UPLOAD, dataSend, headersSend, dataReceive, headersReceive, true);
			s_lastStatusCode = statusCode;
			if (UploadPolicy::IsReleased(statusCode))
				ForgetPairing(); // does not return
			if (statusCode != 200)
				ESP_LOGE(TAG, "Posting to backend returned statuscode %d. Response: %s", statusCode, dataReceive.c_str());
			else
			{
				s_lastSuccessfulUpload = time(nullptr);
				TakeDisplayOffset(dataReceive);
			}
			return statusCode;
		}
	} // namespace

	void Queue::AddMeasurement(const Measurements::Measurement &measurement)
	{
		if (!IsPlausibleTime(measurement.GetTimestamp()))
		{
			ESP_LOGW(TAG, "Measurement with implausible timestamp %lld skipped (clock not synced yet?).",
					 static_cast<long long>(measurement.GetTimestamp()));
			return;
		}

		// On flash if the device has a buffer partition: survives reboots, power loss and long outages.
		if (PersistentStore::GetInstance().Add(measurement))
			return;

		auto measurementPtr = new Measurements::Measurement(std::move(measurement));

		auto err = xQueueSend(m_measurementQueue, &measurementPtr, 0);
		if (err != pdTRUE)
			ESP_LOGE(TAG, "An error occured when adding an item to the queue: no space.");
	}

	int Queue::LastStatusCode() const
	{
		return s_lastStatusCode;
	}

	time_t Queue::LastSuccessfulUpload() const
	{
		return s_lastSuccessfulUpload;
	}

	void Queue::Upload()
	{
		UploadPersistent();
		UploadMemory();
	}

	void Queue::UploadPersistent()
	{
		auto &store = PersistentStore::GetInstance();
		if (!store.Available())
			return;
		for (int batch = 0; batch < PERSISTENT_BATCHES_PER_UPLOAD; batch++)
		{
			std::vector<FlashLog::Record> records;
			std::vector<Measurements::Measurement> measurements;
			if (store.Peek(PERSISTENT_BATCH, records, measurements) == 0)
				return;
			std::vector<Measurements::Measurement *> items;
			for (auto &m : measurements)
				items.push_back(&m);

			int statusCode = PostMeasurements(items);
			if (statusCode == 200)
			{
				store.Consume(records);
				ESP_LOGD(TAG, "Uploaded %u measurements from the persistent buffer; %u waiting.",
						 static_cast<unsigned>(items.size()), static_cast<unsigned>(store.Pending()));
				continue;
			}
			if (!UploadPolicy::IsRetryable(statusCode))
			{
				ESP_LOGE(TAG, "Not retryable: %u measurements dropped.", static_cast<unsigned>(items.size()));
				store.Consume(records);
				continue;
			}
			ESP_LOGW(TAG, "Upload failed; %u measurements stay in the persistent buffer.",
					 static_cast<unsigned>(store.Pending()));
			return;
		}
	}

	void Queue::UploadMemory()
	{
		// Take everything that is queued now; new measurements may be added while we upload.
		std::vector<Measurements::Measurement *> items;
		Measurements::Measurement *item = nullptr;
		while (xQueueReceive(m_measurementQueue, &item, 0) == pdTRUE)
			items.push_back(item);

		if (items.empty())
		{
			ESP_LOGD(TAG, "There were no measurements in the upload queue.");
			return;
		}

		int statusCode = PostMeasurements(items);
		if (statusCode == 200)
		{
			ESP_LOGD(TAG, "Uploaded %u measurements.", static_cast<unsigned>(items.size()));
			for (auto measurement : items)
				delete measurement;
			return;
		}

		if (!UploadPolicy::IsRetryable(statusCode))
		{
			ESP_LOGE(TAG, "Not retryable: %u measurements dropped.", static_cast<unsigned>(items.size()));
			for (auto measurement : items)
				delete measurement;
			return;
		}

		// Put the measurements back in front of the queue (keeping their order), for the next upload.
		// If the queue is full, the oldest ones are dropped.
		unsigned dropped = 0;
		for (auto it = items.rbegin(); it != items.rend(); ++it)
		{
			Measurements::Measurement *measurement = *it;
			if (xQueueSendToFront(m_measurementQueue, &measurement, 0) != pdTRUE)
			{
				delete measurement;
				dropped++;
			}
		}
		ESP_LOGW(TAG, "Kept %u measurements for the next upload, dropped %u (queue full).",
				 static_cast<unsigned>(items.size()) - dropped, dropped);
	}
} // namespace SecureUpload
