#include <secure_upload.hpp>

#include <esp_log.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <cJSON.h>

#include <cstdlib>
#include <ctime>
#include <vector>

#include <generic_esp_32.hpp>

constexpr const char *TAG = "Secure upload";

constexpr int UPLOAD_QUEUE_MAX = 50;

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
		 * Retry later only for failures that may go away: no connection, server errors, timeouts, rate limits.
		 * Other 4xx answers will not change by retrying.
		 */
		bool IsRetryable(int statusCode)
		{
			return statusCode <= 0 || statusCode >= 500 || statusCode == 408 || statusCode == 429;
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

		auto measurementPtr = new Measurements::Measurement(std::move(measurement));

		auto err = xQueueSend(m_measurementQueue, &measurementPtr, 0);
		if (err != pdTRUE)
			ESP_LOGE(TAG, "An error occured when adding an item to the queue: no space.");
	}

	void Queue::Upload()
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

		auto uploadObject = cJSON_CreateObject();

		// Add device upload time.
		cJSON_AddItemToObject(uploadObject, "device_time", cJSON_CreateNumber(time(nullptr)));

		// Add measurements array.
		auto measurements = cJSON_CreateArray();
		cJSON_AddItemToObject(uploadObject, "measurements", measurements);
		for (auto measurement : items)
			cJSON_AddItemToArray(measurements, measurement->GetJSON());

		// Compact JSON (smaller upload); cJSON allocates the string, so it must be freed.
		char *json = cJSON_PrintUnformatted(uploadObject);
		cJSON_Delete(uploadObject);

		int statusCode = -1;
		HTTPUtil::buffer_t dataReceive;
		if (json != nullptr)
		{
			HTTPUtil::buffer_t dataSend(json);
			cJSON_free(json);

			HTTPUtil::headers_t headersSend;
			HTTPUtil::headers_t headersReceive;
			statusCode = GenericESP32Firmware::PostHTTPSToBackend(ENDPOINT_VARIABLE_UPLOAD, dataSend, headersSend, dataReceive, headersReceive, true);
		}

		if (statusCode == 200)
		{
			ESP_LOGD(TAG, "Uploaded %u measurements.", static_cast<unsigned>(items.size()));
			for (auto measurement : items)
				delete measurement;
			return;
		}

		ESP_LOGE(TAG, "Posting to backend returned statuscode %d. Response: %s", statusCode, dataReceive.c_str());

		if (!IsRetryable(statusCode))
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
