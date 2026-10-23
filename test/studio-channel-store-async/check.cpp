#include <OBSApp.hpp>
int FixtureConfigSave(config_t *, const char *, const char *);
#define config_save_safe FixtureConfigSave
#include <utility/MultistreamChannelStore.cpp>
#undef config_save_safe
#include <oauth/OAuthTokenSet.hpp>
#include <QTimer>
#include <QTemporaryDir>
#include <QJsonObject>
#include <QFile>
#include <iostream>
#include <thread>
#include <atomic>
OBSApp appStub;
OBSApp *App()
{
	return &appStub;
}
namespace fixture {
std::mutex mu;
std::map<std::string, std::string> vault;
std::vector<std::string> events;
std::vector<const void *> contexts;
std::atomic_bool failLoad{false}, failRemove{false}, failConfig{false};
std::atomic_int delay{0};
std::atomic_bool failSave{false}, failRollback{false}, cancelAfterCommit{false};
std::string key(const std::string &p, const std::string &a)
{
	return p + "/" + a;
}
std::string get(const std::string &p, const std::string &a)
{
	std::lock_guard l(mu);
	auto it = vault.find(key(p, a));
	return it == vault.end() ? "" : it->second;
}
void put(std::string p, std::string a, std::string v)
{
	std::lock_guard l(mu);
	vault[key(p, a)] = std::move(v);
}
int checks = 0;
std::vector<std::string> failures;
void check(bool pass, std::string why)
{
	++checks;
	if (!pass) {
		failures.push_back(why);
		std::cerr << "FAIL " << why << '\n';
	}
}
} // namespace fixture
bool SecureTokenStore::Save(const std::string &p, const std::string &a, const std::string &v, std::string &error)
{
	auto ctx = RequestForCurrentThread();
	if (!RequestMayStart(ctx, error)) {
		return false;
	}
	{
		std::lock_guard l(fixture::mu);
		fixture::contexts.push_back(ctx.get());
	}
	{
		std::lock_guard l(fixture::mu);
		fixture::events.push_back("save:" + fixture::key(p, a));
		fixture::vault[fixture::key(p, a)] = v;
	}
	const int ms = fixture::delay.exchange(0);
	if (ms) {
		std::this_thread::sleep_for(std::chrono::milliseconds(ms));
	}
	if (fixture::cancelAfterCommit.exchange(false)) {
		ctx->Cancel();
		error = "Reply cancelled after commit";
		return false;
	}
	if (fixture::failSave.exchange(false) || fixture::failRollback.load()) {
		error = "Injected save failure";
		return false;
	}
	ctx->status.store(Status::Success);
	error.clear();
	return true;
}
std::optional<std::string> SecureTokenStore::Load(const std::string &p, const std::string &a, std::string &error)
{
	auto ctx = RequestForCurrentThread();
	if (!RequestMayStart(ctx, error)) {
		return {};
	}
	if (fixture::failLoad.exchange(false)) {
		ctx->status.store(Status::Locked);
		error = "Injected locked vault";
		return {};
	}
	{
		std::lock_guard l(fixture::mu);
		fixture::contexts.push_back(ctx.get());
	}
	std::lock_guard l(fixture::mu);
	fixture::events.push_back("load:" + fixture::key(p, a));
	auto it = fixture::vault.find(fixture::key(p, a));
	error.clear();
	ctx->status.store(it == fixture::vault.end() ? Status::NotFound : Status::Success);
	if (it == fixture::vault.end()) {
		return {};
	}
	return it->second;
}
bool SecureTokenStore::Remove(const std::string &p, const std::string &a, std::string &error)
{
	auto ctx = RequestForCurrentThread();
	if (!RequestMayStart(ctx, error)) {
		return false;
	}
	if (fixture::failRemove.exchange(false)) {
		error = "Injected remove failure";
		return false;
	}
	std::lock_guard l(fixture::mu);
	fixture::events.push_back("remove:" + fixture::key(p, a));
	fixture::vault.erase(fixture::key(p, a));
	error.clear();
	ctx->status.store(Status::Success);
	return true;
}
bool OAuthTokenSet::Remove(StreamPlatform platform, const std::string &id, std::string &error)
{
	return SecureTokenStore::Remove(std::string(GetStreamPlatformInfo(platform).id), id, error);
}
int FixtureConfigSave(config_t *c, const char *t, const char *b)
{
	if (fixture::failConfig.exchange(false)) {
		return CONFIG_ERROR;
	}
	return config_save_safe(c, t, b);
}
using Store = MultistreamChannelStore;
struct Test {
	QObject *receiver = new QObject;
	int heartbeat = 0;
	QTimer pulse;
	QTemporaryDir dir;
	std::vector<std::function<void()>> steps;
	size_t current = 0;
	Test()
	{
		appStub.userConfigLocation = dir.path().toStdString();
		config_open(&appStub.config, (dir.path() + "/user.ini").toUtf8(), CONFIG_OPEN_ALWAYS);
		pulse.setInterval(5);
		QObject::connect(&pulse, &QTimer::timeout, [this] { ++heartbeat; });
		pulse.start();
	}
	MultiStreamChannel channel(std::string id, std::string key = "")
	{
		MultiStreamChannel c;
		c.id = id;
		c.displayName = id;
		c.server = "rtmp://127.0.0.1/local";
		c.streamKey = key;
		return c;
	}
	void seed(std::vector<MultiStreamChannel> c)
	{
		std::string error;
		fixture::check(CommitMetadata(c, error), "seed config");
	}
	void next()
	{
		if (current == steps.size()) {
			finish();
			return;
		}
		QTimer::singleShot(0, receiver, [this] { steps[current++](); });
	}
	void finish()
	{
		Store::Shutdown();
		ShutdownMultistreamTasks();
		fixture::check(MultistreamTaskPool().StopToken()->load(),
			       "pool shutdown signals cancellation before drain");
		fixture::check(!MultistreamTaskPool().start([] {}), "pool rejects jobs after shutdown");
		QJsonArray errors;
		for (auto &e : fixture::failures) {
			errors.append(QString::fromStdString(e));
		}
		QJsonObject report{
			{"checks", fixture::checks},
			{"failures", errors},
			{"passed", errors.empty()},
			{"heartbeat", heartbeat},
			{"scope",
			 "Real Store, RequestContext, config and OAuth removal lease; simulated credential backend; no OBS instance/network/keyring."}};
		std::cout << QJsonDocument(report).toJson().constData();
		config_close(appStub.config);
		appStub.config = nullptr;
		QCoreApplication::exit(errors.empty() ? 0 : 1);
	}
	void setup()
	{
		using fixture::check;
		steps.push_back([this] {
			seed({channel("a")});
			Store::LoadAsync(receiver, [this](Store::Result r) {
				check(r.success && !r.warning.empty(), "missing key visible warning");
				check(Store::Load()[0].enabled, "missing key preserves enabled intent");
				next();
			});
		});
		steps.push_back([this] {
			auto current = Store::CaptureMetadataGuard();
			check(current(), "metadata guard accepts its original session");
			auto before = fixture::events.size();
			config_set_string(appStub.config, SECTION, "Channel0.Title", "externally changed");
			check(!current(), "metadata guard rejects external edits before network delivery");
			check(fixture::events.size() == before, "metadata guard performs no credential I/O");
			next();
		});
		steps.push_back([this] {
			fixture::put("manual-rtmp", "a", "old");
			Store::LoadAsync(receiver, [this](Store::Result r) {
				check(r.success && r.channels[0].streamKey == "old", "load hydrates key");
				auto *cfg = appStub.config;
				config_set_string(cfg, SECTION, "Channel0.AccountId", "external");
				check(Store::Load()[0].streamKey.empty(),
				      "external same-id identity invalidates cache");
				next();
			});
		});
		steps.push_back([this] {
			seed({channel("a"), channel("b")});
			fixture::put("manual-rtmp", "b", "old-b");
			fixture::events.clear();
			fixture::contexts.clear();
			auto a = channel("a", "new-a"), b = channel("b", "new-b");
			fixture::delay = 150;
			int before = heartbeat;
			Store::SaveAsync({a, b}, receiver, [this, before](Store::Result r) {
				check(r.success && r.configCommitted && r.deliveryCurrent,
				      "async save commits current delivery");
				check(heartbeat - before >= 10, "GUI remains responsive during delayed write");
				check(fixture::events.size() >= 4 && fixture::events[0].find("load:") == 0 &&
					      fixture::events[1].find("load:") == 0,
				      "all backups precede writes");
				check(std::all_of(fixture::contexts.begin(), fixture::contexts.end(),
						  [](auto p) { return p == fixture::contexts.front(); }),
				      "one total request context covers backups and writes");
				check(fixture::get("manual-rtmp", "a") == "new-a" &&
					      fixture::get("manual-rtmp", "b") == "new-b",
				      "keys committed");
				next();
			});
			std::string err;
			check(!Store::SetEnabled("a", false, err) && !err.empty(), "Busy blocks metadata toggle");
		});
		steps.push_back([this] {
			fixture::cancelAfterCommit = true;
			Store::SaveAsync(
				{channel("a", "cancelled-write"), channel("b")}, receiver, [this](Store::Result r) {
					check(!r.success && r.rollbackComplete,
					      "ambiguous cancelled write fails with complete recovery");
					check(fixture::get("manual-rtmp", "a") == "new-a",
					      "backup restored with fresh uncancelled context");
					check(Store::Load()[0].streamKey.empty(), "failed write invalidates cache");
					next();
				});
		});
		steps.push_back([this] {
			fixture::failLoad = true;
			Store::LoadAsync(receiver, [this](Store::Result r) {
				check(!r.success && r.credentialStatus == SecureTokenStore::Status::Locked,
				      "locked differs from missing");
				check(Store::Load()[0].enabled, "locked read preserves enabled intent");
				next();
			});
		});
		steps.push_back([this] {
			fixture::failConfig = true;
			Store::SaveAsync({channel("a", "config-failed"), channel("b")}, receiver,
					 [this](Store::Result r) {
						 check(!r.success && !r.configCommitted && r.rollbackComplete,
						       "config save failure reported and recovered");
						 check(fixture::get("manual-rtmp", "a") == "new-a",
						       "config save failure rolls back credential");
						 check(Store::Load().size() == 2 && Store::Load()[0].displayName == "a",
						       "config save failure restores exact metadata fields");
						 next();
					 });
		});
		steps.push_back([this] {
			fixture::delay = 100;
			Store::SaveAsync(
				{channel("a", "other-profile"), channel("b")}, receiver, [this](Store::Result r) {
					check(!r.success && !r.configCommitted, "profile change rejects queued commit");
					check(fixture::get("manual-rtmp", "a") == "new-a",
					      "profile conflict rolls back key");
					config_remove_value(appStub.config, "Basic", "ProfileDir");
					next();
				});
			QTimer::singleShot(15, receiver, [] {
				config_set_string(appStub.config, "Basic", "ProfileDir", "changed-profile");
			});
		});
		steps.push_back([this] {
			fixture::delay = 100;
			Store::SaveAsync({channel("a", "stale"), channel("b")}, receiver, [this](Store::Result r) {
				check(!r.success && !r.configCommitted && r.rollbackComplete,
				      "CAS rejects external metadata changes");
				check(Store::Load()[0].displayName == "External", "CAS preserves external change");
				check(fixture::get("manual-rtmp", "a") == "new-a", "CAS restores remote write");
				next();
			});
			QTimer::singleShot(15, receiver, [] {
				config_set_string(appStub.config, SECTION, "Channel0.DisplayName", "External");
			});
		});
		steps.push_back([this] {
			fixture::put("kick-client", "id", "old-secret");
			Store::SaveSecretAsync(
				"kick-client", "id", "new-secret", receiver,
				[](std::string &e) {
					e = "provider metadata CAS conflict";
					return false;
				},
				[this](Store::Result r) {
					check(!r.success && r.rollbackComplete,
					      "provider metadata rejection fails transaction");
					check(fixture::get("kick-client", "id") == "old-secret",
					      "provider secret rolled back");
					next();
				});
		});
		steps.push_back([this] {
			Store::SaveSecretAsync(
				"kick-client", "id", "new-secret", receiver,
				[](std::string &error) {
					config_set_string(appStub.config, "MultistreamKick", "ClientId", "id");
					error.clear();
					return true;
				},
				[this](Store::Result r) {
					check(r.success && r.configCommitted && r.deliveryCurrent,
					      "provider secret and client metadata have current durable delivery");
					check(fixture::get("kick-client", "id") == "new-secret",
					      "provider secret committed");
					next();
				});
		});
		steps.push_back([this] {
			auto a = channel("oauth");
			a.platform = StreamPlatform::Twitch;
			a.accountId = "same";
			auto b = a;
			b.id = "oauth2";
			seed({a, b});
			fixture::put("twitch", "same", "rotated-raw-token");
			Store::SaveAsync({b}, receiver, [this](Store::Result r) {
				check(r.success, "shared OAuth output metadata save");
				check(fixture::get("twitch", "same") == "rotated-raw-token",
				      "account referenced by another output retains token");
				next();
			});
		});
		steps.push_back([this] {
			fixture::failRemove = true;
			Store::SaveAsync({}, receiver, [this](Store::Result r) {
				check(!r.success && r.rollbackComplete, "failed OAuth removal aborts metadata commit");
				check(Store::Load().size() == 1 &&
					      fixture::get("twitch", "same") == "rotated-raw-token",
				      "failed OAuth removal preserves output and token");
				next();
			});
		});
		steps.push_back([this] {
			Store::SaveAsync({}, receiver, [this](Store::Result r) {
				check(r.success && Store::Load().empty(), "orphan OAuth removal commits metadata");
				check(fixture::get("twitch", "same").empty(), "orphan OAuth token removed under lease");
				next();
			});
		});
		steps.push_back([this] {
			seed({channel("a")});
			fixture::delay = 100;
			auto *doomed = new QObject;
			bool *called = new bool(false);
			Store::SaveAsync({channel("a", "dead-receiver")}, doomed,
					 [called](Store::Result) { *called = true; });
			QTimer::singleShot(15, receiver, [doomed] { delete doomed; });
			QTimer::singleShot(240, receiver, [this, called] {
				check(!*called, "destroyed receiver gets no callback");
				delete called;
				check(fixture::get("manual-rtmp", "a") == "new-a",
				      "receiver destroyed before decision rolls back");
				check(!Store::IsBusy(), "receiver destruction drains pending transaction");
				next();
			});
		});
		steps.push_back([this] {
			auto *doomed = new QObject;
			auto calls = std::make_shared<int>(0);
			auto decisions = std::make_shared<int>(0);
			Store::SaveAsync(
				{channel("a", "between-queues")}, doomed, [calls](Store::Result) { ++*calls; },
				[doomed, decisions] {
					if ((*decisions)++ == 0) {
						QMetaObject::invokeMethod(
							state->bridge, [doomed] { delete doomed; },
							Qt::QueuedConnection);
					}
					return true;
				});
			QTimer::singleShot(120, receiver, [this, calls] {
				check(*calls == 0,
				      "receiver destroyed after durable decision before delivery drops callback safely");
				check(fixture::get("manual-rtmp", "a") == "between-queues",
				      "delivery destruction preserves already durable commit");
				next();
			});
		});
		steps.push_back([this] {
			auto decisions = std::make_shared<int>(0);
			Store::SaveAsync(
				{channel("a", "post-cas-profile")}, receiver,
				[this](Store::Result r) {
					check(r.success && r.configCommitted && !r.deliveryCurrent,
					      "post-CAS profile change preserves durable success but rejects runtime snapshot");
					check(r.channels.empty() && !r.warning.empty(),
					      "stale delivery has warning and no applicable channels");
					check(fixture::get("manual-rtmp", "a") == "post-cas-profile",
					      "post-CAS profile change does not rollback confirmed secret");
					config_remove_value(appStub.config, "Basic", "ProfileDir");
					next();
				},
				[decisions] {
					if ((*decisions)++ == 0) {
						QMetaObject::invokeMethod(
							state->bridge,
							[] {
								config_set_string(appStub.config, "Basic", "ProfileDir",
										  "post-commit-change");
							},
							Qt::QueuedConnection);
					}
					return true;
				});
		});
		steps.push_back([this] {
			auto live = std::make_shared<bool>(false);
			Store::SaveAsync(
				{channel("a", "post-cas-live")}, receiver,
				[this](Store::Result r) {
					check(r.success && r.configCommitted && !r.deliveryCurrent &&
						      r.channels.empty(),
					      "output starts after CAS: durable save but runtime delivery rejected");
					fixture::put("manual-rtmp", "a", "between-queues");
					next();
				},
				[live] {
					if (!*live) {
						QMetaObject::invokeMethod(
							state->bridge, [live] { *live = true; }, Qt::QueuedConnection);
						return true;
					}
					return false;
				});
		});
		steps.push_back([this] {
			fixture::failSave = true;
			fixture::failRollback = true;
			Store::SaveAsync({channel("a", "incomplete")}, receiver, [this](Store::Result r) {
				check(!r.success && !r.rollbackComplete &&
					      r.error.find("Rollback incomplete") != std::string::npos,
				      "rollback failure is explicit");
				check(Store::Load()[0].streamKey.empty(),
				      "rollback failure never approves stale cached credential");
				fixture::failRollback = false;
				fixture::put("manual-rtmp", "a", "between-queues");
				next();
			});
		});
		steps.push_back([this] {
			fixture::delay = 100;
			Store::SaveAsync({channel("a", "shutdown-write")}, receiver, [](Store::Result) {});
			QTimer::singleShot(15, receiver, [this] {
				Store::Shutdown();
				check(fixture::get("manual-rtmp", "a") == "between-queues",
				      "shutdown rollback completes without GUI decision queue");
				check(!Store::IsBusy(), "shutdown drains transaction");
				next();
			});
		});
		next();
	}
};
int main(int argc, char **argv)
{
	QCoreApplication application(argc, argv);
	Test test;
	QTimer::singleShot(0, test.receiver, [&] { test.setup(); });
	return application.exec();
}
