#pragma once

#include <cstdint>

/**
 * What the firmware does with an answer of the server. Header-only and without ESP-IDF, so
 * test/host/upload_policy_test.cpp checks it on the PC.
 */
namespace UploadPolicy
{
	/**
	 * Drop measurements only when the server says the upload itself is wrong: retrying the same data gives the same
	 * answer. Everything else is kept and retried at the next upload moment, so a server or proxy problem (wrong
	 * route: 404, token or proxy rule: 401/403, server errors, no connection) does not lose data. The persistent
	 * buffer drops the oldest measurements when it is full, so keeping is bounded.
	 */
	inline bool IsRetryable(int statusCode)
	{
		return !(statusCode == 400 || statusCode == 413 || statusCode == 422);
	}

	/**
	 * The server refuses this device's activation secret (403) or request (401). A device that is not registered
	 * yet, or a server error, gives another code (500 on API v3): then the app may simply not have registered the
	 * device yet, and trying again helps.
	 */
	inline bool IsActivationRefused(int statusCode)
	{
		return statusCode == 401 || statusCode == 403;
	}

	/** After this many refusals in a row the device forgets its Wi-Fi and shows the provisioning QR code again. */
	constexpr uint32_t ACTIVATION_REFUSALS_BEFORE_RESET = 3;

	/** Seconds to wait before activation attempt number failures + 1: 1, 2, 4, ... minutes, at most 30. */
	inline uint32_t ActivationRetryDelay_s(uint32_t failures)
	{
		uint32_t delay = 60;
		for (uint32_t i = 1; i < failures && delay < 30 * 60; i++)
			delay *= 2;
		return delay < 30 * 60 ? delay : 30 * 60;
	}
} // namespace UploadPolicy
