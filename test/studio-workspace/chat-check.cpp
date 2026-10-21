// Tests use the production Qt dock and providers. All network and credentials are fake.
#include <OBSApp.hpp>
#include <chat/ChatConnection.hpp>
#include <chat/ChatEmotes.hpp>
#include <chat/ChatEventSocket.hpp>
#include <chat/TwitchEventSub.hpp>
#include <QBuffer>
#include <QCryptographicHash>
#include <QPainter>
#include <chat/KickChatConnection.hpp>
#include <chat/TwitchChatConnection.hpp>
#include <chat/YouTubeChatConnection.hpp>
#include <docks/UnifiedChatDock.hpp>
#include <utility/MultistreamChannelStore.hpp>
#include <utility/PlatformIconProvider.hpp>
#include <utility/ChatBadgeIcons.hpp>
#include <QApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <cstring>

static ChatTestApp application;
ChatTestApp *App()
{
	return &application;
}
static std::vector<MultiStreamChannel> storedChannels;
std::vector<MultiStreamChannel> MultistreamChannelStore::Load()
{
	return storedChannels;
}
void OBSDock::closeEvent(QCloseEvent *e)
{
	QDockWidget::closeEvent(e);
}
void OBSDock::showEvent(QShowEvent *e)
{
	QDockWidget::showEvent(e);
}
bool GetDataFilePath(const char *data, std::string &path)
{
	path = std::string(CHAT_SOURCE_DIR) + "/../../frontend/data/" + data;
	return QFile::exists(QString::fromStdString(path));
}
void LoadChatAccessToken(StreamPlatform, const QString &, QObject *, ChatTokenCallback callback)
{
	callback({});
}

class FakeReply : public QNetworkReply {
	QByteArray body;
	qint64 offset = 0;

public:
	FakeReply(const QNetworkRequest &request, QByteArray data, int status, int delay, QObject *parent)
		: QNetworkReply(parent),
		  body(std::move(data))
	{
		setRequest(request);
		setUrl(request.url());
		open(QIODevice::ReadOnly);
		setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
		QTimer::singleShot(delay, this, [this, status] {
			if (isFinished()) {
				return;
			}
			if (status >= 400) {
				setError(QNetworkReply::ContentAccessDenied, "fixture HTTP error");
			}
			if (status == 0) {
				setError(QNetworkReply::TimeoutError, "fixture timeout");
			}
			setFinished(true);
			emit readyRead();
			emit finished();
		});
	}
	void abort() override
	{
		if (!isFinished()) {
			setError(OperationCanceledError, "cancelled");
			setFinished(true);
			emit finished();
		}
	}
	qint64 bytesAvailable() const override { return body.size() - offset + QNetworkReply::bytesAvailable(); }
	qint64 readData(char *target, qint64 size) override
	{
		const qint64 count = qMin(size, body.size() - offset);
		if (!count) {
			return -1;
		}
		std::memcpy(target, body.constData() + offset, count);
		offset += count;
		return count;
	}
};
class FakeNetwork : public QNetworkAccessManager {
public:
	struct Response {
		QByteArray body;
		int status = 200;
		int delay = 0;
	};
	QList<Response> responses;
	QList<QNetworkRequest> requests;
	QList<QByteArray> bodies;

protected:
	QNetworkReply *createRequest(Operation, const QNetworkRequest &request, QIODevice *data) override
	{
		requests.append(request);
		bodies.append(data ? data->readAll() : QByteArray());
		if (responses.isEmpty()) {
			QTest::qFail("Unexpected network request", __FILE__, __LINE__);
			return new FakeReply(request, "{}", 500, 0, this);
		}
		const auto r = responses.takeFirst();
		return new FakeReply(request, r.body, r.status, r.delay, this);
	}
};
static ChatTokenProvider token = [](StreamPlatform, const QString &, QObject *, ChatTokenCallback callback) {
	callback({"fixture-token", "fixture-client"});
};
static ChatChannelRef channel(StreamPlatform platform, QString id = "one")
{
	return {id, platform, "fixture", "Fixture", "123", {}};
}
class KickHarness : public KickChatConnection {
public:
	using KickChatConnection::KickChatConnection;
	void Activate() { BeginSession(); }
};
class TwitchHarness : public TwitchChatConnection {
public:
	using TwitchChatConnection::TwitchChatConnection;
	void Activate()
	{
		BeginSession();
		connected = true;
	}
	bool CanSend() const override { return connected; }
};
class FakeConnection : public ChatConnection {
public:
	using ChatConnection::ChatConnection;
	QStringList sent;
	bool ready = true;
	void Start() override
	{
		BeginSession();
		connected = ready;
		EmitStatus(ready ? ChatConnectionState::Connected : ChatConnectionState::MissingCredential);
	}
	void Stop() override
	{
		EndSession();
		connected = false;
		EmitStatus(ChatConnectionState::Disconnected);
	}
	bool CanSend() const override { return ready && !stopped; }
	bool SendText(const QString &, const QString &id, QString &error) override
	{
		if (!CanSend()) {
			error = "auth";
			return false;
		}
		sent.append(id);
		BeginSend(id);
		return true;
	}
	void Complete(ChatSendState state, QString detail = {})
	{
		FinishSend(sent.last(), state, state == ChatSendState::Delivered ? "remote-id" : QString(), detail);
	}
	void Receive(ChatMessage m)
	{
		m.channelId = channel.channelId;
		m.platform = channel.platform;
		m.channelName = channel.displayName;
		emit messageReceived(m);
	}
};

