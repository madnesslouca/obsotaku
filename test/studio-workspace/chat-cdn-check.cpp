// Explicit read-only smoke check of the public emote CDNs, separate from offline tests.
#include <chat/ChatEmotes.hpp>
#include <chat/ChatEventSocket.hpp>
#include <QApplication>
#include <QFile>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	if (argc != 2) {
		return 2;
	}
	ChatEmoteCache cache;
	QTextDocument document;
	ChatMessage twitch;
	twitch.messageText = "Kappa";
	twitch.emotes = {{0, 5, StreamPlatform::Twitch, "25"}};
	ChatMessage kick;
	kick.messageText = "KEKW";
	kick.emotes = {{0, 4, StreamPlatform::Kick, "37226"}};
	bool twitchLoaded = false, kickLoaded = false, websocketWelcome = false;
	ChatEventSocket socket;
	auto inspect = [&] {
		twitchLoaded = cache.Html(twitch, &document, 28).contains("<img src=\"chatemote:");
		kickLoaded = cache.Html(kick, &document, 28).contains("<img src=\"chatemote:");
		if (twitchLoaded && kickLoaded && websocketWelcome) {
			app.quit();
		}
	};
	QObject::connect(&cache, &ChatEmoteCache::imageReady, &app, inspect);
	QObject::connect(&socket, &ChatEventSocket::textReceived, &app, [&](const QByteArray &bytes) {
		const auto json = QJsonDocument::fromJson(bytes).object();
		if (json["metadata"].toObject()["message_type"].toString() == "session_welcome") {
			websocketWelcome = !json["payload"].toObject()["session"].toObject()["id"].toString().isEmpty();
			socket.Stop();
			inspect();
		}
	});
	QTimer::singleShot(0, &app, [&] {
		socket.Open(QUrl(QStringLiteral("wss://eventsub.wss.twitch.tv/ws")));
		inspect();
	});
	QTimer::singleShot(15000, &app, &QApplication::quit);
	app.exec();
	QJsonArray formats;
	for (const auto &format : QImageReader::supportedImageFormats()) {
		formats.append(QString::fromLatin1(format));
	}
	socket.Stop();
	const QJsonObject result{{"passed", twitchLoaded && kickLoaded && websocketWelcome},
				 {"twitch_eventsub_welcome", websocketWelcome},
				 {"twitch_public_emote_decoded", twitchLoaded},
				 {"kick_public_emote_decoded", kickLoaded},
				 {"supported_formats", formats},
				 {"credentials_used", false},
				 {"public_messages_sent", 0}};
	QFile report(QString::fromLocal8Bit(argv[1]));
	if (!report.open(QIODevice::WriteOnly)) {
		return 3;
	}
	report.write(QJsonDocument(result).toJson());
	return twitchLoaded && kickLoaded && websocketWelcome ? 0 : 1;
}
