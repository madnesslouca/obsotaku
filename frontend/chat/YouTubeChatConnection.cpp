/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "YouTubeChatConnection.hpp"

#include <dialogs/MultistreamAccountsDialog.hpp>
#include <oauth/OAuthTokenSet.hpp>
#include <oauth/PlatformOAuthClient.hpp>
#include <utility/MultistreamTaskPool.hpp>

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>

#include "moc_YouTubeChatConnection.cpp"

namespace {
constexpr int MIN_POLL_MS = 5000;
constexpr int BROADCAST_RETRY_MS = 30000;

qint64 CurrentUnixTime()
{
	return QDateTime::currentSecsSinceEpoch();
}
} // namespace

void YouTubeChatConnection::Start()
{
	if (channel.accountId.isEmpty() && channel.address.trimmed().isEmpty())
		return;
	if (!pollTimer) {
		pollTimer = new QTimer(this);
		pollTimer->setSingleShot(true);
		connect(pollTimer, &QTimer::timeout, this, &YouTubeChatConnection::Poll);
	}
	EmitStatus(ChatConnectionState::Connecting);
	ResolveLiveChat();
}

void YouTubeChatConnection::Stop()
{
	StopReconnect();
	if (pollTimer)
		pollTimer->stop();
	liveChatId.clear();
	nextPageToken.clear();
	accessToken.clear();
	accessTokenExpiresAt = 0;
	connected = false;
	primed = false;
	requestInFlight = false;
	EmitStatus(ChatConnectionState::Disconnected);
}

void YouTubeChatConnection::ScheduleRetry(int milliseconds)
{
	if (!pollTimer)
		return;
	pollTimer->start(milliseconds);
}

void YouTubeChatConnection::WithAccessToken(std::function<void(const QString &)> continuation)
{
	if (!accessToken.isEmpty() && CurrentUnixTime() + 60 < accessTokenExpiresAt) {
		continuation(accessToken);
		return;
	}

	const std::string accountId = channel.accountId.toStdString();
	QPointer<YouTubeChatConnection> guard(this);
	MultistreamTaskPool().start([guard, accountId, continuation = std::move(continuation)]() mutable {
		std::string error;
		auto tokens = OAuthTokenSet::Load(StreamPlatform::YouTube, accountId, error);
		if (tokens && tokens->AccessTokenExpired()) {
			const auto registration =
				MultistreamAccountsDialog::RegistrationForPlatform(StreamPlatform::YouTube);
			OAuthTokenSet refreshed;
			if (PlatformOAuthClient::RefreshTokens(StreamPlatform::YouTube, registration, {}, *tokens,
							       refreshed, error)) {
				refreshed.Save(StreamPlatform::YouTube, accountId, error);
				*tokens = std::move(refreshed);
			} else {
				tokens.reset();
			}
		}

		const QString token = tokens ? QString::fromStdString(tokens->accessToken) : QString();
		const qint64 expiresAt = tokens ? tokens->expiresAt : 0;
		if (!guard)
			return;
		QMetaObject::invokeMethod(
			guard.data(),
			[guard, token, expiresAt, continuation = std::move(continuation)]() mutable {
				if (!guard)
					return;
				guard->accessToken = token;
				guard->accessTokenExpiresAt = expiresAt;
				continuation(token);
			},
			Qt::QueuedConnection);
	});
}