class ChatIntegrationTest : public QObject {
	Q_OBJECT
	QTemporaryDir directory;
	template<typename T> T *control(UnifiedChatDock &dock, const char *name)
	{
		auto *c = dock.findChild<T *>(name);
		Q_ASSERT(c);
		return c;
	}
	void setupChannels()
	{
		storedChannels.clear();
		for (auto platform : {StreamPlatform::Twitch, StreamPlatform::YouTube, StreamPlatform::Kick}) {
			MultiStreamChannel c;
			c.id = std::to_string(static_cast<int>(platform));
			c.displayName = c.id;
			c.accountId = "123";
			c.chatAddress = "fixture";
			c.platform = platform;
			storedChannels.push_back(c);
		}
	}
private slots:
	void initTestCase()
	{
		QCOMPARE(config_open(&application.config, (directory.path() + "/user.ini").toUtf8(),
				     CONFIG_OPEN_ALWAYS),
			 CONFIG_SUCCESS);
		QFile locale(QStringLiteral(CHAT_SOURCE_DIR "/../../frontend/data/locale/pt-BR.ini"));
		QVERIFY(locale.open(QIODevice::ReadOnly));
		for (auto line : locale.readAll().split('\n')) {
			const int eq = line.indexOf('=');
			if (eq < 0) {
				continue;
			}
			auto value = line.mid(eq + 1).trimmed();
			if (value.startsWith('"')) {
				value = value.mid(1, value.size() - 2);
			}
			application.translations[QString::fromUtf8(line.left(eq))] = QString::fromUtf8(value);
		}
		qRegisterMetaType<ChatSendResult>();
		qRegisterMetaType<ChatMessage>();
		qRegisterMetaType<QList<ChatBroadcast>>();
	}
	void cleanup()
	{
		storedChannels.clear();
		config_close(application.config);
		QFile::remove(directory.path() + "/user.ini");
		config_open(&application.config, (directory.path() + "/user.ini").toUtf8(), CONFIG_OPEN_ALWAYS);
	}
	void cleanupTestCase()
	{
		config_close(application.config);
		application.config = nullptr;
	}
	void deliveryResults_data()
	{
		QTest::addColumn<QByteArray>("body");
		QTest::addColumn<int>("http");
		QTest::addColumn<int>("expected");
		QTest::newRow("ack") << QByteArray(R"({"data":{"is_sent":true,"message_id":"id1"}})") << 200
				     << int(ChatSendState::Delivered);
		QTest::newRow("drop")
			<< QByteArray(R"({"data":{"is_sent":false}})") << 200 << int(ChatSendState::Rejected);
		QTest::newRow("auth") << QByteArray("{}") << 401 << int(ChatSendState::Rejected);
		QTest::newRow("scope") << QByteArray("{}") << 403 << int(ChatSendState::Rejected);
		QTest::newRow("rate") << QByteArray("{}") << 429 << int(ChatSendState::Rejected);
		QTest::newRow("server") << QByteArray("{}") << 500 << int(ChatSendState::Unknown);
		QTest::newRow("timeout") << QByteArray("{}") << 0 << int(ChatSendState::Unknown);
		QTest::newRow("malformed") << QByteArray("invalid") << 200 << int(ChatSendState::Unknown);
		QTest::newRow("missing-ack")
			<< QByteArray(R"({"data":{"message_id":"id1"}})") << 200 << int(ChatSendState::Unknown);
	}
	void deliveryResults()
	{
		QFETCH(QByteArray, body);
		QFETCH(int, http);
		QFETCH(int, expected);
		FakeNetwork net;
		net.responses.append({body, http, 20});
		KickHarness kick(channel(StreamPlatform::Kick), &net, nullptr, token);
		kick.Activate();
		QSignalSpy result(&kick, &ChatConnection::sendFinished), echo(&kick, &ChatConnection::messageReceived);
		QString error;
		QVERIFY(kick.SendText("testing", "request", error));
		QCOMPARE(result.count(), 0);
		QTRY_COMPARE(result.count(), 1);
		const auto r = qvariant_cast<ChatSendResult>(result.first().first());
		QCOMPARE(int(r.state), expected);
		QCOMPARE(r.requestId, "request");
		QCOMPARE(echo.count(), expected == int(ChatSendState::Delivered) ? 1 : 0);
		QCOMPARE(net.requests.size(), 1);
		QCOMPARE(net.requests.first().url().host(), "api.kick.com");
		const auto json = QJsonDocument::fromJson(net.bodies.first()).object();
		QCOMPARE(json["broadcaster_user_id"].toInt(), 123);
		QCOMPARE(json["type"].toString(), "user");
	}
	void kickLimits()
	{
		FakeNetwork net;
		KickHarness kick(channel(StreamPlatform::Kick), &net, nullptr, token);
		kick.Activate();
		QString error;
		QVERIFY(!kick.SendText(QString(501, 'x'), "a", error));
		QCOMPARE(error, "too-long");
		QVERIFY(!kick.SendText(QString::fromUcs4(U"👨‍👩‍👧‍👦").repeated(100), "b", error));
		net.responses.append({R"({"data":{"is_sent":true,"message_id":"combined"}})"});
		QVERIFY(kick.SendText(QString::fromUtf8("a\xcc\x81").repeated(500), "c", error));
		QSignalSpy result(&kick, &ChatConnection::sendFinished);
		QTRY_COMPARE(result.count(), 1);
	}
	void twitchAckAndInjection()
	{
		FakeNetwork net;
		net.responses.append({R"({"data":[{"is_sent":true,"message_id":"twitch-id"}]})"});
		TwitchHarness twitch(channel(StreamPlatform::Twitch), &net, nullptr, token);
		twitch.Activate();
		QSignalSpy spy(&twitch, &ChatConnection::sendFinished);
		QString error;
		QVERIFY(twitch.SendText("hello", "t", error));
		QTRY_COMPARE(spy.count(), 1);
		QCOMPARE(qvariant_cast<ChatSendResult>(spy.first().first()).state, ChatSendState::Delivered);
		QCOMPARE(net.requests.first().rawHeader("Client-Id"), "fixture-client");
		const auto body = QJsonDocument::fromJson(net.bodies.first()).object();
		QCOMPARE(body["sender_id"].toString(), "123");
		MultiStreamChatAggregator aggregator;
		QVERIFY(!aggregator.SendText("none", "hello\r\nPRIVMSG #other :oops", "id", error));
		QCOMPARE(net.requests.size(), 1);
	}
	void stopCancelsCredentialsAndPost()
	{
		FakeNetwork net;
		ChatTokenCallback delayed;
		ChatTokenProvider provider = [&](StreamPlatform, const QString &, QObject *, ChatTokenCallback cb) {
			delayed = cb;
		};
		KickHarness kick(channel(StreamPlatform::Kick), &net, nullptr, provider);
		kick.Activate();
		QSignalSpy result(&kick, &ChatConnection::sendFinished);
		QString error;
		QVERIFY(kick.SendText("hello", "id", error));
		kick.Stop();
		QCOMPARE(result.count(), 1);
		delayed({"late-token", "client"});
		QCOMPARE(net.requests.size(), 0);
		QCOMPARE(qvariant_cast<ChatSendResult>(result.first().first()).state, ChatSendState::Unknown);
		KickHarness posted(channel(StreamPlatform::Kick), &net, nullptr, token);
		posted.Activate();
		net.responses.append({R"({"data":{"is_sent":true,"message_id":"late"}})", 200, 30});
		QSignalSpy second(&posted, &ChatConnection::sendFinished);
		QVERIFY(posted.SendText("hi", "id", error));
		posted.Stop();
		QTest::qWait(60);
		QCOMPARE(second.count(), 1);
	}
	void youtubeMultipleAndSelected()
	{
		const QByteArray listing =
			R"({"items":[{"id":"A","snippet":{"title":"Live A","liveChatId":"chatA"}},{"id":"B","snippet":{"title":"Live B","liveChatId":"chatB"}}]})";
		FakeNetwork net;
		net.responses.append({listing});
		YouTubeChatConnection youtube(channel(StreamPlatform::YouTube), &net, nullptr, token);
		QSignalSpy choices(&youtube, &ChatConnection::broadcastsAvailable);
		youtube.Start();
		QTRY_COMPARE(choices.count(), 1);
		QVERIFY(!youtube.CanSend());
		QCOMPARE(net.requests.size(), 1);
		youtube.Stop();
		auto selected = channel(StreamPlatform::YouTube);
		selected.broadcastId = "B";
		YouTubeChatConnection bound(selected, &net, nullptr, token);
		net.responses.append({listing});
		net.responses.append({R"({"items":[],"pollingIntervalMillis":60000})"});
		bound.Start();
		QTRY_COMPARE(net.requests.size(), 3);
		QVERIFY(bound.CanSend());
		QVERIFY(net.requests.last().url().query().contains("liveChatId=chatB"));
		net.responses.append({R"({"id":"yt-id","snippet":{}})"});
		QSignalSpy sent(&bound, &ChatConnection::sendFinished);
		QString error;
		QVERIFY(bound.SendText("hello", "yt", error));
		QTRY_COMPARE(sent.count(), 1);
		QCOMPARE(QJsonDocument::fromJson(net.bodies.last())
				 .object()["snippet"]
				 .toObject()["liveChatId"]
				 .toString(),
			 "chatB");
		QVERIFY(!bound.SendText(QString(201, 'x'), "too-long", error));
		bound.Stop();
	}
	void youtubePaginationDoesNotChooseFirst()
	{
		FakeNetwork net;
		net.responses.append({R"({"items":[{"id":"A","snippet":{"liveChatId":"a"}}],"nextPageToken":"next"})"});
		net.responses.append({R"({"items":[{"id":"B","snippet":{"liveChatId":"b"}}]})"});
		YouTubeChatConnection yt(channel(StreamPlatform::YouTube), &net, nullptr, token);
		QSignalSpy choices(&yt, &ChatConnection::broadcastsAvailable);
		yt.Start();
		QTRY_COMPARE(choices.count(), 1);
		QCOMPARE(qvariant_cast<QList<ChatBroadcast>>(choices.first()[1]).size(), 2);
		QVERIFY(!yt.CanSend());
		yt.Stop();
	}
	void twitchEventsAndDeletion()
	{
		FakeNetwork net;
		TwitchChatConnection twitch(channel(StreamPlatform::Twitch), &net, nullptr, token);
		QSignalSpy messages(&twitch, &ChatConnection::messageReceived),
			removed(&twitch, &ChatConnection::messageRemoved);
		twitch.ParseIrcLine(
			"@id=m1;user-id=42;display-name=User;badges=moderator/1,subscriber/1 :user!u@h PRIVMSG #fixture :hello <script>");
		QCOMPARE(messages.count(), 1);
		auto m = qvariant_cast<ChatMessage>(messages[0][0]);
		QCOMPARE(m.id, "m1");
		QVERIFY(m.isModerator && m.isSubscriber);
		twitch.ParseIrcLine(
			"@id=e1;msg-id=sub;system-msg=User\\ssubscribed;display-name=User :tmi.twitch.tv USERNOTICE #fixture");
		QCOMPARE(messages.count(), 2);
		m = qvariant_cast<ChatMessage>(messages[1][0]);
		QCOMPARE(m.kind, ChatMessageKind::Membership);
		QCOMPARE(m.eventType, "subscription");
		twitch.ParseIrcLine("@target-msg-id=m1 :tmi.twitch.tv CLEARMSG #fixture :hello");
		QCOMPARE(removed.count(), 1);
		QCOMPARE(removed[0][1].toString(), "m1");
		twitch.ParseIrcLine("@target-user-id=42 :tmi.twitch.tv CLEARCHAT #fixture :user");
		QCOMPARE(removed.count(), 2);
	}
	void aggregatorDedupeAndStartup()
	{
		QList<FakeConnection *> connections;
		MultiStreamChatAggregator aggregator(nullptr, [&](const ChatChannelRef &ref, QNetworkAccessManager *net,
								  QObject *parent) {
			auto c = std::make_unique<FakeConnection>(ref, net, parent);
			connections.append(c.get());
			return c;
		});
		bool readyDuringSignal = false;
		connect(&aggregator, &MultiStreamChatAggregator::statusChanged, this,
			[&](const QString &id, StreamPlatform, ChatConnectionState state, const QString &) {
				if (state == ChatConnectionState::Connected) {
					readyDuringSignal = aggregator.CanSend(id);
				}
			});
		aggregator.SetChannels({channel(StreamPlatform::Twitch, "a"), channel(StreamPlatform::Twitch, "b")});
		QVERIFY(readyDuringSignal);
		QSignalSpy messages(&aggregator, &MultiStreamChatAggregator::messageReceived);
		ChatMessage m;
		m.id = "same";
		m.messageText = "x";
		connections[0]->Receive(m);
		connections[0]->Receive(m);
		connections[1]->Receive(m);
		QCOMPARE(messages.count(), 2);
		aggregator.SetChannels({channel(StreamPlatform::Twitch, "a"), channel(StreamPlatform::Twitch, "b")});
		QCOMPARE(connections.size(), 2);
	}
	void dockDeliveryDraftAndRetry()
	{
		setupChannels();
		QHash<QString, FakeConnection *> connections;
		UnifiedChatDock dock(nullptr,
				     [&](const ChatChannelRef &ref, QNetworkAccessManager *net, QObject *parent) {
					     auto c = std::make_unique<FakeConnection>(ref, net, parent);
					     connections[ref.channelId] = c.get();
					     return c;
				     });
		dock.AutoConnectAccounts();
		auto *targets = control<QComboBox>(dock, "unifiedChatSendPlatform");
		targets->setCurrentIndex(targets->findData("*"));
		auto *input = control<QLineEdit>(dock, "unifiedChatSendInput");
		auto *send = control<QPushButton>(dock, "unifiedChatSendButton");
		input->setText("batch one");
		QVERIFY(send->isEnabled());
		send->click();
		QCOMPARE(input->text(), "batch one");
		QVERIFY(!send->isEnabled());
		const auto ids = connections.keys();
		QCOMPARE(ids.size(), 3);
		connections[ids[0]]->Complete(ChatSendState::Delivered);
		connections[ids[1]]->Complete(ChatSendState::Rejected, "auth");
		connections[ids[2]]->Complete(ChatSendState::Unknown, "network");
		QCOMPARE(input->text(), "batch one");
		QVERIFY(send->isEnabled());
		send->click();
		QCOMPARE(connections[ids[0]]->sent.size(), 1);
		QCOMPARE(connections[ids[1]]->sent.size(), 2);
		QCOMPARE(connections[ids[2]]->sent.size(), 1);
		connections[ids[1]]->Complete(ChatSendState::Delivered);
		QCOMPARE(input->text(), "batch one");
		QVERIFY(!send->isEnabled());
		control<QPushButton>(dock, "unifiedChatNewMessage")->click();
		input->setText("batch two");
		send->click();
		input->setText("a fresh draft");
		for (auto *c : connections) {
			c->Complete(ChatSendState::Delivered);
		}
		QCOMPARE(input->text(), "a fresh draft");
		input->setText("batch three");
		send->click();
		for (auto *c : connections) {
			c->Complete(ChatSendState::Delivered);
		}
		QVERIFY(input->text().isEmpty());
	}
	void dockReadingFiltersAndPreferences()
	{
		setupChannels();
		QList<FakeConnection *> connections;
		auto factory = [&](const ChatChannelRef &ref, QNetworkAccessManager *net, QObject *parent) {
			auto c = std::make_unique<FakeConnection>(ref, net, parent);
			connections.append(c.get());
			return c;
		};
		{
			UnifiedChatDock dock(nullptr, factory);
			dock.resize(500, 850);
			dock.show();
			dock.AutoConnectAccounts();
			auto *view = control<QTextBrowser>(dock, "unifiedChatView");
			auto *activity = control<QTextBrowser>(dock, "unifiedChatActivity");
			ChatMessage m;
			m.id = "normal";
			m.senderName = "Alice";
			m.messageText = "question <script>unsafe</script>";
			connections.first()->Receive(m);
			QTRY_VERIFY(view->toPlainText().contains("<script>unsafe</script>"));
			QVERIFY(!view->toHtml().contains("<script>unsafe</script>"));
			m.id = "paid";
			m.kind = ChatMessageKind::SuperChat;
			m.messageText = "paid event";
			connections.first()->Receive(m);
			QTRY_VERIFY(activity->toPlainText().contains("paid event"));
			QVERIFY(!view->toPlainText().contains("paid event"));
			auto *scroll = control<QCheckBox>(dock, "unifiedChatAutoScroll");
			scroll->setChecked(false);
			const auto before = view->toPlainText();
			m.id = "paused";
			m.kind = ChatMessageKind::Normal;
			m.messageText = "new while reading";
			connections.first()->Receive(m);
			QTest::qWait(70);
			QCOMPARE(view->toPlainText(), before);
			auto *resume = control<QPushButton>(dock, "unifiedChatResume");
			QVERIFY(!resume->isHidden());
			QVERIFY(resume->text().contains("1"));
			resume->click();
			QVERIFY(view->toPlainText().contains("new while reading"));
			QVERIFY(resume->isHidden());
			auto *search = control<QLineEdit>(dock, "unifiedChatSearch");
			search->setText("no-match");
			QVERIFY(view->toPlainText().isEmpty());
			search->clear();
			auto filters = dock.findChildren<QToolButton *>("unifiedChatFilter");
			QVERIFY(!filters.isEmpty());
			filters.first()->setChecked(false);
			dock.AutoConnectAccounts();
			filters = dock.findChildren<QToolButton *>("unifiedChatFilter");
			bool hidden = false;
			for (auto *f : filters) {
				hidden |= !f->isChecked();
			}
			QVERIFY(hidden);
			control<QAction>(dock, "Compact")->setChecked(false);
			control<QAction>(dock, "SplitActivity")->setChecked(false);
			control<QAction>(dock, "PaidEvents")->setChecked(false);
			auto *highlight = control<QLineEdit>(dock, "unifiedChatHighlight");
			highlight->setText("question");
			QMetaObject::invokeMethod(highlight, "editingFinished");
		}
		QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
		config_close(application.config);
		QCOMPARE(config_open(&application.config, (directory.path() + "/user.ini").toUtf8(),
				     CONFIG_OPEN_EXISTING),
			 CONFIG_SUCCESS);
		connections.clear();
		UnifiedChatDock restored(nullptr, factory);
		restored.AutoConnectAccounts();
		QVERIFY(!control<QAction>(restored, "Compact")->isChecked());
		QVERIFY(!control<QAction>(restored, "SplitActivity")->isChecked());
		QVERIFY(!control<QAction>(restored, "PaidEvents")->isChecked());
		QCOMPARE(control<QLineEdit>(restored, "unifiedChatHighlight")->text(), "question");
		bool hidden = false;
		for (auto *f : restored.findChildren<QToolButton *>("unifiedChatFilter")) {
			hidden |= !f->isChecked();
		}
		QVERIFY(hidden);
	}
	void dockBurstAndScrollPause()
	{
		setupChannels();
		FakeConnection *connection = nullptr;
		UnifiedChatDock dock(nullptr,
				     [&](const ChatChannelRef &ref, QNetworkAccessManager *net, QObject *parent) {
					     auto c = std::make_unique<FakeConnection>(ref, net, parent);
					     if (!connection) {
						     connection = c.get();
					     }
					     return c;
				     });
		dock.resize(480, 750);
		dock.show();
		dock.AutoConnectAccounts();
		auto *view = control<QTextBrowser>(dock, "unifiedChatView");
		for (int i = 0; i < 600; ++i) {
			ChatMessage m;
			m.id = QString::number(i);
			m.senderName = "Alice";
			m.messageText = QStringLiteral("Message #%1").arg(i);
			connection->Receive(m);
		}
		QTRY_VERIFY(view->toPlainText().contains("Message #599"));
		QVERIFY(!view->toPlainText().contains("Message #299"));
		QVERIFY(view->toPlainText().contains("Message #300"));
		QVERIFY(view->verticalScrollBar()->maximum() > 0);
		view->verticalScrollBar()->setValue(0);
		QVERIFY(!control<QCheckBox>(dock, "unifiedChatAutoScroll")->isChecked());
		control<QPushButton>(dock, "unifiedChatResume")->click();
		QCOMPARE(view->verticalScrollBar()->value(), view->verticalScrollBar()->maximum());
		auto cursor = view->textCursor();
		cursor.select(QTextCursor::Document);
		view->setTextCursor(cursor);
		QVERIFY(!control<QCheckBox>(dock, "unifiedChatAutoScroll")->isChecked());
		QVERIFY(view->textCursor().hasSelection());
	}
	void dockWithoutAccounts()
	{
		UnifiedChatDock dock;
		dock.AutoConnectAccounts();
		QVERIFY(!control<QLineEdit>(dock, "unifiedChatSendInput")->isEnabled());
		QVERIFY(!control<QLabel>(dock, "unifiedChatSendHint")->text().isEmpty());
		QVERIFY(!control<QPushButton>(dock, "unifiedChatSendButton")->isEnabled());
		QVERIFY(control<QTextBrowser>(dock, "unifiedChatActivity")->parentWidget()->isHidden());
	}

