/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "UnifiedChatDock.hpp"

#include <utility/MultistreamChannelStore.hpp>

#include <OBSApp.hpp>
#include <qt-wrappers.hpp>

#include <QColor>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

#include "moc_UnifiedChatDock.cpp"

namespace {
/* Keeps the dock bounded: a busy chat would otherwise grow the document
 * without limit for the whole broadcast. */
constexpr int MAX_CHAT_BLOCKS = 500;

QString PlatformName(StreamPlatform platform)
{
	const auto name = GetStreamPlatformInfo(platform).displayName;
	return QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size()));
}

QString PlatformColor(StreamPlatform platform)
{
	const auto color = GetStreamPlatformInfo(platform).brandColor;
	return QString::fromUtf8(color.data(), static_cast<qsizetype>(color.size()));
}

/* Colors arrive from chat tags and platform JSON, i.e. from other users. They
 * are interpolated into a style attribute, so anything but a plain hex color
 * has to be rejected instead of escaped. */
QString SafeUserColor(const QString &color, StreamPlatform platform)
{
	static const QRegularExpression hexColor(QStringLiteral("^#[0-9a-fA-F]{6}$"));
	if (hexColor.match(color).hasMatch())
		return color;
	return PlatformColor(platform);
}

QString BadgeHtml(StreamPlatform platform)
{
	const QColor color(PlatformColor(platform));
	const QString rgb =
		QStringLiteral("%1,%2,%3").arg(color.red()).arg(color.green()).arg(color.blue());

	return QStringLiteral("<span style=\"background-color:rgba(%1,0.18); border:1px solid rgba(%1,0.4); "
			      "border-radius:4px; padding:1px 5px; font-weight:bold; font-size:10px;\">%2</span>")
		.arg(rgb, PlatformName(platform).toHtmlEscaped());
}

QString StatusText(ChatConnectionState state, const QString &platformName, const QString &detail)
{
	switch (state) {
	case ChatConnectionState::Connecting:
		return QTStr("Multistream.Chat.Connecting").arg(platformName);
	case ChatConnectionState::Connected:
		return QTStr("Multistream.Chat.Connected").arg(platformName);
	case ChatConnectionState::Disconnected:
		return QTStr("Multistream.Chat.Disconnected").arg(platformName);
	case ChatConnectionState::MissingCredential:
		return QTStr("Multistream.Chat.MissingAccount").arg(platformName);
	case ChatConnectionState::WaitingForBroadcast:
		return QTStr("Multistream.Chat.WaitingForBroadcast").arg(platformName);
	case ChatConnectionState::Unsupported:
		return QTStr("Multistream.Chat.Unsupported").arg(platformName);
	case ChatConnectionState::Failed:
		return detail.isEmpty() ? QTStr("Multistream.Chat.Failed").arg(platformName)
					: QTStr("Multistream.Chat.FailedDetail").arg(platformName, detail);
	}
	return {};
}
} // namespace

UnifiedChatDock::UnifiedChatDock(QWidget *parent) : OBSDock(parent)
{
	setObjectName(QStringLiteral("unifiedChatDock"));
	setWindowTitle(QTStr("Multistream.Chat.Title"));

	aggregator = new MultiStreamChatAggregator(this);

	auto *mainWidget = new QWidget(this);
	mainWidget->setObjectName(QStringLiteral("unifiedChatDockContent"));

	auto *layout = new QVBoxLayout(mainWidget);
	layout->setContentsMargins(8, 8, 8, 8);
	layout->setSpacing(6);

	auto *toolbar = new QHBoxLayout();
	toolbar->setContentsMargins(0, 0, 0, 0);
	toolbar->setSpacing(8);

	showTwitch = new QCheckBox(QStringLiteral("⯀ Twitch"), mainWidget);
	showTwitch->setObjectName(QStringLiteral("chatFilterTwitch"));
	showTwitch->setChecked(true);

	showYouTube = new QCheckBox(QStringLiteral("▶ YouTube"), mainWidget);
	showYouTube->setObjectName(QStringLiteral("chatFilterYouTube"));
	showYouTube->setChecked(true);

	showKick = new QCheckBox(QStringLiteral("K Kick"), mainWidget);
	showKick->setObjectName(QStringLiteral("chatFilterKick"));
	showKick->setChecked(true);

	autoScroll = new QCheckBox(QTStr("Multistream.Chat.AutoScroll"), mainWidget);
	autoScroll->setChecked(true);

	clearButton = new QPushButton(QTStr("Multistream.Chat.Clear"), mainWidget);

	toolbar->addWidget(showTwitch);
	toolbar->addWidget(showYouTube);
	toolbar->addWidget(showKick);
	toolbar->addStretch();
	toolbar->addWidget(autoScroll);
	toolbar->addWidget(clearButton);

	layout->addLayout(toolbar);

	chatView = new QTextBrowser(mainWidget);
	chatView->setObjectName(QStringLiteral("unifiedChatView"));
	chatView->setOpenExternalLinks(false);
	chatView->setOpenLinks(false);
	chatView->setReadOnly(true);
	layout->addWidget(chatView, 1);

	statusLabel = new QLabel(QTStr("Multistream.Chat.Ready"), mainWidget);
	statusLabel->setObjectName(QStringLiteral("unifiedChatStatus"));
	layout->addWidget(statusLabel);

	setWidget(mainWidget);

	connect(aggregator, &MultiStreamChatAggregator::messageReceived, this, &UnifiedChatDock::OnChatMessage);
	connect(aggregator, &MultiStreamChatAggregator::statusChanged, this, &UnifiedChatDock::OnStatusChanged);
	connect(clearButton, &QPushButton::clicked, chatView, &QTextBrowser::clear);
}

