/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "UnifiedChatDock.hpp"

#include <utility/ChatBadgeIcons.hpp>
#include <utility/MultistreamChannelStore.hpp>
#include <utility/PlatformIconProvider.hpp>
#include <utility/StreamPlatformDisplay.hpp>

#include <OBSApp.hpp>
#include <qt-wrappers.hpp>

#include <QColor>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSet>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

#include "moc_UnifiedChatDock.cpp"

namespace {
constexpr int MAX_CHAT_BLOCKS = 500;
constexpr int PLATFORM_ICON_SIZE = 14;

constexpr int ROLE_BADGE_SIZE = 14;

/* Document resource name the platform logo is registered under, so a message
 * row can reference the artwork with a plain <img>. */
QString PlatformIconUrl(StreamPlatform platform)
{
	return QStringLiteral("platform:%1").arg(StreamPlatformId(platform));
}

QString RoleBadgeUrl(const QString &roleLabel)
{
	return QStringLiteral("badge:%1").arg(roleLabel.toLower());
}

/* Every role the aggregator can report, in the order they are drawn. */
const QStringList &RoleBadgeLabels()
{
	static const QStringList labels{QStringLiteral("HOST"), QStringLiteral("MOD"), QStringLiteral("VIP"),
					QStringLiteral("SUB")};
	return labels;
}

QString PlatformName(StreamPlatform platform)
{
	return StreamPlatformDisplayName(platform);
}

QString PlatformColor(StreamPlatform platform)
{
	const auto color = GetStreamPlatformInfo(platform).brandColor;
	return QString::fromUtf8(color.data(), static_cast<qsizetype>(color.size()));
}

QString SafeUserColor(const QString &color, StreamPlatform platform)
{
	static const QRegularExpression hexColor(QStringLiteral("^#[0-9a-fA-F]{6}$"));
	if (hexColor.match(color).hasMatch())
		return color;
	return PlatformColor(platform);
}

/* The logo alone: the platform is recognisable at a glance and the row keeps
 * its width for the message. The name stays in the title attribute. */
QString PlatformBadgeHtml(StreamPlatform platform)
{
	return QStringLiteral("<img src=\"%1\" width=\"%2\" height=\"%2\" title=\"%3\" "
			      "style=\"vertical-align:middle;\">")
		.arg(PlatformIconUrl(platform))
		.arg(PLATFORM_ICON_SIZE)
		.arg(PlatformName(platform).toHtmlEscaped());
}

QString RoleBadgeHtml(const QString &label, const QString &bg, const QString &fg)
{
	return QStringLiteral("<span style=\"background-color:%1; color:%2; border-radius:3px; "
			      "padding:0 4px; font-size:9px; font-weight:700;\">%3</span>&nbsp;")
		.arg(bg, fg, label.toHtmlEscaped());
}

/* Badges the artwork covers become an icon; anything else keeps the lettered
 * pill, so an unknown role is still shown rather than silently dropped. */
QString RolesHtml(const ChatMessage &msg, const QSet<QString> &drawnRoles)
{
	QString html;
	for (const QString &badge : msg.roleBadges) {
		if (drawnRoles.contains(badge)) {
			html += QStringLiteral("<img src=\"%1\" width=\"%2\" height=\"%2\" title=\"%3\" "
					       "style=\"vertical-align:middle;\">&nbsp;")
					.arg(RoleBadgeUrl(badge))
					.arg(ROLE_BADGE_SIZE)
					.arg(badge.toHtmlEscaped());
		} else if (badge == QStringLiteral("HOST")) {
			html += RoleBadgeHtml(badge, QStringLiteral("#7c3aed"), QStringLiteral("#fff"));
		} else if (badge == QStringLiteral("MOD")) {
			html += RoleBadgeHtml(badge, QStringLiteral("#16a34a"), QStringLiteral("#fff"));
		} else if (badge == QStringLiteral("VIP")) {
			html += RoleBadgeHtml(badge, QStringLiteral("#db2777"), QStringLiteral("#fff"));
		} else if (badge == QStringLiteral("SUB")) {
			html += RoleBadgeHtml(badge, QStringLiteral("#2563eb"), QStringLiteral("#fff"));
		} else {
			html += RoleBadgeHtml(badge, QStringLiteral("#ca8a04"), QStringLiteral("#111"));
		}
	}
	return html;
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
		if (detail == QStringLiteral("auth"))
			return QTStr("Multistream.Chat.AuthFailed").arg(platformName);
		return detail.isEmpty() ? QTStr("Multistream.Chat.Failed").arg(platformName)
					: QTStr("Multistream.Chat.FailedDetail").arg(platformName, detail);
	}
	return {};
}

