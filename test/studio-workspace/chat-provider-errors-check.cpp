/* Offline regression of the real YouTube provider. FakeNetwork never opens a
 * socket; all access tokens and response bodies below are synthetic. */
#include <chat/YouTubeChatConnection.hpp>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QThread>
#include <QTimer>
#include <QUrlQuery>
#include <functional>
#include <algorithm>
#include <cstring>
#include <cstdio>

void LoadChatAccessToken(StreamPlatform, const QString &, QObject *, ChatTokenCallback callback)
{
	callback({}); // Production credential adapter is deliberately not linked.
}

namespace {
struct Response {
	QByteArray body;
	int status = 200;
	int delay = 0;
};
class FakeReply final : public QNetworkReply {
	QByteArray body;
	qint64 offset = 0;
public:
	FakeReply(const QNetworkRequest &request, Response response, QObject *parent) : QNetworkReply(parent), body(response.body)
	{
		setRequest(request);
		setUrl(request.url());
		setAttribute(QNetworkRequest::HttpStatusCodeAttribute, response.status);
		if (response.status >= 400 || response.status == 0)
			setError(response.status == 0 ? RemoteHostClosedError
				 : response.status == 401 ? AuthenticationRequiredError
				 : response.status == 403 ? ContentAccessDenied
				 : response.status == 404 ? ContentNotFoundError : ServiceUnavailableError,
				 "Synthetic HTTP failure PRIVATE_ERROR_BODY");
		open(QIODevice::ReadOnly);
		QTimer::singleShot(response.delay, this, [this] { setFinished(true); emit readyRead(); emit finished(); });
	}
	void abort() override { setError(OperationCanceledError, "cancelled"); setFinished(true); }
	qint64 bytesAvailable() const override { return body.size() - offset + QNetworkReply::bytesAvailable(); }
protected:
	qint64 readData(char *data, qint64 maximum) override
	{
		const qint64 count = std::min(maximum, qint64(body.size()) - offset);
		if (count <= 0)
			return -1;
		std::memcpy(data, body.constData() + offset, size_t(count));
		offset += count;
		return count;
	}
};
class FakeNetwork final : public QNetworkAccessManager {
public:
	QList<Response> responses;
	QList<QNetworkRequest> requests;
	bool unexpected = false;
protected:
	QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request, QIODevice *) override
	{
		requests.append(request);
		if (operation != GetOperation || responses.isEmpty()) {
			unexpected = true;
			return new FakeReply(request, {"{}", 500}, this);
		}
		return new FakeReply(request, responses.takeFirst(), this);
	}
};
const QByteArray listing = R"({"items":[{"id":"broadcast-A","snippet":{"title":"Fixture","liveChatId":"chat-A"}}]})";
const QByteArray prime = R"({"items":[],"nextPageToken":"page-after-prime","pollingIntervalMillis":60000})";
QByteArray message(const char *id, int interval = 60000)
{
	return QJsonDocument(QJsonObject{{"nextPageToken", "page-after-message"}, {"pollingIntervalMillis", interval},
		{"items", QJsonArray{QJsonObject{{"id", id}, {"snippet", QJsonObject{{"type", "textMessageEvent"}, {"displayMessage", "fixture text"}}},
			{"authorDetails", QJsonObject{{"displayName", "Fixture author"}}}}}}}).toJson();
}
Response failure(const QString &reason, int status, int delay = 0)
{
	return {QJsonDocument(QJsonObject{{"error", QJsonObject{{"message", "PRIVATE_ERROR_BODY SECRET_FIXTURE_ID"},
		{"errors", QJsonArray{QJsonObject{{"reason", reason}}}}}}}).toJson(), status, delay};
}
ChatChannelRef target()
{
	return {"fixture-channel", StreamPlatform::YouTube, "fixture-account", "Fixture", "123", {}};
}
const ChatTokenProvider token = [](StreamPlatform, const QString &, QObject *, ChatTokenCallback callback) {
	callback({"fixture-token", "fixture-client"});
};
void settle(int milliseconds = 10)
{
	QEventLoop loop;
	QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
	loop.exec();
}
bool waitFor(const std::function<bool()> &ready)
{
	QElapsedTimer elapsed;
	elapsed.start();
	while (!ready() && elapsed.elapsed() < 1500) {
		QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
		QThread::msleep(1);
	}
	return ready();
}
} // namespace