	QByteArray emotePng(int size = 32)
	{
		QImage image(size, size, QImage::Format_ARGB32);
		image.fill(QColor("#f5c43b"));
		QPainter painter(&image);
		painter.setPen(QPen(Qt::black, 2));
		painter.drawEllipse(QPoint(size / 3, size / 3), 1, 2);
		painter.drawEllipse(QPoint(2 * size / 3, size / 3), 1, 2);
		painter.drawArc(size / 4, size / 3, size / 2, size / 2, 180 * 16, 180 * 16);
		painter.end();
		QByteArray bytes;
		QBuffer buffer(&bytes);
		buffer.open(QIODevice::WriteOnly);
		image.save(&buffer, "PNG");
		return bytes;
	}
	void emoteRangesAndSafeUrls()
	{
		ChatMessage m;
		m.messageText = QString::fromUtf8("👋 Kappa Keepo Kappa <img>");
		ChatEmotes::ParseTwitch(m, "25:2-6,14-18/1902:8-12/bad?id:0-1/25:-1-4,999-1000");
		QCOMPARE(m.emotes.size(), 3);
		QCOMPARE(m.emotes[0].start, 3);
		QCOMPARE(m.messageText.mid(m.emotes[0].start, m.emotes[0].length), "Kappa");
		QCOMPARE(ChatEmotes::ImageUrl(m.emotes[0]).host(), "static-cdn.jtvnw.net");
		QVERIFY(ChatEmotes::ImageUrl({0, 1, StreamPlatform::Twitch, "../secrets"}).isEmpty());
		QVERIFY(ChatEmotes::ImageUrl({0, 1, StreamPlatform::Kick, "https://localhost"}).isEmpty());
		m.emotes.clear();
		m.messageText = "hi [emote:123:Smile] <script>";
		ChatEmotes::ParseKick(m);
		QCOMPARE(m.emotes.size(), 1);
		QCOMPARE(ChatEmotes::ImageUrl(m.emotes.first()).host(), "files.kick.com");
	}
	void emoteCacheImagesFallbackAndPreference()
	{
		FakeNetwork net;
		net.responses.append({emotePng()});
		ChatEmoteCache cache(nullptr, &net);
		ChatMessage m;
		m.messageText = "Kappa <script>";
		m.emotes = {{0, 5, StreamPlatform::Twitch, "25"}};
		QTextDocument doc;
		QSignalSpy ready(&cache, &ChatEmoteCache::imageReady);
		QVERIFY(cache.Html(m, &doc, 28).contains("Kappa &lt;script&gt;"));
		QTRY_COMPARE(ready.count(), 1);
		auto html = cache.Html(m, &doc, 28);
		QVERIFY(html.contains("<img src=\"chatemote:"));
		QVERIFY(html.contains("&lt;script&gt;"));
		QVERIFY(!doc.resource(QTextDocument::ImageResource, QUrl(ChatEmotes::ResourceName(m.emotes.first())))
				 .isNull());
		QCOMPARE(net.requests.size(), 1);
		QVERIFY(net.requests.first().rawHeader("Authorization").isEmpty());
		m.emotes = {{0, 5, StreamPlatform::Kick, "123"}};
		net.responses.append({"broken", 404});
		cache.Html(m, &doc, 28);
		QTRY_COMPARE(net.requests.size(), 2);
		QTest::qWait(10);
		QVERIFY(cache.Html(m, &doc, 28).contains("Kappa"));
		QCOMPARE(net.requests.size(), 2); // failed image not hammered
		cache.Stop();
	}
	void emoteOversizeAndInvalidImage_data()
	{
		QTest::addColumn<int>("mode");
		QTest::newRow("pixel-limit") << 0;
		QTest::newRow("byte-limit") << 1;
		QTest::newRow("bad-format") << 2;
		QTest::newRow("redirect") << 3;
	}
	void emoteOversizeAndInvalidImage()
	{
		QFETCH(int, mode);
		FakeNetwork net;
		const auto bytes = mode == 0   ? emotePng(1024)
				   : mode == 1 ? QByteArray(512 * 1024 + 1, 'x')
					       : QByteArray("not an image");
		net.responses.append({bytes, mode == 3 ? 302 : 200});
		ChatEmoteCache cache(nullptr, &net);
		QTextDocument doc;
		ChatMessage m;
		m.messageText = "Kappa";
		m.emotes = {{0, 5, StreamPlatform::Twitch, "25"}};
		QSignalSpy spy(&cache, &ChatEmoteCache::imageReady);
		cache.Html(m, &doc, 28);
		QTest::qWait(30);
		QCOMPARE(spy.count(), 0);
		QVERIFY(!cache.Html(m, &doc, 28).contains("<img"));
		QCOMPARE(net.requests.size(), 1);
	}
	void dockEmoteImagesInBothLayouts()
	{
		setupChannels();
		FakeConnection *connection = nullptr;
		FakeNetwork net;
		net.responses.append({emotePng()});
		UnifiedChatDock dock(
			nullptr,
			[&](const ChatChannelRef &ref, QNetworkAccessManager *n, QObject *parent) {
				auto c = std::make_unique<FakeConnection>(ref, n, parent);
				if (!connection) {
					connection = c.get();
				}
				return c;
			},
			&net);
		dock.AutoConnectAccounts();
		ChatMessage m;
		m.id = "emote";
		m.senderName = "Alice";
		m.messageText = "Kappa";
		m.emotes = {{0, 5, StreamPlatform::Twitch, "25"}};
		connection->Receive(m);
		auto *view = control<QTextBrowser>(dock, "unifiedChatView");
		QTRY_VERIFY(view->toHtml().contains("chatemote:"));
		control<QAction>(dock, "Compact")->setChecked(false);
		QVERIFY(view->toHtml().contains("chatemote:"));
		control<QAction>(dock, "ShowEmotes")->setChecked(false);
		QVERIFY(!view->toHtml().contains("<img src=\"chatemote:"));
		QVERIFY(view->toPlainText().contains("Kappa"));
	}
	void prepareEvents(TwitchEventSub &events)
	{
		events.BeginSession();
		events.watchdog = new QTimer(&events);
		events.watchdog->setSingleShot(true);
		events.current = new ChatEventSocket(&events);
		events.access = {"fixture-token",
				 "fixture-client",
				 {"moderator:read:followers", "channel:read:redemptions"}};
	}
	QByteArray eventEnvelope(const QString &id, const QString &type, const QJsonObject &event)
	{
		return QJsonDocument(
			       QJsonObject{{"metadata", QJsonObject{{"message_type", "notification"},
								    {"message_id", id},
								    {"message_timestamp", "2026-10-19T12:00:00Z"}}},
					   {"payload", QJsonObject{{"subscription", QJsonObject{{"type", type}}},
								   {"event", event}}}})
			.toJson(QJsonDocument::Compact);
	}
	void eventSubSubscriptionsAndTransfer()
	{
		FakeNetwork net;
		for (int i = 0; i < 3; ++i) {
			net.responses.append({R"({"data":[{"status":"enabled"}]})", 202});
		}
		TwitchEventSub events(channel(StreamPlatform::Twitch), &net, nullptr, token);
		prepareEvents(events);
		const QByteArray welcome =
			R"({"metadata":{"message_type":"session_welcome"},"payload":{"session":{"id":"session1","keepalive_timeout_seconds":30}}})";
		events.Handle(events.current, welcome);
		QTRY_VERIFY(events.IsConnected());
		QCOMPARE(net.requests.size(), 3);
		QSet<QString> types;
		for (const auto &bytes : net.bodies) {
			const auto body = QJsonDocument::fromJson(bytes).object();
			types.insert(body["type"].toString());
			QCOMPARE(body["transport"].toObject()["session_id"].toString(), "session1");
			if (body["type"].toString() == "channel.follow") {
				QCOMPARE(body["condition"].toObject()["moderator_user_id"].toString(), "123");
			}
		}
		QVERIFY(types.contains("channel.follow"));
		QVERIFY(types.contains("channel.channel_points_automatic_reward_redemption.add"));
		events.Handle(events.current, welcome);
		QCOMPARE(net.requests.size(), 3); // duplicate welcome cannot duplicate subscriptions
		events.pending = new ChatEventSocket(&events);
		auto *replacement = events.pending.data();
		events.Handle(
			replacement,
			R"({"metadata":{"message_type":"session_welcome"},"payload":{"session":{"id":"session2","keepalive_timeout_seconds":30}}})");
		QCOMPARE(events.current.data(), replacement);
		QVERIFY(!events.pending);
		QCOMPARE(net.requests.size(), 3);
		QCOMPARE(events.sessionId, "session2");
		events.Stop();
	}
	void eventSubFollowPointsDedupeAndRouting()
	{
		FakeNetwork net;
		TwitchEventSub events(channel(StreamPlatform::Twitch), &net, nullptr, token);
		prepareEvents(events);
		QSignalSpy received(&events, &ChatConnection::messageReceived);
		QJsonObject event{{"broadcaster_user_id", "123"}, {"user_id", "42"}, {"user_name", "Alice"}};
		auto follow = eventEnvelope("one", "channel.follow", event);
		events.Handle(events.current, follow);
		events.Handle(events.current, follow);
		QCOMPARE(received.count(), 1);
		auto m = qvariant_cast<ChatMessage>(received[0][0]);
		QCOMPARE(m.kind, ChatMessageKind::Follow);
		QCOMPARE(m.eventType, "follow");
		event["reward"] = QJsonObject{{"title", "Choose a song"}, {"cost", 500}};
		event["user_input"] = "My song";
		events.Handle(events.current,
			      eventEnvelope("two", "channel.channel_points_custom_reward_redemption.add", event));
		QCOMPARE(received.count(), 2);
		m = qvariant_cast<ChatMessage>(received[1][0]);
		QCOMPARE(m.eventCount, 500);
		QCOMPARE(m.messageText, "My song");
		event["reward"] = QJsonObject{{"type", "send_highlighted_message"}, {"channel_points", 100}};
		event["message"] =
			QJsonDocument::fromJson(
				R"({"fragments":[{"type":"text","text":"hello "},{"type":"emote","text":"Kappa","emote":{"id":"25"}}]})")
				.object();
		events.Handle(events.current,
			      eventEnvelope("three", "channel.channel_points_automatic_reward_redemption.add", event));
		QCOMPARE(received.count(), 3);
		m = qvariant_cast<ChatMessage>(received[2][0]);
		QCOMPARE(m.emotes.size(), 1);
		QCOMPARE(m.emotes[0].start, 6);
		event["broadcaster_user_id"] = "another-account";
		events.Handle(events.current, eventEnvelope("wrong", "channel.follow", event));
		QCOMPARE(received.count(), 3);
		events.Stop();
		events.Handle(nullptr, follow);
		QCOMPARE(received.count(), 3);
	}
	void eventSubMissingPermissions()
	{
		FakeNetwork net;
		TwitchEventSub events(channel(StreamPlatform::Twitch), &net, nullptr, token);
		QSignalSpy states(&events, &ChatConnection::statusChanged);
		events.Start();
		QVERIFY(events.NeedsCredentials());
		QCOMPARE(net.requests.size(), 0);
		QVERIFY(!events.current);
		QCOMPARE(qvariant_cast<ChatConnectionState>(states.last()[2]), ChatConnectionState::MissingCredential);
	}
	void eventSocketValidationAndFrames()
	{
		QVERIFY(ChatEventSocket::AllowedUrl(QUrl("wss://eventsub.wss.twitch.tv/ws?token=opaque")));
		for (const auto &url :
		     {"ws://eventsub.wss.twitch.tv/ws", "wss://127.0.0.1/ws", "wss://evil.test/ws",
		      "wss://user@eventsub.wss.twitch.tv/ws", "wss://eventsub.wss.twitch.tv:123/ws"}) {
			QVERIFY(!ChatEventSocket::AllowedUrl(QUrl(url)));
		}
		ChatEventSocket socket;
		socket.running = true;
		socket.upgraded = true;
		QSignalSpy messages(&socket, &ChatEventSocket::textReceived), closed(&socket, &ChatEventSocket::closed);
		socket.buffer = QByteArray::fromHex("0102") + "he";
		socket.Frames();
		QCOMPARE(messages.count(), 0);
		socket.buffer = QByteArray::fromHex("8003") + "llo";
		socket.Frames();
		QCOMPARE(messages.count(), 1);
		QCOMPARE(messages[0][0].toByteArray(), "hello");
		socket.buffer = QByteArray::fromHex("818000000000");
		socket.Frames();
		QCOMPARE(closed.count(), 1);
		QVERIFY(!socket.running);
		ChatEventSocket handshake;
		handshake.running = true;
		handshake.key = "fixture-key";
		const auto accept = QCryptographicHash::hash(handshake.key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11",
							     QCryptographicHash::Sha1)
					    .toBase64();
		handshake.buffer =
			"HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Accept: " +
			accept + "\r\n\r\n";
		QVERIFY(handshake.Handshake());
		QVERIFY(handshake.upgraded);
	}
	void dockNewActivityFilters()
	{
		setupChannels();
		QList<FakeConnection *> connections;
		UnifiedChatDock dock(nullptr,
				     [&](const ChatChannelRef &ref, QNetworkAccessManager *net, QObject *parent) {
					     auto c = std::make_unique<FakeConnection>(ref, net, parent);
					     connections.append(c.get());
					     return c;
				     });
		dock.AutoConnectAccounts();
		auto *feed = control<QTextBrowser>(dock, "unifiedChatActivity");
		ChatMessage event;
		event.senderName = "Alice";
		event.id = "follow";
		event.kind = ChatMessageKind::Follow;
		event.eventType = "follow";
		connections.first()->Receive(event);
		QTRY_VERIFY(feed->toPlainText().contains("Começou a seguir"));
		control<QAction>(dock, "FollowEvents")->setChecked(false);
		QVERIFY(!feed->toPlainText().contains("Começou a seguir"));
		event.id = "raid";
		event.kind = ChatMessageKind::Raid;
		event.eventType = "raid";
		event.eventCount = 25;
		connections.first()->Receive(event);
		QTRY_VERIFY(feed->toPlainText().contains("25 espectadores"));
		event.id = "points";
		event.kind = ChatMessageKind::ChannelPoints;
		event.eventType = "points";
		event.eventTitle = "Tocar música";
		event.eventCount = 500;
		connections.first()->Receive(event);
		QTRY_VERIFY(feed->toPlainText().contains("Tocar música"));
		control<QAction>(dock, "PointEvents")->setChecked(false);
		QVERIFY(!feed->toPlainText().contains("Tocar música"));
		control<QAction>(dock, "DisableAllEvents")->trigger();
		QVERIFY(feed->toPlainText().isEmpty());
	}