QString MessageRowHtml(const ChatMessage &msg, const QSet<QString> &drawnRoles)
{
	const QString nickColor = SafeUserColor(msg.userColor, msg.platform);
	QString rowStyle = QStringLiteral("margin:0 0 8px 0; padding:6px 8px; border-radius:6px;");
	if (msg.kind == ChatMessageKind::SuperChat)
		rowStyle += QStringLiteral(" background-color:rgba(234,179,8,0.14); border-left:3px solid #eab308;");
	else if (msg.kind == ChatMessageKind::Membership)
		rowStyle += QStringLiteral(" background-color:rgba(34,197,94,0.12); border-left:3px solid #22c55e;");
	else if (msg.isBroadcaster)
		rowStyle += QStringLiteral(" background-color:rgba(124,58,237,0.10);");

	QString paid;
	if (msg.kind == ChatMessageKind::SuperChat && !msg.paidAmount.isEmpty()) {
		paid = QStringLiteral(" <span style=\"color:#eab308; font-weight:700;\">%1%2</span>")
			       .arg(msg.paidAmount.toHtmlEscaped(),
				    msg.paidCurrency.isEmpty()
					    ? QString()
					    : QStringLiteral(" %1").arg(msg.paidCurrency.toHtmlEscaped()));
	}

	const QString channelBit = msg.channelName.isEmpty()
					   ? QString()
					   : QStringLiteral(" <span style=\"color:#888; font-size:10px;\">@%1</span>")
						     .arg(msg.channelName.toHtmlEscaped());

	/* Qt's rich text drops margins on inline spans, so the gap between the
	 * nickname and the message has to be real characters or the two run
	 * together. The colon is what every chat client uses for the same job. */
	return QStringLiteral("<div style=\"%1\">"
			      "<div style=\"margin-bottom:2px;\">%2 %3"
			      "<span style=\"font-size:10px; color:#888;\">&nbsp;%4</span>%5%6</div>"
			      "<div><b style=\"color:%7;\">%8:</b>&nbsp;%9</div></div>")
		.arg(rowStyle, PlatformBadgeHtml(msg.platform), RolesHtml(msg, drawnRoles),
		     msg.timestamp.toHtmlEscaped(),
		     channelBit, paid, nickColor, msg.senderName.toHtmlEscaped(), msg.messageText.toHtmlEscaped());
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

	filtersLayout = new QHBoxLayout();
	filtersLayout->setContentsMargins(0, 0, 0, 0);
	filtersLayout->setSpacing(8);
	toolbar->addLayout(filtersLayout);
	toolbar->addStretch();

	autoScroll = new QCheckBox(QTStr("Multistream.Chat.AutoScroll"), mainWidget);
	autoScroll->setChecked(true);
	clearButton = new QPushButton(QTStr("Multistream.Chat.Clear"), mainWidget);
	toolbar->addWidget(autoScroll);
	toolbar->addWidget(clearButton);
	layout->addLayout(toolbar);

	chatView = new QTextBrowser(mainWidget);
	chatView->setObjectName(QStringLiteral("unifiedChatView"));
	chatView->setOpenExternalLinks(false);
	chatView->setOpenLinks(false);
	chatView->setReadOnly(true);
	RegisterPlatformIcons();
	layout->addWidget(chatView, 1);

	auto *sendRow = new QHBoxLayout();
	sendRow->setContentsMargins(0, 0, 0, 0);
	sendRow->setSpacing(6);

	sendChannel = new QComboBox(mainWidget);
	sendChannel->setObjectName(QStringLiteral("unifiedChatSendPlatform"));
	sendChannel->setMinimumWidth(140);

	sendInput = new QLineEdit(mainWidget);
	sendInput->setObjectName(QStringLiteral("unifiedChatSendInput"));
	sendInput->setPlaceholderText(QTStr("Multistream.Chat.SendPlaceholder"));

	sendButton = new QPushButton(QTStr("Multistream.Chat.Send"), mainWidget);
	sendButton->setObjectName(QStringLiteral("unifiedChatSendButton"));
	sendButton->setEnabled(false);

	sendRow->addWidget(sendChannel);
	sendRow->addWidget(sendInput, 1);
	sendRow->addWidget(sendButton);
	layout->addLayout(sendRow);

	sendHint = new QLabel(mainWidget);
	sendHint->setObjectName(QStringLiteral("unifiedChatSendHint"));
	sendHint->setWordWrap(true);
	layout->addWidget(sendHint);

	statusLabel = new QLabel(QTStr("Multistream.Chat.Ready"), mainWidget);
	statusLabel->setObjectName(QStringLiteral("unifiedChatStatus"));
	statusLabel->setWordWrap(true);
	layout->addWidget(statusLabel);

	setWidget(mainWidget);

	connect(aggregator, &MultiStreamChatAggregator::messageReceived, this, &UnifiedChatDock::OnChatMessage);
	connect(aggregator, &MultiStreamChatAggregator::statusChanged, this, &UnifiedChatDock::OnStatusChanged);
	connect(clearButton, &QPushButton::clicked, chatView, &QTextBrowser::clear);
	connect(sendButton, &QPushButton::clicked, this, &UnifiedChatDock::OnSendClicked);
	connect(sendInput, &QLineEdit::returnPressed, this, &UnifiedChatDock::OnSendClicked);
	connect(sendChannel, qOverload<int>(&QComboBox::currentIndexChanged), this,
		&UnifiedChatDock::OnSendChannelChanged);
}

