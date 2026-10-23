/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "SecureTokenStore.hpp"

#include <utility>

namespace {
thread_local std::shared_ptr<SecureTokenStore::RequestContext> currentRequest;
}

SecureTokenStore::RequestContext::RequestContext(std::chrono::milliseconds timeout)
	: deadline(std::chrono::steady_clock::now() + timeout)
{
}

void SecureTokenStore::RequestContext::Cancel() noexcept
{
	cancelled.store(true, std::memory_order_relaxed);
}

bool SecureTokenStore::RequestContext::IsCancelled() const noexcept
{
	return cancelled.load(std::memory_order_relaxed);
}

SecureTokenStore::Status SecureTokenStore::RequestContext::GetStatus() const noexcept
{
	return status.load(std::memory_order_relaxed);
}

std::chrono::steady_clock::time_point SecureTokenStore::RequestContext::GetDeadline() const noexcept
{
	return deadline;
}

SecureTokenStore::RequestScope::RequestScope(std::shared_ptr<RequestContext> context)
	: previous(std::exchange(currentRequest, std::move(context)))
{
}

SecureTokenStore::RequestScope::~RequestScope()
{
	currentRequest = std::move(previous);
}

std::shared_ptr<SecureTokenStore::RequestContext> SecureTokenStore::RequestForCurrentThread()
{
	return currentRequest ? currentRequest : std::make_shared<RequestContext>();
}

bool SecureTokenStore::RequestMayStart(const std::shared_ptr<RequestContext> &request, std::string &error)
{
	if (request->IsCancelled()) {
		request->status.store(Status::Cancelled, std::memory_order_relaxed);
		error = "Secure credential storage was cancelled.";
		return false;
	}
	if (std::chrono::steady_clock::now() >= request->GetDeadline()) {
		request->status.store(Status::TimedOut, std::memory_order_relaxed);
		error = "Secure credential storage timed out.";
		return false;
	}
	return true;
}