class ChatProviderErrorsTest {
	QJsonArray checks;
	bool passed = true;
	void check(bool ok, const QString &label)
	{
		checks.append(QJsonObject{{"check", label}, {"passed", ok}});
		passed &= ok;
	}
public:
	QJsonObject run()
	{
		struct Case { const char *reason; int status; bool suspended; ChatConnectionState state; const char *detail; };
		const QList<Case> cases{{"quotaExceeded", 403, true, ChatConnectionState::Failed, "quota"},
			{"dailyLimitExceeded", 403, true, ChatConnectionState::Failed, "quota"},
			{"forbidden", 403, true, ChatConnectionState::Failed, "permissions"},
			{"insufficientPermissions", 403, true, ChatConnectionState::Failed, "permissions"},
			{"", 403, true, ChatConnectionState::Failed, "permissions"},
			{"", 401, true, ChatConnectionState::MissingCredential, "authentication"},
			{"liveChatDisabled", 403, true, ChatConnectionState::Failed, "disabled"},
			{"liveChatEnded", 403, false, ChatConnectionState::WaitingForBroadcast, "not available"},
			{"notFound", 404, false, ChatConnectionState::WaitingForBroadcast, "not available"},
			{"liveChatNotFound", 404, false, ChatConnectionState::WaitingForBroadcast, "not available"},
			{"rateLimitExceeded", 403, false, ChatConnectionState::Failed, "too frequent"},
			{"", 429, false, ChatConnectionState::Failed, "too frequent"},
			{"", 503, false, ChatConnectionState::Failed, "Retrying"},
			{"", 0, false, ChatConnectionState::Failed, "Retrying"}};
		for (const bool discovery : {true, false}) {
			for (const auto &item : cases) {
				const QString label = QString("%1 HTTP%2 %3").arg(discovery ? "discovery" : "poll").arg(item.status).arg(item.reason);
				FakeNetwork net;
				YouTubeChatConnection provider(target(), &net, nullptr, token);
				ChatConnectionState state = ChatConnectionState::Disconnected;
				QString detail;
				int messages = 0, connectedEvents = 0;
				QObject::connect(&provider, &ChatConnection::statusChanged, [&](const QString &, StreamPlatform, ChatConnectionState value, const QString &text) {
					state = value; detail = text; if (value == ChatConnectionState::Connected) ++connectedEvents;
				});
				QObject::connect(&provider, &ChatConnection::messageReceived, [&](const ChatMessage &) { ++messages; });
				if (!discovery) {
					net.responses.append({listing}); net.responses.append({prime});
					provider.Start();
					check(waitFor([&] { return net.requests.size() == 2 && !provider.requestInFlight; }), label + " precondition");
					provider.pollTimer->stop();
				}
				net.responses.append(failure(item.reason, item.status));
				const auto before = net.requests.size();
				if (discovery) provider.Start(); else provider.Poll();
				check(waitFor([&] { return net.requests.size() > before && !provider.resolving && !provider.requestInFlight; }), label + " reply completed");
				check(state == item.state, label + " classified state");
				check(detail.contains(item.detail, Qt::CaseInsensitive) && !detail.contains("PRIVATE") && !detail.contains("SECRET"), label + " short safe diagnostic");
				check(!provider.IsConnected() && !provider.CanSend(), label + " no false connected state");
				check(provider.pollingSuspended == item.suspended, label + " suspension policy");
				check(provider.pollTimer->isActive() == !item.suspended, label + " automatic timer policy");
				if (item.suspended) {
					const auto requests = net.requests.size();
					provider.Poll(); provider.ResolveLiveChat(); settle();
					check(net.requests.size() == requests, label + " no automatic rediscovery or polling while suspended");
				} else {
					const int retry = !discovery && QString(item.detail) == "too frequent" ? 60000 : 30000;
					check(provider.pollTimer->interval() == retry, label + " recoverable retry interval respects server limit");
					provider.pollTimer->stop();
				}
				if (item.state == ChatConnectionState::WaitingForBroadcast)
					check(provider.liveChatId.isEmpty() && provider.nextPageToken.isEmpty(), label + " expired chat cleared");
				if (!discovery && (item.status == 503 || QString(item.detail) == "too frequent"))
					check(provider.liveChatId == "chat-A" && provider.nextPageToken == "page-after-prime", label + " transient error preserves chat and cursor");
				const int oldConnected = connectedEvents;
				net.responses.append({listing}); net.responses.append({message("old-backlog")});
				provider.Stop(); provider.Start();
				check(waitFor([&] { return provider.IsConnected() && !provider.requestInFlight && !provider.resolving; }), label + " Stop Start recovers");
				check(connectedEvents == oldConnected + 1 && messages == 0 && provider.primed, label + " recovery emits one Connected and suppresses old backlog");
				check(!provider.pollingSuspended && provider.nextPageToken == "page-after-message", label + " restart clears suspended state and establishes fresh cursor");
				provider.pollTimer->stop();
				net.responses.append({message("fresh-message")}); provider.Poll();
				check(waitFor([&] { return messages == 1 && !provider.requestInFlight; }), label + " new message delivered once after recovery");
				provider.Stop(); settle();
				check(!net.unexpected, label + " no unplanned network operations");
			}
		}
		// A transient Poll failure resumes the same cursor, without discovery or
		// throwing away the first fresh message as though it were backlog.
		{
			FakeNetwork net;
			YouTubeChatConnection provider(target(), &net, nullptr, token);
			int messages = 0, connected = 0;
			QObject::connect(&provider, &ChatConnection::messageReceived, [&](const ChatMessage &) { ++messages; });
			QObject::connect(&provider, &ChatConnection::statusChanged, [&](const QString &, StreamPlatform, ChatConnectionState state, const QString &) { if (state == ChatConnectionState::Connected) ++connected; });
			net.responses.append({listing}); net.responses.append({prime}); provider.Start();
			check(waitFor([&] { return net.requests.size() == 2 && !provider.requestInFlight; }), "transient recovery precondition");
			provider.pollTimer->stop(); net.responses.append(failure({}, 503)); provider.Poll();
			check(waitFor([&] { return net.requests.size() == 3 && !provider.requestInFlight; }), "transient failure completed");
			provider.pollTimer->stop(); net.responses.append({message("after-transient")}); provider.Poll();
			check(waitFor([&] { return messages == 1 && !provider.requestInFlight; }), "transient retry delivers first fresh message once");
			check(provider.IsConnected() && connected == 2 && provider.primed, "transient retry restores Connected without replaying backlog");
			check(net.requests.size() == 4 && net.requests.last().url().path().endsWith("/liveChat/messages") &&
			      QUrlQuery(net.requests.last().url()).queryItemValue("pageToken") == "page-after-prime", "transient retry preserves cursor and avoids rediscovery");
			check(provider.pollTimer->interval() == 60000 && !net.unexpected, "transient retry respects new server interval");
			provider.Stop();
		}
		// Stopping in a synchronous discovery observer cannot resurrect the
		// old session or start its first Poll after Stop returned.
		{
			FakeNetwork net;
			YouTubeChatConnection provider(target(), &net, nullptr, token);
			QObject::connect(&provider, &ChatConnection::broadcastsAvailable, [&] { provider.Stop(); });
			net.responses.append({listing}); provider.Start(); settle();
			check(net.requests.size() == 1 && !net.unexpected, "Stop during broadcast choices prevents obsolete first Poll");
			check(!provider.IsConnected() && provider.liveChatId.isEmpty() && !provider.pollTimer->isActive(), "Stop during discovery leaves clean disconnected state");
		}
		// A message observer can stop synchronously: the rest of that response
		// must not emit messages or arm a timer belonging to the stopped session.
		{
			FakeNetwork net;
			YouTubeChatConnection provider(target(), &net, nullptr, token);
			int messages = 0;
			QObject::connect(&provider, &ChatConnection::messageReceived, [&](const ChatMessage &) { ++messages; provider.Stop(); });
			net.responses.append({listing}); net.responses.append({prime}); provider.Start();
			check(waitFor([&] { return net.requests.size() == 2 && !provider.requestInFlight; }), "message Stop precondition");
			provider.pollTimer->stop();
			auto batch = QJsonDocument::fromJson(message("batch-one")).object();
			auto items = batch["items"].toArray(); auto second = items[0].toObject(); second["id"] = "batch-two"; items.append(second); batch["items"] = items;
			net.responses.append({QJsonDocument(batch).toJson()}); provider.Poll(); settle();
			check(messages == 1 && !provider.IsConnected() && !provider.pollTimer->isActive(), "Stop in message callback suppresses obsolete remainder and timer");
			check(!net.unexpected, "message Stop issues no unplanned requests");
		}
		{
			FakeNetwork net;
			YouTubeChatConnection provider(target(), &net, nullptr, token);
			ChatConnectionState state = ChatConnectionState::Disconnected;
			QObject::connect(&provider, &ChatConnection::statusChanged, [&](const QString &, StreamPlatform, ChatConnectionState value, const QString &) { state = value; });
			net.responses.append({R"({"items":[]})"}); provider.Start(); settle();
			check(state == ChatConnectionState::WaitingForBroadcast && !provider.pollingSuspended, "successful empty discovery remains waiting for a live broadcast");
			check(provider.pollTimer->isActive() && provider.pollTimer->interval() == 30000 && net.requests.size() == 1, "no-live discovery retains existing retry policy");
			provider.Stop();
		}
		// Explicit Start on a suspended provider must invalidate old read callbacks.
		{
			FakeNetwork net;
			YouTubeChatConnection provider(target(), &net, nullptr, token);
			net.responses.append({listing}); net.responses.append({prime}); provider.Start();
			check(waitFor([&] { return net.requests.size() == 2 && !provider.requestInFlight; }), "overlap precondition");
			provider.pollTimer->stop();
			net.responses.append(failure("quotaExceeded", 403, 5)); provider.Poll();
			net.responses.append({R"({"items":[],"nextPageToken":"stale-next-page"})", 200, 30}); provider.ResolveLiveChat();
			check(waitFor([&] { return provider.pollingSuspended; }), "quota suspends overlapping discovery");
			settle(50);
			check(net.requests.size() == 4 && provider.pollingSuspended && !provider.IsConnected(), "late pagination cannot resurrect suspended reads");
			net.responses.append({listing}); net.responses.append({prime}); provider.Start();
			check(waitFor([&] { return net.requests.size() == 6 && provider.IsConnected() && !provider.requestInFlight; }), "explicit Start resumes suspended provider without Stop");
			check(!net.unexpected && provider.primed && !provider.resolving && !provider.pollingSuspended, "explicit Start has clean timers flags and read generation");
			provider.Stop();
		}
		// Keep polling intervals exactly as the existing provider policy specifies.
		for (const int interval : {1000, 5000, 60000}) {
			FakeNetwork net;
			YouTubeChatConnection provider(target(), &net, nullptr, token);
			net.responses.append({listing}); net.responses.append({message("prime", interval)}); provider.Start();
			check(waitFor([&] { return net.requests.size() == 2 && !provider.requestInFlight; }), QString("interval %1 precondition").arg(interval));
			check(provider.pollTimer->isActive() && provider.pollTimer->interval() == std::max(interval, 5000), QString("interval %1 policy unchanged").arg(interval));
			provider.Stop();
		}
		// Token-provider callbacks from a stopped session cannot cause requests.
		{
			FakeNetwork net;
			QList<ChatTokenCallback> tokens;
			ChatTokenProvider delayed = [&](StreamPlatform, const QString &, QObject *, ChatTokenCallback callback) { tokens.append(callback); };
			YouTubeChatConnection provider(target(), &net, nullptr, delayed);
			provider.Start(); provider.Stop(); provider.Start();
			check(tokens.size() == 2 && net.requests.isEmpty(), "deferred token adapter controls both sessions");
			tokens[0]({"obsolete-fixture-token", "fixture-client"}); settle();
			check(net.requests.isEmpty(), "old session token callback cannot start network discovery");
			net.responses.append({listing}); net.responses.append({prime}); tokens[1]({"fixture-token", "fixture-client"});
			check(waitFor([&] { return tokens.size() == 3; }), "fresh discovery requests token for first Poll");
			tokens[2]({"fixture-token", "fixture-client"});
			check(waitFor([&] { return net.requests.size() == 2 && !provider.requestInFlight; }), "current token session completes discovery and Poll");
			check(!net.unexpected && provider.IsConnected(), "token provider behavior preserved without credential adapter");
			provider.Stop();
		}
		return {{"passed", passed}, {"checks", checks}, {"check_count", checks.size()},
			{"scope", "Offline real YouTubeChatConnection/ChatConnection with synthetic replies and tokens; no API account, socket or external message send; platform/native execution provenance is supplied by the runner."}};
	}
};

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	ChatProviderErrorsTest test;
	const auto result = test.run();
	const QByteArray json = QJsonDocument(result).toJson();
	if (argc > 1) {
		QFile file(QString::fromLocal8Bit(argv[1]));
		if (!file.open(QIODevice::WriteOnly) || file.write(json) != json.size()) return 2;
	}
	std::fwrite(json.constData(), 1, size_t(json.size()), stdout);
	return result["passed"].toBool() ? 0 : 1;
}