void UnifiedChatDock::showEvent(QShowEvent *event)
{
	OBSDock::showEvent(event);
	AutoConnectAccounts();
}

void UnifiedChatDock::hideEvent(QHideEvent *event)
{
	DisconnectAccounts();
	OBSDock::hideEvent(event);
}

void UnifiedChatDock::RebuildFilters(const std::vector<ChatChannelRef> &channels)
{
	while (QLayoutItem *item = filtersLayout->takeAt(0)) {
		if (QWidget *widget = item->widget())
			widget->deleteLater();
		delete item;
	}
	channelFilters.clear();
	channelNames.clear();
	channelPlatforms.clear();

	/* One filter per channel rather than per platform: with two accounts on
	 * the same platform, a single checkbox would hide both at once and the
	 * second account would have no label of its own anywhere. */
	for (const auto &channel : channels) {
		if (!GetStreamPlatformInfo(channel.platform).supportsChat)
			continue;

		const QString label = channel.displayName.isEmpty() ? channel.address : channel.displayName;
		channelNames.insert(channel.channelId, label);
		channelPlatforms.insert(channel.channelId, channel.platform);

		auto *box = new QCheckBox(label, this->widget());
		box->setChecked(true);
		box->setToolTip(PlatformName(channel.platform));
		box->setStyleSheet(QStringLiteral("QCheckBox { color: %1; font-weight: 600; }")
					   .arg(PlatformColor(channel.platform)));
		connect(box, &QCheckBox::toggled, this, &UnifiedChatDock::OnFilterToggled);
		filtersLayout->addWidget(box);
		channelFilters.insert(channel.channelId, box);
	}
}

