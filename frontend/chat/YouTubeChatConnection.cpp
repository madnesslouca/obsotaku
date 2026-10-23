/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "YouTubeChatConnection.hpp"

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

} // namespace

void YouTubeChatConnection::Start()
{
	if (channel.accountId.isEmpty() && channel.address.trimmed().isEmpty()) {
		return;
	}
	if (!pollTimer) {
		pollTimer = new QTimer(this);
		pollTimer->setSingleShot(true);
		connect(pollTimer, &QTimer::timeout, this, &YouTubeChatConnection::Poll);
	}
	if (!stopped && !pollingSuspended) {
		return;
	}
	BeginSession();
	++readGeneration;
	pollingSuspended = false;
	resolving = false;
	requestInFlight = false;
	connected = false;
	primed = false;
	lastPollIntervalMs = MIN_POLL_MS;
	liveChatId.clear();
	nextPageToken.clear();
	broadcasts.clear();
	pollTimer->stop();
	EmitStatus(ChatConnectionState::Connecting);
	ResolveLiveChat();
}

void YouTubeChatConnection::Stop()
{
	EndSession();
	++readGeneration;
	pollingSuspended = false;
	resolving = false;
	broadcasts.clear();
	if (pollTimer) {
		pollTimer->stop();
	}
	liveChatId.clear();
	nextPageToken.clear();

	connected = false;
	primed = false;
	requestInFlight = false;
	EmitStatus(ChatConnectionState::Disconnected);
}

void YouTubeChatConnection::ScheduleRetry(int milliseconds)
{
	if (!pollTimer || stopped || pollingSuspended) {
		return;
	}
	pollTimer->start(milliseconds);
}

void YouTubeChatConnection::SuspendPolling(ChatConnectionState state, const QString &detail)
{
	pollingSuspended = true;
	++readGeneration;
	connected = false;
	resolving = false;
	requestInFlight = false;
	if (pollTimer) {
		pollTimer->stop();
	}
	// Suspend reads only. An already accepted send retains its own result;
	// an ambiguous send must never be automatically retried or cancelled here.
	EmitStatus(state, detail);
}

bool YouTubeChatConnection::HandleApiError(QNetworkReply *reply)
{
	const auto serial = readGeneration;
	const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	if (reply->error() == QNetworkReply::NoError && status < 400) {
		return false;
	}
	QStringList reasons;
	const auto error = QJsonDocument::fromJson(reply->readAll()).object()[QStringLiteral("error")].toObject();
	for (const auto &value : error[QStringLiteral("errors")].toArray()) {
		reasons.append(value.toObject()[QStringLiteral("reason")].toString().toLower());
	}
	connected = false;
	if (reasons.contains(QStringLiteral("quotaexceeded")) ||
	    reasons.contains(QStringLiteral("dailylimitexceeded"))) {
		SuspendPolling(ChatConnectionState::Failed,
			       QStringLiteral("YouTube API quota exhausted. Chat polling paused; reconnect to retry."));
	} else if (status == 401) {
		SuspendPolling(ChatConnectionState::MissingCredential,
			       QStringLiteral("YouTube authentication required. Reconnect after signing in."));
	} else if (reasons.contains(QStringLiteral("livechatdisabled"))) {
		SuspendPolling(ChatConnectionState::Failed,
			       QStringLiteral("YouTube live chat is disabled. Reconnect after enabling it."));
	} else if (status == 404 || reasons.contains(QStringLiteral("livechatended")) ||
		   reasons.contains(QStringLiteral("livechatnotfound")) || reasons.contains(QStringLiteral("notfound"))) {
		liveChatId.clear();
		nextPageToken.clear();
		primed = false;
		EmitStatus(ChatConnectionState::WaitingForBroadcast,
			   QStringLiteral("YouTube live chat is not available. Waiting for a broadcast."));
		if (serial == readGeneration) {
			ScheduleRetry(BROADCAST_RETRY_MS);
		}
	} else if (status == 429 || reasons.contains(QStringLiteral("ratelimitexceeded"))) {
		EmitStatus(ChatConnectionState::Failed,
			   QStringLiteral("YouTube chat requests are too frequent. Retrying shortly."));
		if (serial == readGeneration) {
			ScheduleRetry(std::max(BROADCAST_RETRY_MS, lastPollIntervalMs));
		}
	} else if (status == 403) {
		SuspendPolling(ChatConnectionState::Failed,
			       QStringLiteral("YouTube chat access denied. Check account permissions, then reconnect."));
	} else {
		EmitStatus(ChatConnectionState::Failed, QStringLiteral("YouTube chat request failed. Retrying shortly."));
		if (serial == readGeneration) {
			ScheduleRetry(BROADCAST_RETRY_MS);
		}
	}
	return true;
}

