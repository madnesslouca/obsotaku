/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>

class SecureTokenStore {
public:
	enum class Status { NotStarted, Success, NotFound, Cancelled, TimedOut, Unavailable, Locked, InvalidInput, Failed };

	/* Share one context across a transaction, installed INSIDE its worker thread.
	 * Cancellation is thread-safe. A cancelled write may already have reached the
	 * service: callers must retain its backup before attempting the write, and
	 * reconcile using a fresh, uncancelled context if any step fails. */
	class RequestContext {
	public:
		explicit RequestContext(std::chrono::milliseconds timeout = std::chrono::seconds(15));
		void Cancel() noexcept;
		bool IsCancelled() const noexcept;
		Status GetStatus() const noexcept;
		std::chrono::steady_clock::time_point GetDeadline() const noexcept;

	private:
		friend class SecureTokenStore;
		const std::chrono::steady_clock::time_point deadline;
		std::atomic_bool cancelled{false};
		std::atomic<Status> status{Status::NotStarted};
	};

	class RequestScope {
	public:
		explicit RequestScope(std::shared_ptr<RequestContext> context);
		~RequestScope();
		RequestScope(const RequestScope &) = delete;
		RequestScope &operator=(const RequestScope &) = delete;

	private:
		std::shared_ptr<RequestContext> previous;
	};

	/* Blocking compatibility API. UI callers must dispatch the whole transaction
	 * to a worker; no nested Qt event loop is run by the credential backend.
	 * Missing Load has an empty error; failures never do. Missing Remove succeeds.
	 * Secret Service cancellation interrupts pending asynchronous calls. Windows
	 * checks before each native call; CredWrite/Read/Delete are not interruptible,
	 * and a successful native write is reported as successful even if cancelled
	 * concurrently. Callers still check cancellation before committing config. */
	static bool Save(const std::string &platformId, const std::string &accountId, const std::string &secret,
			 std::string &error);
	static std::optional<std::string> Load(const std::string &platformId, const std::string &accountId,
					       std::string &error);
	static bool Remove(const std::string &platformId, const std::string &accountId, std::string &error);

private:
	static std::shared_ptr<RequestContext> RequestForCurrentThread();
	static bool RequestMayStart(const std::shared_ptr<RequestContext> &request, std::string &error);
};