void UnifiedChatDock::showEvent(QShowEvent *event)
{
	OBSDock::showEvent(event);
	AutoConnectAccounts();
}

void UnifiedChatDock::hideEvent(QHideEvent *event)
{
	/* No point in holding IRC/WebSocket connections or spending YouTube quota
	 * while the dock is not on screen. */
	DisconnectAccounts();
	OBSDock::hideEvent(event);
}

void UnifiedChatDock::AutoConnectAccounts()
{
	if (connected)
		return;

	bool any = false;
	bool unsupportedOnly = true;
	for (const auto &channel : MultistreamChannelStore::Load()) {
		if (!GetStreamPlatformInfo(channel.platform).supportsChat)
			continue;
		unsupportedOnly = false;

		/* Twitch and Kick chats are addressed by channel name, YouTube by
		 * the connected account id. */
		const QString target = channel.platform == StreamPlatform::YouTube
					       ? QString::fromStdString(channel.accountId)
					       : QString::fromStdString(channel.displayName);
		if (target.isEmpty())
			continue;
		aggregator->ConnectPlatform(channel.platform, target);
		any = true;
	}

	connected = any;
	if (!any)
		statusLabel->setText(unsupportedOnly ? QTStr("Multistream.Chat.NoAccounts")
						     : QTStr("Multistream.Chat.NoChatChannels"));
}

void UnifiedChatDock::DisconnectAccounts()
{
	if (!connected)
		return;
	aggregator->DisconnectAll();
	connected = false;
}

void UnifiedChatDock::OnChatMessage(const ChatMessage &msg)
{
	if (msg.platform == StreamPlatform::Twitch && !showTwitch->isChecked())
		return;
	if (msg.platform == StreamPlatform::YouTube && !showYouTube->isChecked())
		return;
	if (msg.platform == StreamPlatform::Kick && !showKick->isChecked())
		return;

	const QString formattedHtml =
		QStringLiteral("<div style=\"margin-bottom: 5px;\">%1 <span style=\"font-size:10px;\">%2</span> "
			       "<b style=\"color:%3;\">%4:</b> <span>%5</span></div>")
			.arg(BadgeHtml(msg.platform), msg.timestamp.toHtmlEscaped(),
			     SafeUserColor(msg.userColor, msg.platform), msg.senderName.toHtmlEscaped(),
			     msg.messageText.toHtmlEscaped());

	chatView->append(formattedHtml);

	QTextDocument *document = chatView->document();
	while (document->blockCount() > MAX_CHAT_BLOCKS) {
		QTextCursor cursor(document->firstBlock());
		cursor.select(QTextCursor::BlockUnderCursor);
		cursor.removeSelectedText();
		cursor.deleteChar();
	}

	if (autoScroll->isChecked())
		chatView->verticalScrollBar()->setValue(chatView->verticalScrollBar()->maximum());
}

void UnifiedChatDock::OnStatusChanged(StreamPlatform platform, ChatConnectionState state, const QString &detail)
{
	statusLabel->setText(StatusText(state, PlatformName(platform), detail));
}