void YouTubeChatConnection::ResolveLiveChat(const QString &pageToken)
{
	if (stopped || pollingSuspended || resolving) {
		return;
	}
	resolving = true;
	if (pageToken.isEmpty()) {
		broadcasts.clear();
	}
	const auto serial = readGeneration;
	WithAccessToken([this, pageToken, serial](const ChatAccessToken &access) {
		if (stopped || pollingSuspended || serial != readGeneration) {
			return;
		}
		if (access.token.isEmpty()) {
			resolving = false;
			connected = false;
			EmitStatus(ChatConnectionState::MissingCredential);
			return;
		}
		QUrl url(QStringLiteral("https://www.googleapis.com/youtube/v3/liveBroadcasts"));
		QUrlQuery query;
		query.addQueryItem(QStringLiteral("part"), QStringLiteral("snippet"));
		query.addQueryItem(QStringLiteral("broadcastStatus"), QStringLiteral("active"));
		query.addQueryItem(QStringLiteral("broadcastType"), QStringLiteral("all"));
		query.addQueryItem(QStringLiteral("maxResults"), QStringLiteral("50"));
		if (!pageToken.isEmpty()) {
			query.addQueryItem(QStringLiteral("pageToken"), pageToken);
		}
		url.setQuery(query);
		QNetworkRequest request(url);
		request.setTransferTimeout(15000);
		request.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(access.token).toUtf8());
		auto *reply = netManager->get(request);
		TrackReply(reply);
		connect(reply, &QNetworkReply::finished, this, [this, reply, pageToken, serial] {
			reply->deleteLater();
			if (stopped || pollingSuspended || serial != readGeneration) {
				return;
			}
			resolving = false;
			if (HandleApiError(reply)) {
				return;
			}
			const auto root = QJsonDocument::fromJson(reply->readAll()).object();
			for (const auto &value : root[QStringLiteral("items")].toArray()) {
				const auto item = value.toObject();
				const auto snippet = item[QStringLiteral("snippet")].toObject();
				const auto chatId = snippet[QStringLiteral("liveChatId")].toString(
					snippet[QStringLiteral("activeLiveChatId")].toString());
				if (!chatId.isEmpty()) {
					broadcasts.append({item[QStringLiteral("id")].toString(),
							   snippet[QStringLiteral("title")].toString(), chatId});
				}
			}
			const QString next = root[QStringLiteral("nextPageToken")].toString();
			if (!next.isEmpty()) {
				// Refuse automatic selection if pagination is inconsistent.
				if (next == pageToken || broadcasts.size() > 1000) {
					EmitStatus(ChatConnectionState::Failed);
					ScheduleRetry(BROADCAST_RETRY_MS);
					return;
				}
				ResolveLiveChat(next);
				return;
			}
			emit broadcastsAvailable(channel.channelId, broadcasts);
			if (stopped || pollingSuspended || serial != readGeneration) {
				return;
			}
			liveChatId.clear();
			for (const auto &broadcast : broadcasts) {
				if (broadcast.id == channel.broadcastId ||
				    (channel.broadcastId.isEmpty() && broadcasts.size() == 1)) {
					liveChatId = broadcast.liveChatId;
					break;
				}
			}
			if (liveChatId.isEmpty()) {
				connected = false;
				EmitStatus(broadcasts.size() > 1 && channel.broadcastId.isEmpty()
						   ? ChatConnectionState::SelectBroadcast
						   : ChatConnectionState::WaitingForBroadcast);
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
	if (stopped || pollingSuspended) {
		return;
	}
	if (liveChatId.isEmpty()) {
		ResolveLiveChat();
		return;
	}
	if (requestInFlight) {
		return;
	}

	requestInFlight = true;
	const auto serial = readGeneration;
	WithAccessToken([this, serial](const ChatAccessToken &access) {
		if (stopped || pollingSuspended || serial != readGeneration) {
			return;
		}
		const auto &token = access.token;
		if (liveChatId.isEmpty()) {
			return;
		}
		if (token.isEmpty()) {
			requestInFlight = false;
			connected = false;
			EmitStatus(ChatConnectionState::MissingCredential);
			return;
		}

		QUrl url(QStringLiteral("https://www.googleapis.com/youtube/v3/liveChat/messages"));
		QUrlQuery query;
		query.addQueryItem(QStringLiteral("liveChatId"), liveChatId);
		query.addQueryItem(QStringLiteral("part"), QStringLiteral("snippet,authorDetails"));
		if (!nextPageToken.isEmpty()) {
			query.addQueryItem(QStringLiteral("pageToken"), nextPageToken);
		}
		url.setQuery(query);

		QNetworkRequest request(url);
		request.setTransferTimeout(15000);
		request.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(token).toUtf8());
		requestInFlight = true;
		QNetworkReply *reply = netManager->get(request);
		TrackReply(reply);
		connect(reply, &QNetworkReply::finished, this, [this, reply, serial]() {
			reply->deleteLater();
			if (stopped || pollingSuspended || serial != readGeneration) {
				return;
			}
			requestInFlight = false;
			if (liveChatId.isEmpty()) {
				return;
			}

			if (HandleApiError(reply)) {
				return;
			}

			const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
			const QJsonObject obj = doc.object();
			if (!connected) {
				connected = true;
				EmitStatus(ChatConnectionState::Connected, DisplayName());
				if (stopped || pollingSuspended || serial != readGeneration) {
					return;
				}
			}
			nextPageToken = obj[QStringLiteral("nextPageToken")].toString();

			const bool emitMessages = primed;
			primed = true;

			if (emitMessages) {
				for (const QJsonValue &value : obj[QStringLiteral("items")].toArray()) {
					if (stopped || pollingSuspended || serial != readGeneration) {
						return;
					}
					const QJsonObject item = value.toObject();
					const QJsonObject snippet = item[QStringLiteral("snippet")].toObject();
					const QJsonObject author = item[QStringLiteral("authorDetails")].toObject();

					ChatMessage msg = NewMessage();
					msg.id = item[QStringLiteral("id")].toString();
					msg.senderId = author[QStringLiteral("channelId")].toString();
					const auto published = QDateTime::fromString(
						snippet[QStringLiteral("publishedAt")].toString(), Qt::ISODate);
					if (published.isValid()) {
						msg.timestamp =
							published.toLocalTime().toString(QStringLiteral("hh:mm"));
					}
					msg.messageText = snippet[QStringLiteral("displayMessage")].toString();
					msg.senderName = author[QStringLiteral("displayName")].toString();
					msg.isModerator = author[QStringLiteral("isChatModerator")].toBool();
					msg.isSubscriber = author[QStringLiteral("isChatSponsor")].toBool();
					msg.isBroadcaster = author[QStringLiteral("isChatOwner")].toBool();

					const QString type = snippet[QStringLiteral("type")].toString();
					if (type == QStringLiteral("messageDeletedEvent")) {
						emit messageRemoved(
							channel.channelId,
							snippet[QStringLiteral("messageDeletedDetails")]
								.toObject()[QStringLiteral("deletedMessageId")]
								.toString(),
							{});
						continue;
					}
					if (type == QStringLiteral("superChatEvent")) {
						msg.kind = ChatMessageKind::SuperChat;
						const QJsonObject details =
							snippet[QStringLiteral("superChatDetails")].toObject();
						msg.paidAmount =
							details[QStringLiteral("amountDisplayString")].toString();
						if (msg.paidAmount.isEmpty()) {
							msg.paidAmount =
								QString::number(details[QStringLiteral("amountMicros")]
												.toVariant()
												.toLongLong() /
											1000000.0,
										'f', 2);
						}
						msg.paidCurrency = details[QStringLiteral("currency")].toString();
						if (msg.messageText.isEmpty()) {
							msg.messageText =
								details[QStringLiteral("userComment")].toString();
						}
					} else if (type == QStringLiteral("superStickerEvent")) {
						msg.kind = ChatMessageKind::SuperChat;
						const QJsonObject details =
							snippet[QStringLiteral("superStickerDetails")].toObject();
						msg.paidAmount =
							details[QStringLiteral("amountDisplayString")].toString();
						msg.paidCurrency = details[QStringLiteral("currency")].toString();
						if (msg.messageText.isEmpty()) {
							msg.messageText = QStringLiteral("Super Sticker");
						}
					} else if (type == QStringLiteral("memberMilestoneChatEvent") ||
						   type == QStringLiteral("newSponsorEvent") ||
						   type == QStringLiteral("membershipGiftingEvent") ||
						   type == QStringLiteral("giftMembershipReceivedEvent")) {
						msg.kind = ChatMessageKind::Membership;
						if (type == "membershipGiftingEvent") {
							msg.eventType = "membership-gift";
							const auto details =
								snippet["membershipGiftingDetails"].toObject();
							msg.eventCount = details["giftMembershipsCount"].toInt();
							msg.eventTitle = details["giftMembershipsLevelName"].toString();
						} else if (type == "giftMembershipReceivedEvent") {
							msg.eventType = "membership-received";
							msg.eventTitle = snippet["giftMembershipReceivedDetails"]
										 .toObject()["memberLevelName"]
										 .toString();
						} else if (type == "memberMilestoneChatEvent") {
							msg.eventType = "resubscription";
							const auto details =
								snippet["memberMilestoneChatDetails"].toObject();
							msg.eventCount = details["memberMonth"].toInt();
							msg.eventTitle = details["memberLevelName"].toString();
							if (msg.messageText.isEmpty()) {
								msg.messageText = details["userComment"].toString();
							}
						} else {
							msg.eventType = "subscription";
							msg.eventTitle = snippet["newSponsorDetails"]
										 .toObject()["memberLevelName"]
										 .toString();
						}
					}

					msg.roleBadges = BuildRoleBadges(msg);
					if (msg.kind == ChatMessageKind::SuperChat && !msg.paidAmount.isEmpty()) {
						msg.roleBadges.prepend(msg.paidAmount);
					}

					if (!msg.senderName.isEmpty() &&
					    (!msg.messageText.isEmpty() || !msg.eventType.isEmpty())) {
						emit messageReceived(msg);
					}
				}
			}

			if (!stopped && !pollingSuspended && serial == readGeneration) {
				const int suggested = obj[QStringLiteral("pollingIntervalMillis")].toInt(MIN_POLL_MS);
				lastPollIntervalMs = std::max(suggested, MIN_POLL_MS);
				ScheduleRetry(lastPollIntervalMs);
			}
		});
	});
}

bool YouTubeChatConnection::CanSend() const
{
	return connected && !liveChatId.isEmpty();
}

bool YouTubeChatConnection::SendText(const QString &text, const QString &requestId, QString &error)
{
	if (stopped || !CanSend()) {
		error = "youtube-not-live";
		return false;
	}
	if (text.toUcs4().size() > 200) {
		error = "too-long";
		return false;
	}
	BeginSend(requestId);
	const QString target = liveChatId;
	WithAccessToken([this, target, text, requestId](const ChatAccessToken &access) {
		if (access.token.isEmpty()) {
			FinishSend(requestId, ChatSendState::Rejected, {}, QStringLiteral("auth"));
			return;
		}
		QUrl url(QStringLiteral("https://www.googleapis.com/youtube/v3/liveChat/messages?part=snippet"));
		QNetworkRequest request(url);
		request.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(access.token).toUtf8());
		const QJsonObject snippet{{"liveChatId", target},
					  {"type", "textMessageEvent"},
					  {"textMessageDetails", QJsonObject{{"messageText", text}}}};
		PostMessage(requestId, text, request, {{"snippet", snippet}});
	});
	error.clear();
	return true;
}
