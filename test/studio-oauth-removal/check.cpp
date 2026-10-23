/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Real manager/token/context code; deterministic storage/network substitutes.
 * No libobs startup, GUI, network client or OS credential backend is linked. */
#include <oauth/ConnectedAccountManager.hpp>
#include <util/base.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>
using namespace std;
using namespace std::chrono_literals;
using Store = SecureTokenStore;
using AccountManager = ConnectedAccountManager;
namespace {
mutex storageMutex, refreshMutex, logMutex;
map<pair<string, string>, string> storage;
atomic_int loads{0}, saves{0}, removes{0}, refreshCalls{0}, dirtyLogs{0};
atomic_bool failLoad{false}, failSave{false};
enum class RemoveBehavior { Normal, CommitCancel, FailBefore, FailAfter };
atomic<RemoveBehavior> removeBehavior{RemoveBehavior::Normal};
condition_variable refreshCv;
bool refreshBlocked = false, refreshEntered = false, refreshReleased = false;
int checks = 0, failures = 0;
string lastLog;
long long cancelledWaitMs = -1, expiredWaitMs = -1;
const ConnectedStreamAccount account{StreamPlatform::Twitch, "qa-account", "QA", true};
const string original = "{ \"access_token\":\"qa-old\", \"refresh_token\":\"qa-refresh\", \"expires_at\":1, \"extra\":\"preserve-raw\" }";
const string unexpired = "{ \"access_token\":\"qa-valid\", \"refresh_token\":\"qa-refresh\", \"expires_at\":4102444800, \"extra\":\"preserve-raw\" }";
void Check(bool pass, const char *label)
{
	++checks;
	if (!pass) { ++failures; cerr << "FAIL " << label << "\n"; }
}
void Seed(const string &value)
{
	lock_guard lock(storageMutex);
	storage[{"twitch", account.accountId}] = value;
}
optional<string> Raw()
{
	lock_guard lock(storageMutex);
	auto it = storage.find({"twitch", account.accountId});
	return it == storage.end() ? optional<string>{} : optional<string>{it->second};
}
void Reset()
{
	lock_guard lock(storageMutex);
	storage.clear();
	loads = saves = removes = refreshCalls = dirtyLogs = 0;
	failLoad = failSave = false;
	removeBehavior = RemoveBehavior::Normal;
	lastLog.clear();
}
void BlockRefresh()
{
	lock_guard lock(refreshMutex);
	refreshBlocked = true; refreshEntered = false; refreshReleased = false;
}
bool AwaitRefresh()
{
	unique_lock lock(refreshMutex);
	return refreshCv.wait_for(lock, 2s, [] { return refreshEntered; });
}
void ReleaseRefresh()
{
	lock_guard lock(refreshMutex);
	refreshReleased = true; refreshCv.notify_all();
}
bool WaitFlag(const atomic_bool &flag)
{
	for (int i = 0; i < 200 && !flag.load(); ++i) this_thread::sleep_for(1ms);
	return flag.load();
}
thread StartRefresh(atomic_bool &done, bool &result, OAuthTokenSet &tokens)
{
	return thread([&] {
		string error;
		result = AccountManager::LoadUsableTokens(account.platform, account.accountId, {}, "", tokens, error);
		done = true;
	});
}
void FreshRollback(AccountManager::CredentialRemoval &lease, bool expected = true)
{
	string error;
	Store::RequestScope recovery(make_shared<Store::RequestContext>());
	const bool okay = lease.Rollback(error);
	Check(okay == expected, "rollback outcome explicit");
	Check(okay ? error.empty() : !error.empty(), "rollback error matches outcome");
}
void Basic()
{
	Reset(); string error;
	Check(!AccountManager::BeginCredentialRemoval({StreamPlatform::CustomRtmp, "qa", "", true}, {}, error), "reject custom account");
	Check(!error.empty() && loads == 0 && removes == 0, "invalid account no credential calls");
	Check(!AccountManager::BeginCredentialRemoval({StreamPlatform::Twitch, "", "", true}, {}, error), "reject empty account");
	Seed(original);
	{
		auto lease = AccountManager::BeginCredentialRemoval(account, {}, error);
		Check(bool(lease) && error.empty(), "prepare backs up current raw credential");
		Check(loads == 1 && removes == 0 && saves == 0 && Raw() == original, "prepare makes no mutation");
	}
	Check(removes == 0 && saves == 0 && dirtyLogs == 0, "unattempted destructor no credential IO");
	auto ctx = make_shared<Store::RequestContext>();
	auto lease = AccountManager::BeginCredentialRemoval(account, ctx, error);
	Check(bool(lease), "prepare rollback transaction");
	Check(lease->Remove(error) && !Raw(), "remove attempts credential deletion");
	ctx->Cancel();
	FreshRollback(*lease);
	Check(Raw() == original, "restore exact raw bytes including unknown fields");
	const int called = removes;
	Check(!lease->Remove(error) && !error.empty() && removes == called, "cancelled request prevents another delete");
	lease.reset();
	Check(dirtyLogs == 0, "successful rollback destructor clean");
}
void WaitInterrupt(bool cancel)
{
	Reset(); Seed(original); BlockRefresh();
	atomic_bool done{false}; bool result = false; OAuthTokenSet tokens;
	auto refresher = StartRefresh(done, result, tokens);
	Check(AwaitRefresh(), "refresh owns existing account mutex");
	auto ctx = make_shared<Store::RequestContext>(cancel ? 2s : 80ms);
	thread stopper;
	if (cancel) stopper = thread([ctx] { this_thread::sleep_for(40ms); ctx->Cancel(); });
	string error; const auto start = chrono::steady_clock::now();
	auto lease = AccountManager::BeginCredentialRemoval(account, ctx, error);
	const auto elapsed = chrono::steady_clock::now() - start;
	(cancel ? cancelledWaitMs : expiredWaitMs) = chrono::duration_cast<chrono::milliseconds>(elapsed).count();
	if (stopper.joinable()) stopper.join();
	Check(!lease && !error.empty(), "interrupt account-lock wait");
	Check(elapsed < 500ms, "wait interruption bounded before network completes");
	Check(cancel ? ctx->IsCancelled() : chrono::steady_clock::now() >= ctx->GetDeadline(), "cancel or deadline observed");
	Check(!done && removes == 0 && loads == 1, "waiting cancellation never reaches credential backend");
	ReleaseRefresh(); refresher.join();
	Check(result && refreshCalls == 1 && Raw().has_value(), "refresh completes unmodified after abandoned wait");
	Check(dirtyLogs == 0, "abandoned wait no dirty lease");
}
void RotatedBackup()
{
	Reset(); Seed(original); BlockRefresh();
	atomic_bool done{false}; bool result = false; OAuthTokenSet tokens;
	auto refresher = StartRefresh(done, result, tokens);
	Check(AwaitRefresh(), "rotating refresh owns lock");
	thread release([] { this_thread::sleep_for(40ms); ReleaseRefresh(); });
	string error;
	auto lease = AccountManager::BeginCredentialRemoval(account, make_shared<Store::RequestContext>(2s), error);
	release.join(); refresher.join();
	Check(bool(lease) && result && tokens.accessToken == "qa-rotated", "prepare waits for rotation to become durable");
	const auto rotatedRaw = Raw();
	Check(rotatedRaw && *rotatedRaw != original, "current credential rotated before backup");
	Check(lease->Remove(error) && !Raw(), "delete current rotated credential");
	FreshRollback(*lease);
	Check(Raw() == rotatedRaw, "rollback never resurrects pre-refresh credential");
	lease.reset();
	Check(dirtyLogs == 0, "rotated recovery closes cleanly");
}
void HeldLease()
{
	Reset(); Seed(unexpired); string error;
	auto lease = AccountManager::BeginCredentialRemoval(account, {}, error);
	Check(bool(lease), "lease prepared before concurrent refresh");
	const int before = loads;
	atomic_bool done{false}; bool result = false; OAuthTokenSet tokens;
	auto refresher = StartRefresh(done, result, tokens);
	this_thread::sleep_for(35ms);
	Check(!done && loads == before, "lease excludes refresh load until final decision");
	Check(lease->Remove(error), "delete while refresh waits");
	FreshRollback(*lease);
	this_thread::sleep_for(15ms);
	Check(!done, "rollback retains lease until worker finishes");
	lease.reset();
	Check(WaitFlag(done), "destruction releases existing account mutex");
	refresher.join();
	Check(result && tokens.accessToken == "qa-valid" && refreshCalls == 0, "waiting consumer sees restored credential");
}
void AmbiguousAndIncomplete()
{
	Reset(); Seed(original); string error;
	auto ctx = make_shared<Store::RequestContext>();
	auto lease = AccountManager::BeginCredentialRemoval(account, ctx, error);
	removeBehavior = RemoveBehavior::CommitCancel;
	Check(!lease->Remove(error) && ctx->IsCancelled() && !Raw(), "cancelled delete may already commit");
	FreshRollback(*lease);
	Check(Raw() == original, "fresh recovery restores after ambiguous delete");
	lease.reset();
	Check(dirtyLogs == 0, "ambiguous delete recovered explicitly");

	removeBehavior = RemoveBehavior::FailBefore;
	lease = AccountManager::BeginCredentialRemoval(account, {}, error);
	Check(!lease->Remove(error) && Raw() == original, "failure before commit still attempts recovery");
	FreshRollback(*lease); lease.reset();

	removeBehavior = RemoveBehavior::FailAfter;
	lease = AccountManager::BeginCredentialRemoval(account, {}, error);
	Check(!lease->Remove(error) && !Raw(), "unconfirmed deletion marked before backend call");
	failSave = true; FreshRollback(*lease, false);
	const int saveCount = saves, removeCount = removes;
	lease.reset();
	Check(saves == saveCount && removes == removeCount, "dirty destructor performs no hidden credential IO");
	Check(dirtyLogs == 1, "unresolved removal emits fixed diagnostic");
	Check(lastLog.find(account.accountId) == string::npos && lastLog.find("qa-old") == string::npos, "dirty diagnostic has no identifier or secret");
	failSave = false; Seed(unexpired);
	atomic_bool done{false}; bool result = false; OAuthTokenSet tokens;
	auto refresher = StartRefresh(done, result, tokens);
	Check(WaitFlag(done), "failed rollback destructor still releases account mutex");
	refresher.join();
	Check(result, "account remains usable after explicitly incomplete recovery");
}
void CommitMissingAndLegacy()
{
	Reset(); string error;
	auto lease = AccountManager::BeginCredentialRemoval(account, {}, error);
	Check(bool(lease) && error.empty(), "absent credential has a valid no-value backup");
	Check(lease->Remove(error), "absent deletion idempotent");
	FreshRollback(*lease);
	Check(!Raw(), "rollback absent backup preserves absence");
	lease->Commit();
	Check(!lease->Remove(error) && !error.empty(), "committed lease rejects further mutation");
	Check(!lease->Rollback(error) && !error.empty(), "committed lease cannot restore old credential");
	lease.reset();

	Seed(original);
	Check(AccountManager::Disconnect(account, error) && !Raw(), "legacy wrapper commits confirmed deletion");
	Seed(original);
	removeBehavior = RemoveBehavior::CommitCancel;
	Check(!AccountManager::Disconnect(account, error) && !error.empty(), "legacy wrapper returns ambiguous removal failure");
	Check(Raw() == original && dirtyLogs == 0, "legacy wrapper reconciles before returning failure");
	removeBehavior = RemoveBehavior::FailAfter; failSave = true;
	Check(!AccountManager::Disconnect(account, error), "legacy wrapper returns failed recovery");
	Check(error.find("did not complete") != string::npos && dirtyLogs == 1, "legacy wrapper reports incomplete rollback");
}
void PrepareFailure()
{
	Reset(); Seed(unexpired); string error; failLoad = true;
	Check(!AccountManager::BeginCredentialRemoval(account, {}, error) && !error.empty(), "backup failure refuses transaction");
	Check(removes == 0 && saves == 0 && Raw() == unexpired, "backup failure no mutation");
	failLoad = false;
	auto ctx = make_shared<Store::RequestContext>();
	auto lease = AccountManager::BeginCredentialRemoval(account, ctx, error);
	Check(bool(lease), "backup failure released mutex");
	ctx->Cancel(); const int before = removes;
	Check(!lease->Remove(error) && removes == before && Raw() == unexpired, "cancel between prepare/remove leaves original");
}
}
extern "C" void blog(int, const char *format, ...)
{
	lock_guard lock(logMutex); lastLog = format;
	if (lastLog == "OAuth credential removal ended without confirmed commit or rollback.") ++dirtyLogs;
}
bool Store::Save(const string &platform, const string &id, const string &secret, string &error)
{
	++saves; auto request = RequestForCurrentThread();
	if (!RequestMayStart(request, error)) return false;
	if (failSave) {
		request->status = Status::Failed; error = "Private fixture save failed."; return false;
	}
	lock_guard lock(storageMutex);
	storage[{platform, id}] = secret; request->status = Status::Success; error.clear(); return true;
}
optional<string> Store::Load(const string &platform, const string &id, string &error)
{
	++loads; auto request = RequestForCurrentThread();
	if (!RequestMayStart(request, error)) return {};
	if (failLoad) {
		request->status = Status::Unavailable; error = "Private fixture provider unavailable."; return {};
	}
	lock_guard lock(storageMutex); auto it = storage.find({platform, id});
	request->status = it == storage.end() ? Status::NotFound : Status::Success; error.clear();
	return it == storage.end() ? optional<string>{} : optional<string>{it->second};
}
bool Store::Remove(const string &platform, const string &id, string &error)
{
	++removes; auto request = RequestForCurrentThread();
	if (!RequestMayStart(request, error)) return false;
	const auto behavior = removeBehavior.exchange(RemoveBehavior::Normal);
	if (behavior == RemoveBehavior::FailBefore) {
		request->status = Status::Failed; error = "Private fixture removal failed before commit."; return false;
	}
	{ lock_guard lock(storageMutex); storage.erase({platform, id}); }
	if (behavior == RemoveBehavior::CommitCancel) {
		request->Cancel(); request->status = Status::Cancelled;
		error = "Private fixture removal reply cancelled after commit."; return false;
	}
	if (behavior == RemoveBehavior::FailAfter) {
		request->status = Status::Failed; error = "Private fixture removal reply failed after commit."; return false;
	}
	request->status = Status::Success; error.clear(); return true;
}
bool PlatformOAuthClient::RefreshTokens(StreamPlatform, const OAuthClientRegistration &, const string &,
				       const OAuthTokenSet &current, OAuthTokenSet &tokens, string &error)
{
	++refreshCalls;
	{
		unique_lock lock(refreshMutex); refreshEntered = true; refreshCv.notify_all();
		if (refreshBlocked) refreshCv.wait(lock, [] { return refreshReleased; });
	}
	tokens = current; tokens.accessToken = "qa-rotated"; tokens.refreshToken = "qa-rotated-refresh";
	tokens.expiresAt = 4102444800; error.clear(); return true;
}
bool PlatformOAuthClient::ResolveIngest(StreamPlatform, const OAuthClientRegistration &, const OAuthTokenSet &,
				       ResolvedStreamIngest &, string &error)
{
	error = "Ingest resolution is not part of this private fixture."; return false;
}
int main()
{
	Basic(); WaitInterrupt(true); WaitInterrupt(false); RotatedBackup(); HeldLease();
	AmbiguousAndIncomplete(); CommitMissingAndLegacy(); PrepareFailure();
	cout << "{\"checks\":" << checks << ",\"failures\":" << failures << ",\"passed\":"
	     << (failures == 0 ? "true" : "false") << ",\"cancelled_wait_ms\":" << cancelledWaitMs
	     << ",\"expired_wait_ms\":" << expiredWaitMs << "}\n";
	return failures ? 1 : 0;
}
