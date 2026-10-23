/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "SecureTokenStore.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using Store = SecureTokenStore;
using Status = Store::Status;
using namespace std::chrono_literals;

namespace {
int checks = 0, failures = 0;
const std::string platform = "otakudobs-qa-private";
const std::string account = "fixture-account";
const std::string initial = "OtakudOBS-QA-private-canary-v1";
const std::string changed = "OtakudOBS-QA-private-canary-v2";
void Check(bool condition, const char *label)
{
	++checks;
	if (!condition) {
		++failures;
		std::cerr << "FAIL " << label << "\n";
	}
}
void ExpectSecret(const std::string &p, const std::string &a, const std::string &expected)
{
	std::string error = "stale";
	auto secret = Store::Load(p, a, error);
	Check(secret && *secret == expected, "exact secret round trip");
	Check(error.empty(), "successful lookup clears error");
}
void RoundTrip()
{
	std::string error;
	Store::Remove(platform, account, error);
	auto context = std::make_shared<Store::RequestContext>();
	Store::RequestScope scope(context);
	auto absent = Store::Load(platform, account, error);
	Check(!absent && error.empty(), "missing is distinct from failure");
	Check(context->GetStatus() == Status::NotFound, "missing typed status");
	Check(Store::Save(platform, account, initial, error), "save initial");
	Check(error.empty(), "save clears error");
	Check(context->GetStatus() == Status::Success, "save success status");
	ExpectSecret(platform, account, initial);
	Check(Store::Save(platform, account, changed, error), "update credential");
	ExpectSecret(platform, account, changed);
	const std::string binary("qa-\0-byte-\xff", 11);
	Check(Store::Save(platform, account, binary, error), "binary credential supported");
	ExpectSecret(platform, account, binary);
	Check(Store::Save(platform + "/a", "b", initial, error), "first structured namespace");
	Check(Store::Save(platform, "a/b", changed, error), "second structured namespace");
	ExpectSecret(platform + "/a", "b", initial);
	ExpectSecret(platform, "a/b", changed);
	Check(Store::Save(platform, "conta-ação-日本", initial, error), "unicode identifier");
	ExpectSecret(platform, "conta-ação-日本", initial);
	Check(Store::Remove(platform, "conta-ação-日本", error), "remove unicode");
	Check(Store::Remove(platform + "/a", "b", error), "remove first namespace");
	Check(Store::Remove(platform, "a/b", error), "remove second namespace");
	Check(Store::Remove(platform, account, error), "remove credential");
	Check(Store::Remove(platform, account, error), "remove absent idempotent");
	Check(!Store::Load(platform, account, error) && error.empty(), "removed credential absent");

	Check(!Store::Save("", account, initial, error) && !error.empty(), "reject empty platform");
	Check(context->GetStatus() == Status::InvalidInput, "invalid typed status");
	Check(!Store::Save(platform, account, "", error) && !error.empty(), "reject empty secret");
	Check(!Store::Load(platform, std::string("a\0b", 3), error) && !error.empty(), "reject NUL identifier");
	Check(!Store::Load(platform, std::string("\xff", 1), error) && !error.empty(), "reject invalid UTF8");

	context->Cancel();
	Check(!Store::Save(platform, account, changed, error), "cancelled scope blocks write");
	Check(context->GetStatus() == Status::Cancelled, "cancelled status");
	{
		auto recovery = std::make_shared<Store::RequestContext>();
		Store::RequestScope recoveryScope(recovery);
		Check(Store::Save(platform, account, initial, error), "fresh rollback scope not cancelled");
		ExpectSecret(platform, account, initial);
	}
	Check(!Store::Load(platform, account, error), "outer cancelled scope restored");
	Check(context->GetStatus() == Status::Cancelled, "outer scope cancellation retained");
	{
		Store::RequestScope recoveryScope(std::make_shared<Store::RequestContext>());
		Check(Store::Remove(platform, account, error), "cleanup in fresh scope");
	}
}
void Interrupted(bool cancel, bool already)
{
	auto context = std::make_shared<Store::RequestContext>(cancel ? 5s : (already ? -1ms : 140ms));
	Store::RequestScope scope(context);
	std::thread canceller;
	if (cancel && already)
		context->Cancel();
	else if (cancel)
		canceller = std::thread([context] { std::this_thread::sleep_for(60ms); context->Cancel(); });
	std::string error;
	auto start = std::chrono::steady_clock::now();
	auto secret = Store::Load(platform, account, error);
	auto elapsed = std::chrono::steady_clock::now() - start;
	if (canceller.joinable())
		canceller.join();
	Check(!secret && !error.empty(), "interrupted lookup fails closed");
	Check(context->GetStatus() == (cancel ? Status::Cancelled : Status::TimedOut), "typed interruption");
	Check(elapsed < 2s, "bounded interruption completion");
	if (already)
		Check(elapsed < 100ms, "pre-interrupted avoids D-Bus");
	else if (!cancel)
		Check(elapsed >= 100ms, "deadline actually waited");
	start = std::chrono::steady_clock::now();
	secret = Store::Load(platform, account, error);
	Check(!secret && !error.empty(), "shared scope stays interrupted");
	Check(std::chrono::steady_clock::now() - start < 100ms, "transaction deadline not renewed per call");
}
void Locked()
{
	auto context = std::make_shared<Store::RequestContext>(2s);
	Store::RequestScope scope(context);
	std::string error;
	Check(!Store::Load(platform, account, error), "locked does not return secret");
	Check(!error.empty() && context->GetStatus() == Status::Locked, "dismissed unlock is not missing");
	Check(!Store::Remove(platform, account, error), "locked item not reported deleted");
	Check(!error.empty() && context->GetStatus() == Status::Locked, "locked remove typed");
}
void Failure()
{
	auto context = std::make_shared<Store::RequestContext>(2s);
	Store::RequestScope scope(context);
	std::string error;
	Check(!Store::Load(platform, account, error), "unavailable lookup has no value");
	Check(!error.empty() && context->GetStatus() == Status::Unavailable, "unavailable distinct from missing");
	Check(error.find(initial) == std::string::npos, "remote error text redacted");
}
void CommitCancel(bool timeout)
{
	std::string error;
	Check(Store::Save(platform, account, initial, error), "seed before ambiguous write");
	auto context = std::make_shared<Store::RequestContext>(timeout ? 140ms : 3s);
	std::thread cancel;
	{
		Store::RequestScope scope(context);
		if (!timeout)
			cancel = std::thread([context] { std::this_thread::sleep_for(80ms); context->Cancel(); });
		Check(!Store::Save(platform, account, changed, error), "in-flight write returns cancelled");
		Check(context->GetStatus() == (timeout ? Status::TimedOut : Status::Cancelled), "in-flight typed interruption");
	}
	if (cancel.joinable())
		cancel.join();
	ExpectSecret(platform, account, changed);
	Check(Store::Save(platform, account, initial, error), "fresh scope reconciles committed write");
	ExpectSecret(platform, account, initial);
	Check(Store::Remove(platform, account, error), "cleanup reconciled credential");
}
void Parallel()
{
	std::vector<int> okay(4);
	std::vector<std::thread> threads;
	for (int n = 0; n < 4; ++n)
		threads.emplace_back([&, n] {
			std::string id = "parallel-" + std::to_string(n), error;
			bool pass = true;
			for (int i = 0; i < 3; ++i) {
				auto ctx = std::make_shared<Store::RequestContext>(5s);
				Store::RequestScope scope(ctx);
				pass &= Store::Save(platform, id, initial, error);
				auto secret = Store::Load(platform, id, error);
				pass &= secret && *secret == initial && error.empty();
			}
			pass &= Store::Remove(platform, id, error);
			okay[n] = pass;
		});
	for (auto &thread : threads)
		thread.join();
	for (bool pass : okay)
		Check(pass, "independent per-call contexts across threads");
}
void WindowsNative()
{
	std::string error;
	auto context = std::make_shared<Store::RequestContext>();
	Store::RequestScope scope(context);
	Check(Store::Remove(platform, account, error), "private Windows cleanup before test");
	Check(!Store::Load(platform, account, error) && error.empty(), "Windows missing is not failure");
	Check(context->GetStatus() == Status::NotFound, "Windows missing typed status");
	Check(Store::Save(platform, account, initial, error), "Windows native initial write");
	Check(context->GetStatus() == Status::Success && error.empty(), "Windows write typed status");
	ExpectSecret(platform, account, initial);
	Check(Store::Save(platform, account, changed, error), "Windows native update");
	ExpectSecret(platform, account, changed);
	context->Cancel();
	Check(!Store::Save(platform, account, initial, error), "Windows pre-cancel prevents native write");
	Check(context->GetStatus() == Status::Cancelled && !error.empty(), "Windows cancellation typed");
	{
		Store::RequestScope recovery(std::make_shared<Store::RequestContext>());
		ExpectSecret(platform, account, changed);
	}
	{
		auto expired = std::make_shared<Store::RequestContext>(-1ms);
		Store::RequestScope expiredScope(expired);
		Check(!Store::Remove(platform, account, error), "Windows pre-expired prevents native deletion");
		Check(expired->GetStatus() == Status::TimedOut && !error.empty(), "Windows timeout typed");
	}
	{
		auto recoveryContext = std::make_shared<Store::RequestContext>();
		Store::RequestScope recovery(recoveryContext);
		ExpectSecret(platform, account, changed);
		Check(!Store::Save("", account, initial, error), "Windows invalid identifier");
		Check(recoveryContext->GetStatus() == Status::InvalidInput, "Windows invalid typed status");
		Check(!Store::Save(platform, account + std::string("\0suffix", 7), initial, error), "Windows rejects NUL write target");
		Check(recoveryContext->GetStatus() == Status::InvalidInput, "Windows NUL write typed invalid");
		Check(!Store::Load(platform + std::string("\0suffix", 7), account, error), "Windows rejects NUL lookup target");
		Check(!error.empty() && recoveryContext->GetStatus() == Status::InvalidInput, "Windows NUL lookup typed invalid");
		Check(!Store::Remove(platform, account + std::string("\0suffix", 7), error), "Windows rejects NUL removal target");
		Check(recoveryContext->GetStatus() == Status::InvalidInput, "Windows NUL removal typed invalid");
		ExpectSecret(platform, account, changed);
		Check(Store::Remove(platform, account, error), "Windows cleanup native item");
		Check(Store::Remove(platform, account, error), "Windows idempotent cleanup");
		Check(!Store::Load(platform, account, error) && error.empty(), "Windows item absent after cleanup");
	}
}
}
int main(int argc, char **argv)
{
#ifdef __linux__
	const char *privateBus = std::getenv("OTAKUDOBS_PRIVATE_BUS_ADDRESS");
	const char *sessionBus = std::getenv("DBUS_SESSION_BUS_ADDRESS");
	if (!privateBus || !sessionBus || std::string(privateBus) != sessionBus) {
		std::cerr << "This fixture requires its isolated D-Bus session guard\n";
		return 2;
	}
#endif
#ifdef _WIN32
	if (!std::getenv("OTAKUDOBS_PRIVATE_WINDOWS_STORE")) {
		std::cerr << "This fixture requires an explicitly isolated Windows QA account\n";
		return 2;
	}
#endif
	const std::string mode = argc > 1 ? argv[1] : "";
	if (mode == "windows-native")
		WindowsNative();
	else if (mode == "roundtrip")
		RoundTrip();
	else if (mode == "parallel")
		Parallel();
	else if (mode == "timeout")
		Interrupted(false, false);
	else if (mode == "cancel")
		Interrupted(true, false);
	else if (mode == "pre-cancel")
		Interrupted(true, true);
	else if (mode == "expired")
		Interrupted(false, true);
	else if (mode == "locked")
		Locked();
	else if (mode == "unavailable" || mode == "remote-error")
		Failure();
	else if (mode == "commit-cancel")
		CommitCancel(false);
	else if (mode == "commit-timeout")
		CommitCancel(true);
	else if (mode == "persist-write") {
		std::string error;
		Check(Store::Save(platform, account, initial, error), "persist write");
	} else if (mode == "persist-read") {
		ExpectSecret(platform, account, initial);
		std::string error;
		Check(Store::Remove(platform, account, error), "persist cleanup");
	} else {
		std::cerr << "Unknown fixture mode\n";
		return 2;
	}
	std::cout << "{\"mode\":\"" << mode << "\",\"checks\":" << checks << ",\"failures\":" << failures
		  << ",\"passed\":" << (failures == 0 ? "true" : "false") << "}\n";
	return failures ? 1 : 0;
}