void YouTubeChatConnection::ResolveLiveChat()
{
	QPointer<YouTubeChatConnection> guard(this);
	WithAccessToken([this, guard](const QString &token) {
		if (!guard)
			return;
		if (token.isEmpty()) {
			connected = false;
			EmitStatus(ChatConnectionState::MissingCredential);
			return;
		}

		QUrl url(QStringLiteral("https://www.googleapis.com/youtube/v3/liveBroadcasts"));
		QUrlQuery query;
		query.addQueryItem(QStringLiteral("part"), QStringLiteral("snippet"));
		query.addQueryItem(QStringLiteral("broadcastStatus"), QStringLiteral("active"));
		query.addQueryItem(QStringLiteral("broadcastType"), QStringLiteral("all"));
		url.setQuery(query);

		QNetworkRequest request(url);
		request.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(token).toUtf8());
		QNetworkReply *reply = netManager->get(request);
		connect(reply, &QNetworkReply::finished, this, [this, guard, reply]() {
			reply->deleteLater();
			if (!guard)
				return;
			if (reply->error() != QNetworkReply::NoError) {
				connected = false;
				EmitStatus(ChatConnectionState::Failed, reply->errorString());
				ScheduleRetry(BROADCAST_RETRY_MS);
				return;
			}

			const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
			const QJsonArray items = doc.object()[QStringLiteral("items")].toArray();
			for (const QJsonValue &value : items) {
				const QString chatId = value.toObject()[QStringLiteral("snippet")]
							       .toObject()[QStringLiteral("activeLiveChatId")]
							       .toString();
				if (!chatId.isEmpty()) {
					liveChatId = chatId;
					break;
				}
			}

			if (liveChatId.isEmpty()) {
				connected = false;
				EmitStatus(ChatConnectionState::WaitingForBroadcast);
				ScheduleRetry(BROADCAST_RETRY_MS);
				return;
			}

			connected = true;
			primed = false;
			nextPageToken.clear();
			ResetReconnectDelay();
			EmitStatus(ChatConnectionState::Connected, DisplayName());
			Poll();
		});
	});
}

void YouTubeChatConnection::Poll()
{
	if (liveChatId.isEmpty()) {
		ResolveLiveChat();
		return;
	}
	if (requestInFlight)
		return;

	QPointer<YouTubeChatConnection> guard(this);
	WithAccessToken([this, guard](const QString &token) {
		if (!guard || liveChatId.isEmpty())
			return;
		if (token.isEmpty()) {
			connected = false;
			EmitStatus(ChatConnectionState::MissingCredential);
			return;
		}

		QUrl url(QStringLiteral("https://www.googleapis.com/youtube/v3/liveChat/messages"));
		QUrlQuery query;
		query.addQueryItem(QStringLiteral("liveChatId"), liveChatId);
		query.addQueryItem(QStringLiteral("part"), QStringLiteral("snippet,authorDetails"));
		if (!nextPageToken.isEmpty())
			query.addQueryItem(QStringLiteral("pageToken"), nextPageToken);
		url.setQuery(query);

		QNetworkRequest request(url);
		request.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(token).toUtf8());
		requestInFlight = true;
		QNetworkReply *reply = netManager->get(request);
		connect(reply, &QNetworkReply::finished, this, [this, guard, reply]() {
			reply->deleteLater();
			if (!guard)
				return;
			requestInFlight = false;
			if (liveChatId.isEmpty())
				return;

			if (reply->error() != QNetworkReply::NoError) {
				const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
				if (status == 403 || status == 404) {
					liveChatId.clear();
					connected = false;
					EmitStatus(ChatConnectionState::WaitingForBroadcast);
				} else {
					EmitStatus(ChatConnectionState::Failed, reply->errorString());
				}
				ScheduleRetry(BROADCAST_RETRY_MS);
				return;
			}

			const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
			const QJsonObject obj = doc.object();
			nextPageToken = obj[QStringLiteral("nextPageToken")].toString();

			const bool emitMessages = primed;
			primed = true;

			if (emitMessages) {
				for (const QJsonValue &value : obj[QStringLiteral("items")].toArray()) {
					const QJsonObject item = value.toObject();
					const QJsonObject snippet = item[QStringLiteral("snippet")].toObject();
					const QJsonObject author = item[QStringLiteral("authorDetails")].toObject();

					ChatMessage msg = NewMessage();
					msg.messageText = snippet[QStringLiteral("displayMessage")].toString();
					msg.senderName = author[QStringLiteral("displayName")].toString();
					msg.isModerator = author[QStringLiteral("isChatModerator")].toBool();
					msg.isSubscriber = author[QStringLiteral("isChatSponsor")].toBool();
					msg.isBroadcaster = author[QStringLiteral("isChatOwner")].toBool();

					const QString type = snippet[QStringLiteral("type")].toString();
					if (type == QStringLiteral("superChatEvent")) {
						msg.kind = ChatMessageKind::SuperChat;
						const QJsonObject details =
							snippet[QStringLiteral("superChatDetails")].toObject();
						msg.paidAmount = details[QStringLiteral("amountDisplayString")].toString();
						if (msg.paidAmount.isEmpty())
							msg.paidAmount = QString::number(
								details[QStringLiteral("amountMicros")]
										.toVariant()
										.toLongLong() /
									1000000.0,
								'f', 2);
						msg.paidCurrency = details[QStringLiteral("currency")].toString();
						if (msg.messageText.isEmpty())
							msg.messageText = details[QStringLiteral("userComment")].toString();
					} else if (type == QStringLiteral("superStickerEvent")) {
						msg.kind = ChatMessageKind::SuperChat;
						const QJsonObject details =
							snippet[QStringLiteral("superStickerDetails")].toObject();
						msg.paidAmount = details[QStringLiteral("amountDisplayString")].toString();
						msg.paidCurrency = details[QStringLiteral("currency")].toString();
						if (msg.messageText.isEmpty())
							msg.messageText = QStringLiteral("Super Sticker");
					} else if (type == QStringLiteral("memberMilestoneChatEvent") ||
						   type == QStringLiteral("newSponsorEvent") ||
						   type == QStringLiteral("membershipGiftingEvent") ||
						   type == QStringLiteral("giftMembershipReceivedEvent")) {
						msg.kind = ChatMessageKind::Membership;
					}

					msg.roleBadges = BuildRoleBadges(msg);
					if (msg.kind == ChatMessageKind::SuperChat && !msg.paidAmount.isEmpty())
						msg.roleBadges.prepend(msg.paidAmount);

					if (!msg.senderName.isEmpty() && !msg.messageText.isEmpty())
						emit messageReceived(msg);
				}
			}

			const int suggested = obj[QStringLiteral("pollingIntervalMillis")].toInt(MIN_POLL_MS);
			ScheduleRetry(std::max(suggested, MIN_POLL_MS));
		});
	});
}