	void youtubeGiftsMilestonesAndEmptyEvents()
	{
		FakeNetwork net;
		net.responses.append({R"({"items":[{"id":"A","snippet":{"liveChatId":"chatA"}}]})"});
		net.responses.append({R"({"items":[],"pollingIntervalMillis":60000})"});
		YouTubeChatConnection yt(channel(StreamPlatform::YouTube), &net, nullptr, token);
		yt.Start();
		QTRY_VERIFY(yt.primed);
		QVERIFY(!yt.requestInFlight);
		net.responses.append({R"({"items":[
		 {"id":"gift","authorDetails":{"displayName":"Alice"},"snippet":{"type":"membershipGiftingEvent","membershipGiftingDetails":{"giftMembershipsCount":5,"giftMembershipsLevelName":"Gold"}}},
		 {"id":"received","authorDetails":{"displayName":"Bob"},"snippet":{"type":"giftMembershipReceivedEvent","giftMembershipReceivedDetails":{"memberLevelName":"Gold"}}},
		 {"id":"milestone","authorDetails":{"displayName":"Carla"},"snippet":{"type":"memberMilestoneChatEvent","memberMilestoneChatDetails":{"memberMonth":12,"userComment":"Hello"}}},
		 {"id":"new","authorDetails":{"displayName":"Diego"},"snippet":{"type":"newSponsorEvent","newSponsorDetails":{"memberLevelName":"Silver"}}}
		],"pollingIntervalMillis":60000})"});
		QSignalSpy messages(&yt, &ChatConnection::messageReceived);
		yt.Poll();
		QTRY_COMPARE(messages.count(), 4);
		auto gift = qvariant_cast<ChatMessage>(messages[0][0]);
		QCOMPARE(gift.eventType, "membership-gift");
		QCOMPARE(gift.eventCount, 5);
		QCOMPARE(qvariant_cast<ChatMessage>(messages[1][0]).eventType, "membership-received");
		const auto milestone = qvariant_cast<ChatMessage>(messages[2][0]);
		QCOMPARE(milestone.eventCount, 12);
		QCOMPARE(milestone.messageText, "Hello");
		QCOMPARE(qvariant_cast<ChatMessage>(messages[3][0]).eventType, "subscription");
		yt.Stop();
	}
	void sharedActivityConnectionPerAccount()
	{
		MultiStreamChatAggregator aggregator(nullptr, [](const ChatChannelRef &ref, QNetworkAccessManager *net,
								 QObject *parent) {
			return std::make_unique<FakeConnection>(ref, net, parent);
		});
		auto a = channel(StreamPlatform::Twitch, "a"), b = channel(StreamPlatform::Twitch, "b");
		aggregator.SetChannels({a, b});
		QCOMPARE(aggregator.findChildren<TwitchEventSub *>().size(), 1);
		aggregator.SetChannels({a});
		QCOMPARE(aggregator.findChildren<TwitchEventSub *>().size(), 1);
		b.accountId = "456";
		aggregator.SetChannels({a, b});
		QCOMPARE(aggregator.findChildren<TwitchEventSub *>().size(), 2);
	}
	void eventSubRefusalAndPartialPermissions()
	{
		FakeNetwork net;
		TwitchEventSub events(channel(StreamPlatform::Twitch), &net, nullptr, token);
		prepareEvents(events);
		events.access.scopes = {"moderator:read:followers"};
		net.responses.append({R"({"data":[{"status":"enabled"}]})", 202});
		QSignalSpy states(&events, &ChatConnection::statusChanged);
		events.Handle(
			events.current,
			R"({"metadata":{"message_type":"session_welcome"},"payload":{"session":{"id":"session"}}})");
		QTRY_VERIFY(events.IsConnected());
		QCOMPARE(net.requests.size(), 1);
		QCOMPARE(states.last()[3].toString(), "events-partial");
		events.Stop();
		TwitchEventSub refused(channel(StreamPlatform::Twitch), &net, nullptr, token);
		prepareEvents(refused);
		for (int i = 0; i < 3; ++i) {
			net.responses.append({"{}", i == 0 ? 401 : 500});
		}
		refused.Handle(
			refused.current,
			R"({"metadata":{"message_type":"session_welcome"},"payload":{"session":{"id":"denied"}}})");
		QTRY_VERIFY(refused.NeedsCredentials());
		QVERIFY(!refused.current);
		QVERIFY(!refused.IsConnected());
	}
	void emoteMetadataUpdatesAndReadingPause()
	{
		setupChannels();
		FakeNetwork net;
		net.responses.append({emotePng(), 200, 100});
		FakeConnection *c = nullptr;
		UnifiedChatDock dock(
			nullptr,
			[&](const ChatChannelRef &ref, QNetworkAccessManager *n, QObject *parent) {
				auto item = std::make_unique<FakeConnection>(ref, n, parent);
				if (!c) {
					c = item.get();
				}
				return item;
			},
			&net);
		dock.AutoConnectAccounts();
		ChatMessage m;
		m.id = "ack-echo";
		m.senderName = "Alice";
		m.messageText = "Kappa";
		c->Receive(m);
		QSignalSpy appended(dock.Aggregator(), &MultiStreamChatAggregator::messageReceived),
			updated(dock.Aggregator(), &MultiStreamChatAggregator::messageUpdated);
		m.emotes = {{0, 5, StreamPlatform::Twitch, "25"}};
		c->Receive(m);
		QCOMPARE(appended.count(), 0);
		QCOMPARE(updated.count(), 1);
		auto *view = control<QTextBrowser>(dock, "unifiedChatView");
		QTRY_COMPARE(net.requests.size(), 1);
		control<QCheckBox>(dock, "unifiedChatAutoScroll")->setChecked(false);
		const auto text = view->toPlainText();
		QTest::qWait(160);
		QCOMPARE(view->toPlainText(), text);
		control<QPushButton>(dock, "unifiedChatResume")->click();
		QVERIFY(view->toHtml().contains("chatemote:"));
		QCOMPARE(view->toPlainText().count("Alice"), 1);
	}

	void emojiPickerKeepsDraft()
	{
		setupChannels();
		UnifiedChatDock dock(nullptr,
				     [](const ChatChannelRef &ref, QNetworkAccessManager *net, QObject *parent) {
					     return std::make_unique<FakeConnection>(ref, net, parent);
				     });
		dock.AutoConnectAccounts();
		auto *input = control<QLineEdit>(dock, "unifiedChatSendInput");
		input->setText("Olá ");
		auto *choice = control<QPushButton>(dock, "unifiedChatEmojiChoice");
		const auto emoji = choice->property("emoji").toString();
		choice->click();
		QCOMPARE(input->text(), QStringLiteral("Olá ") + emoji);
		QVERIFY(control<QToolButton>(dock, "unifiedChatEmojiButton")->menu());
	}

	void dockPreview()
	{
		const auto output = qEnvironmentVariable("STUDIO_CHAT_PREVIEW");
		if (output.isEmpty()) {
			return;
		}
		setupChannels();
		QList<FakeConnection *> connections;
		FakeNetwork emoteNetwork;
		emoteNetwork.responses.append({emotePng()});
		emoteNetwork.responses.append({emotePng()});
		const QStringList names{"Twitch · canal principal", "YouTube · estúdio", "Kick · comunidade"};
		for (int i = 0; i < 3; ++i) {
			storedChannels[i].displayName = names[i].toStdString();
		}
		UnifiedChatDock dock(
			nullptr,
			[&](const ChatChannelRef &ref, QNetworkAccessManager *net, QObject *parent) {
				auto c = std::make_unique<FakeConnection>(ref, net, parent);
				connections.append(c.get());
				return c;
			},
			&emoteNetwork);
		dock.setStyleSheet(
			"QWidget { background:#1d1d23; color:#eeeeef; } QTextBrowser, QLineEdit, QComboBox { background:#141419; color:#eee; border:1px solid #383840; border-radius:5px; padding:5px; } QPushButton, QToolButton { background:#303039; border:1px solid #454550; padding:6px; border-radius:5px; } QToolButton:checked { background:#4b3279; } QScrollBar:vertical { background:#232329; width:10px; } QScrollBar::handle:vertical { background:#535362; min-height:20px; }");
		QPalette palette = dock.palette();
		palette.setColor(QPalette::Base, QColor("#141419"));
		palette.setColor(QPalette::Text, QColor("#eee"));
		dock.setPalette(palette);
		dock.resize(560, 860);
		dock.show();
		dock.AutoConnectAccounts();
		const QStringList senders{"Ana", "Bruno", "Carla", "Diego", "Ana", "Rafa"};
		const QStringList texts{"O áudio está ótimo!",
					"Dá para separar a música do VOD?",
					"Consigo acompanhar pelo Linux também?",
					"Sim! O chat agora reúne todos os canais.",
					"Gostei da pausa para ler as mensagens.",
					"Vamos testar os efeitos VST3 na próxima cena!"};
		for (int i = 0; i < texts.size(); ++i) {
			ChatMessage m;
			m.id = QString::number(i);
			m.senderName = senders[i];
			m.messageText = texts[i];
			m.timestamp = "19:42";
			m.isModerator = i == 3;
			m.roleBadges = BuildRoleBadges(m);
			if (i == 0) {
				m.messageText += QStringLiteral(" Kappa");
				m.emotes = {{int(m.messageText.size()) - 5, 5, StreamPlatform::Twitch, "25"}};
			}
			if (i == 2) {
				m.messageText += QStringLiteral(" [emote:37226:KEKW]");
				ChatEmotes::ParseKick(m);
			}
			connections[i % 3]->Receive(m);
		}
		ChatMessage event;
		event.id = "event";
		event.senderName = "Marina";
		event.messageText = "Obrigada pela live!";
		event.kind = ChatMessageKind::SuperChat;
		event.paidAmount = "R$ 10,00";
		event.timestamp = "19:43";
		connections[1]->Receive(event);
		event.id = "event2";
		event.kind = ChatMessageKind::Membership;
		event.senderName = "Tiago";
		event.messageText = "Inscreveu-se no canal";
		event.paidAmount.clear();
		connections[0]->Receive(event);
		event.id = "follow-demo";
		event.kind = ChatMessageKind::Follow;
		event.eventType = "follow";
		event.senderName = "Júlia";
		event.messageText.clear();
		connections[0]->Receive(event);
		event.id = "raid-demo";
		event.kind = ChatMessageKind::Raid;
		event.eventType = "raid";
		event.senderName = "Rafa";
		event.eventCount = 25;
		connections[0]->Receive(event);
		event.id = "points-demo";
		event.kind = ChatMessageKind::ChannelPoints;
		event.eventType = "points";
		event.senderName = "João";
		event.eventTitle = "Tocar música";
		event.eventCount = 500;
		connections[0]->Receive(event);
		dock.Aggregator()->activityStatusChanged(QString::fromStdString(storedChannels[0].id),
							 ChatConnectionState::Connected,
							 QStringLiteral("events-connected"));
		auto *target = control<QComboBox>(dock, "unifiedChatSendPlatform");
		target->setCurrentIndex(target->findData("*"));
		control<QLineEdit>(dock, "unifiedChatSendInput")->setText("Obrigado por acompanhar!");
		QTest::qWait(100);
		QTRY_VERIFY(control<QTextBrowser>(dock, "unifiedChatView")->toHtml().contains("chatemote:"));
		QVERIFY(dock.grab().save(output));
	}
};
QTEST_MAIN(ChatIntegrationTest)
#include "chat-check.moc"
