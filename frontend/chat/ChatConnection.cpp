/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "ChatConnection.hpp"

#include <QDateTime>

#include <algorithm>

#include "moc_ChatConnection.cpp"

namespace {
constexpr int MAX_RECONNECT_DELAY_MS = 60000;
constexpr int INITIAL_RECONNECT_DELAY_MS = 5000;
} // namespace

QString ChatCurrentTimestamp()
{
	return QDateTime::currentDateTime().toString(QStringLiteral("hh:mm"));
}

QStringList BuildRoleBadges(const ChatMessage &message)
{
	QStringList badges;
	if (message.isBroadcaster)
		badges << QStringLiteral("HOST");
	if (message.isModerator)
		badges << QStringLiteral("MOD");
	if (message.isVip)
		badges << QStringLiteral("VIP");
	if (message.isSubscriber)
		badges << QStringLiteral("SUB");
	return badges;
}

ChatConnection::ChatConnection(ChatChannelRef channelRef, QNetworkAccessManager *manager, QObject *parent)
	: QObject(parent),
	  channel(std::move(channelRef)),
	  netManager(manager)
{
	reconnectTimer = new QTimer(this);
	reconnectTimer->setSingleShot(true);
	connect(reconnectTimer, &QTimer::timeout, this, &ChatConnection::Start);
}

bool ChatConnection::SameTargetAs(const ChatChannelRef &other) const
{
	return channel.channelId == other.channelId && channel.platform == other.platform &&
	       channel.address.trimmed().compare(other.address.trimmed(), Qt::CaseInsensitive) == 0 &&
	       channel.accountId == other.accountId;
}

bool ChatConnection::SendText(const QString &, QString &error)
{
	error = "unsupported";
	return false;
}

void ChatConnection::EmitStatus(ChatConnectionState state, const QString &detail)
{
	emit statusChanged(channel.channelId, channel.platform, state, detail);
}

void ChatConnection::ScheduleReconnect()
{
	if (channel.address.trimmed().isEmpty())
		return;
	reconnectTimer->start(reconnectDelayMs);
	reconnectDelayMs = std::min(reconnectDelayMs * 2, MAX_RECONNECT_DELAY_MS);
}

void ChatConnection::ResetReconnectDelay()
{
	reconnectDelayMs = INITIAL_RECONNECT_DELAY_MS;
}

void ChatConnection::StopReconnect()
{
	reconnectTimer->stop();
}

QString ChatConnection::DisplayName() const
{
	return channel.displayName.isEmpty() ? channel.address : channel.displayName;
}

ChatMessage ChatConnection::NewMessage() const
{
	ChatMessage message;
	message.channelId = channel.channelId;
	message.platform = channel.platform;
	message.channelName = DisplayName();
	message.timestamp = ChatCurrentTimestamp();
	return message;
}