void UnifiedChatDock::AutoConnectAccounts()
{
	std::vector<ChatChannelRef> targets;
	bool unsupportedOnly = true;

	for (const auto &channel : MultistreamChannelStore::Load()) {
		const auto &info = GetStreamPlatformInfo(channel.platform);
		if (!info.supportsChat)
			continue;
		unsupportedOnly = false;

		ChatChannelRef ref;
		ref.channelId = QString::fromStdString(channel.id);
		ref.platform = channel.platform;
		ref.displayName = QString::fromStdString(channel.displayName);
		ref.accountId = QString::fromStdString(channel.accountId);
		/* Twitch/Kick chats are addressed by the platform handle; YouTube by
		 * account id. The handle is stored separately so renaming a channel
		 * in the interface cannot point chat at a channel that does not exist. */
		if (channel.platform == StreamPlatform::YouTube) {
			ref.address = ref.accountId;
		} else {
			ref.address = QString::fromStdString(channel.chatAddress);
			if (ref.address.isEmpty())
				ref.address = ref.displayName;
		}
		if (ref.address.isEmpty())
			continue;
		targets.push_back(std::move(ref));
	}

	RebuildFilters(targets);
	RefreshSendTargets(targets);

	if (targets.empty()) {
		connected = false;
		statusLabel->setText(unsupportedOnly ? QTStr("Multistream.Chat.NoAccounts")
						     : QTStr("Multistream.Chat.NoChatChannels"));
		aggregator->DisconnectAll();
		return;
	}

	aggregator->SetChannels(targets);
	connected = true;
	UpdateStatusSummary();
	OnSendChannelChanged(sendChannel->currentIndex());
}

void UnifiedChatDock::DisconnectAccounts()
{
	if (!connected && channelStates.isEmpty()) {
		aggregator->DisconnectAll();
		return;
	}
	aggregator->DisconnectAll();
	connected = false;
	channelStates.clear();
	channelDetails.clear();
}

void UnifiedChatDock::RefreshSendTargets(const std::vector<ChatChannelRef> &channels)
{
	const QString previous = sendChannel->currentData().toString();
	sendChannel->blockSignals(true);
	sendChannel->clear();

	for (const auto &channel : channels) {
		if (!MultiStreamChatAggregator::PlatformCanSend(channel.platform))
			continue;
		/* The platform is in the label because two channels can share a
		 * name across platforms, and the target has to be unambiguous. */
		const QString label = channel.displayName.isEmpty() ? channel.address : channel.displayName;
		sendChannel->addItem(QStringLiteral("%1 (%2)").arg(label, PlatformName(channel.platform)),
				     channel.channelId);
	}

	const int restore = sendChannel->findData(previous);
	sendChannel->setCurrentIndex(restore >= 0 ? restore : 0);
	sendChannel->blockSignals(false);
	sendChannel->setEnabled(sendChannel->count() > 0);
}

void UnifiedChatDock::OnSendChannelChanged(int)
{
	if (sendChannel->count() == 0) {
		sendButton->setEnabled(false);
		sendInput->setEnabled(false);
		sendHint->setText(QTStr("Multistream.Chat.SendUnavailable"));
		return;
	}

	const QString channelId = sendChannel->currentData().toString();
	const bool can = aggregator->CanSend(channelId);
	sendButton->setEnabled(can);
	sendInput->setEnabled(true);

	const QString name = channelNames.value(channelId, sendChannel->currentText());
	if (can) {
		sendHint->setText(QTStr("Multistream.Chat.SendReady").arg(name));
		return;
	}

	/* A destination that cannot take a message says why: the two reasons ask
	 * different things of the user — reconnect the account, or go live. */
	switch (channelPlatforms.value(channelId, StreamPlatform::CustomRtmp)) {
	case StreamPlatform::Twitch:
		sendHint->setText(QTStr("Multistream.Chat.SendNeedsTwitchAuth"));
		break;
	case StreamPlatform::YouTube:
		sendHint->setText(QTStr("Multistream.Chat.SendNeedsYouTubeLive"));
		break;
	default:
		sendHint->setText(QTStr("Multistream.Chat.SendUnavailable"));
		break;
	}
}

