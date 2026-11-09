#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <measurements.hpp>

namespace SecureUpload
{
    /**
     * A queue that holds measurement data.
     *
     * Use Queue::AddMeasurement(measurement) to add a measurement to the queue.
     * Use Queue::Upload() to upload the queue's contents to the backend server.
     */
    class Queue
    {
    public:
        /**
         * Get an instance of the queue. This is a singleton.
         *
         * @returns an instance of the queue.
         */
        static Queue &GetInstance();

        /**
         * Add a measurement to the queue. The measurements will be sent when Queue::Upload() is called.
         *
         * @param measurement The measurement to add to the queue.
         *
         */
        void AddMeasurement(const Measurements::Measurement &measurement);

        /**
         * Upload all measurements on the queue to the backend API.
         */
        void Upload();

    private:
        /**
         * Create a new queue.
         */
        Queue();

        /** Upload from the persistent buffer on flash (if the device has one), in batches. */
        void UploadPersistent();

        /** Upload what is buffered in RAM. */
        void UploadMemory();

        /**
         * Queue of measurements to upload.
         */
        QueueHandle_t m_measurementQueue;
    };
} // namespace Upload