bool YouTubeChatConnection::CanSend() const
{
	return connected && !liveChatId.isEmpty();
}

bool YouTubeChatConnection::SendText(const QString &text, QString &error)
{
	if (liveChatId.isEmpty()) {
		error = "youtube-not-live";
		return false;
	}

	error.clear();
	QPointer<YouTubeChatConnection> guard(this);
	const QString targetChatId = liveChatId;
	const QString bodyText = text;

	WithAccessToken([this, guard, targetChatId, bodyText](const QString &token) {
		if (!guard)
			return;
		/* Silence here would look like the message was sent: the input
		 * clears either way, so a refusal has to reach the status line. */
		if (token.isEmpty()) {
			EmitStatus(ChatConnectionState::MissingCredential);
			return;
		}
		if (targetChatId.isEmpty()) {
			EmitStatus(ChatConnectionState::WaitingForBroadcast);
			return;
		}

		QUrl url(QStringLiteral("https://www.googleapis.com/youtube/v3/liveChat/messages"));
		QUrlQuery query;
		query.addQueryItem(QStringLiteral("part"), QStringLiteral("snippet"));
		url.setQuery(query);

		QJsonObject snippet;
		snippet[QStringLiteral("liveChatId")] = targetChatId;
		snippet[QStringLiteral("type")] = QStringLiteral("textMessageEvent");
		QJsonObject textDetails;
		textDetails[QStringLiteral("messageText")] = bodyText;
		snippet[QStringLiteral("textMessageDetails")] = textDetails;
		QJsonObject root;
		root[QStringLiteral("snippet")] = snippet;

		QNetworkRequest request(url);
		request.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(token).toUtf8());
		request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

		QNetworkReply *reply = netManager->post(request, QJsonDocument(root).toJson(QJsonDocument::Compact));
		connect(reply, &QNetworkReply::finished, this, [this, guard, reply, bodyText]() {
			reply->deleteLater();
			if (!guard)
				return;
			if (reply->error() != QNetworkReply::NoError) {
				EmitStatus(ChatConnectionState::Failed, reply->errorString());
				return;
			}

			ChatMessage echo = NewMessage();
			echo.senderName = QStringLiteral("You");
			echo.messageText = bodyText;
			echo.isBroadcaster = true;
			echo.roleBadges = BuildRoleBadges(echo);
			emit messageReceived(echo);
		});
	});
	return true;
}