void UnifiedChatDock::OnSendClicked()
{
	if (sendChannel->count() == 0)
		return;

	const QString channelId = sendChannel->currentData().toString();
	const QString text = sendInput->text();
	QString error;
	if (!aggregator->SendText(channelId, text, error)) {
		if (error == QStringLiteral("twitch-anonymous"))
			sendHint->setText(QTStr("Multistream.Chat.SendNeedsTwitchAuth"));
		else if (error == QStringLiteral("youtube-not-live"))
			sendHint->setText(QTStr("Multistream.Chat.SendNeedsYouTubeLive"));
		else if (error == QStringLiteral("empty"))
			return;
		else
			sendHint->setText(QTStr("Multistream.Chat.SendFailed"));
		return;
	}

	sendInput->clear();
	OnSendChannelChanged(sendChannel->currentIndex());
}

void UnifiedChatDock::OnFilterToggled()
{
	/* Filters only hide future rows; already rendered messages stay. */
}

bool UnifiedChatDock::ChannelFilterEnabled(const QString &channelId) const
{
	const auto *box = channelFilters.value(channelId, nullptr);
	return !box || box->isChecked();
}

void UnifiedChatDock::RegisterPlatformIcons()
{
	/* Rich text cannot load files, so the logos are handed to the document as
	 * named resources the message rows point an <img> at. */
	const qreal ratio = devicePixelRatioF() > 0 ? devicePixelRatioF() : 1.0;
	for (const auto platform : SelectableStreamPlatforms()) {
		QPixmap glyph = PlatformIconProvider::Glyph(platform, PLATFORM_ICON_SIZE, ratio);
		if (glyph.isNull())
			continue;
		chatView->document()->addResource(QTextDocument::ImageResource, QUrl(PlatformIconUrl(platform)),
						  QVariant(glyph));
	}

	/* A role with no artwork is left out of the set and keeps the text pill. */
	drawnRoleBadges.clear();
	for (const QString &label : RoleBadgeLabels()) {
		QPixmap glyph = ChatBadgeIcons::Glyph(label, ROLE_BADGE_SIZE, ratio);
		if (glyph.isNull())
			continue;
		chatView->document()->addResource(QTextDocument::ImageResource, QUrl(RoleBadgeUrl(label)),
						  QVariant(glyph));
		drawnRoleBadges.insert(label);
	}
}

void UnifiedChatDock::AppendHtml(const QString &html)
{
	chatView->append(html);

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

void UnifiedChatDock::OnChatMessage(const ChatMessage &msg)
{
	if (!ChannelFilterEnabled(msg.channelId))
		return;
	AppendHtml(MessageRowHtml(msg, drawnRoleBadges));
}

void UnifiedChatDock::OnStatusChanged(const QString &channelId, StreamPlatform platform, ChatConnectionState state,
				      const QString &detail)
{
	channelStates.insert(channelId, state);
	channelDetails.insert(channelId, detail);
	if (!channelNames.contains(channelId))
		channelNames.insert(channelId, PlatformName(platform));
	UpdateStatusSummary();
	OnSendChannelChanged(sendChannel->currentIndex());
}

void UnifiedChatDock::UpdateStatusSummary()
{
	if (channelStates.isEmpty()) {
		statusLabel->setText(QTStr("Multistream.Chat.Ready"));
		return;
	}

	/* Named by channel rather than by platform: with two accounts on one
	 * platform, "Conectado ao chat da Twitch" twice says nothing. */
	QStringList parts;
	for (auto it = channelStates.constBegin(); it != channelStates.constEnd(); ++it)
		parts << StatusText(it.value(), channelNames.value(it.key(), it.key()), channelDetails.value(it.key()));
	statusLabel->setText(parts.join(QStringLiteral(" · ")));
}
