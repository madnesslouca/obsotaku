/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "UnifiedChatDock.hpp"

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

/* Document resource name the platform logo is registered under, so a message
 * row can reference the artwork with a plain <img>. */
QString PlatformIconUrl(StreamPlatform platform)
{
	return QStringLiteral("platform:%1").arg(StreamPlatformId(platform));
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

QString RolesHtml(const ChatMessage &msg)
{
	QString html;
	for (const QString &badge : msg.roleBadges) {
		if (badge == QStringLiteral("HOST"))
			html += RoleBadgeHtml(badge, QStringLiteral("#7c3aed"), QStringLiteral("#fff"));
		else if (badge == QStringLiteral("MOD"))
			html += RoleBadgeHtml(badge, QStringLiteral("#16a34a"), QStringLiteral("#fff"));
		else if (badge == QStringLiteral("VIP"))
			html += RoleBadgeHtml(badge, QStringLiteral("#db2777"), QStringLiteral("#fff"));
		else if (badge == QStringLiteral("SUB"))
			html += RoleBadgeHtml(badge, QStringLiteral("#2563eb"), QStringLiteral("#fff"));
		else
			html += RoleBadgeHtml(badge, QStringLiteral("#ca8a04"), QStringLiteral("#111"));
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

QString MessageRowHtml(const ChatMessage &msg)
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
		.arg(rowStyle, PlatformBadgeHtml(msg.platform), RolesHtml(msg), msg.timestamp.toHtmlEscaped(),
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

	sendPlatform = new QComboBox(mainWidget);
	sendPlatform->setObjectName(QStringLiteral("unifiedChatSendPlatform"));
	sendPlatform->setMinimumWidth(110);

	sendInput = new QLineEdit(mainWidget);
	sendInput->setObjectName(QStringLiteral("unifiedChatSendInput"));
	sendInput->setPlaceholderText(QTStr("Multistream.Chat.SendPlaceholder"));

	sendButton = new QPushButton(QTStr("Multistream.Chat.Send"), mainWidget);
	sendButton->setObjectName(QStringLiteral("unifiedChatSendButton"));
	sendButton->setEnabled(false);

	sendRow->addWidget(sendPlatform);
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
	connect(sendPlatform, qOverload<int>(&QComboBox::currentIndexChanged), this,
		&UnifiedChatDock::OnSendPlatformChanged);
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
	platformFilters.clear();

	QSet<int> seen;
	for (const auto &channel : channels) {
		if (!GetStreamPlatformInfo(channel.platform).supportsChat)
			continue;
		const int key = static_cast<int>(channel.platform);
		if (seen.contains(key))
			continue;
		seen.insert(key);

		auto *box = new QCheckBox(PlatformName(channel.platform), this->widget());
		box->setChecked(true);
		box->setObjectName(QStringLiteral("chatFilter_%1")
					   .arg(QString::fromUtf8(GetStreamPlatformInfo(channel.platform).id.data(),
								  static_cast<qsizetype>(
									  GetStreamPlatformInfo(channel.platform).id.size()))));
		box->setStyleSheet(QStringLiteral("QCheckBox { color: %1; font-weight: 600; }")
					   .arg(PlatformColor(channel.platform)));
		connect(box, &QCheckBox::toggled, this, &UnifiedChatDock::OnFilterToggled);
		filtersLayout->addWidget(box);
		platformFilters.insert(key, box);
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
	RefreshSendTargets();

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
	OnSendPlatformChanged(sendPlatform->currentIndex());
}

void UnifiedChatDock::DisconnectAccounts()
{
	if (!connected && platformStates.isEmpty()) {
		aggregator->DisconnectAll();
		return;
	}
	aggregator->DisconnectAll();
	connected = false;
	platformStates.clear();
	platformDetails.clear();
}

void UnifiedChatDock::RefreshSendTargets()
{
	const int previous = sendPlatform->currentData().toInt();
	sendPlatform->blockSignals(true);
	sendPlatform->clear();

	for (auto it = platformFilters.constBegin(); it != platformFilters.constEnd(); ++it) {
		const auto platform = static_cast<StreamPlatform>(it.key());
		if (!GetStreamPlatformInfo(platform).supportsChat)
			continue;
		/* Kick has no public send path in this build. */
		if (platform == StreamPlatform::Kick)
			continue;
		sendPlatform->addItem(PlatformName(platform), it.key());
	}

	const int restore = sendPlatform->findData(previous);
	sendPlatform->setCurrentIndex(restore >= 0 ? restore : 0);
	sendPlatform->blockSignals(false);
	sendPlatform->setEnabled(sendPlatform->count() > 0);
}

void UnifiedChatDock::OnSendPlatformChanged(int)
{
	if (sendPlatform->count() == 0) {
		sendButton->setEnabled(false);
		sendInput->setEnabled(false);
		sendHint->setText(QTStr("Multistream.Chat.SendUnavailable"));
		return;
	}

	const auto platform = static_cast<StreamPlatform>(sendPlatform->currentData().toInt());
	const bool can = aggregator->CanSend(platform);
	sendButton->setEnabled(can);
	sendInput->setEnabled(true);

	if (platform == StreamPlatform::Twitch && !can)
		sendHint->setText(QTStr("Multistream.Chat.SendNeedsTwitchAuth"));
	else if (platform == StreamPlatform::YouTube && !can)
		sendHint->setText(QTStr("Multistream.Chat.SendNeedsYouTubeLive"));
	else if (can)
		sendHint->setText(QTStr("Multistream.Chat.SendReady").arg(PlatformName(platform)));
	else
		sendHint->setText(QTStr("Multistream.Chat.SendUnavailable"));
}

void UnifiedChatDock::OnSendClicked()
{
	if (sendPlatform->count() == 0)
		return;

	const auto platform = static_cast<StreamPlatform>(sendPlatform->currentData().toInt());
	const QString text = sendInput->text();
	QString error;
	if (!aggregator->SendText(platform, text, error)) {
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
	OnSendPlatformChanged(sendPlatform->currentIndex());
}

void UnifiedChatDock::OnFilterToggled()
{
	/* Filters only hide future rows; already rendered messages stay. */
}

bool UnifiedChatDock::PlatformFilterEnabled(StreamPlatform platform) const
{
	const auto *box = platformFilters.value(static_cast<int>(platform), nullptr);
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
	if (!PlatformFilterEnabled(msg.platform))
		return;
	AppendHtml(MessageRowHtml(msg));
}

void UnifiedChatDock::OnStatusChanged(const QString &, StreamPlatform platform, ChatConnectionState state,
				      const QString &detail)
{
	platformStates.insert(static_cast<int>(platform), state);
	platformDetails.insert(static_cast<int>(platform), detail);
	UpdateStatusSummary();
	OnSendPlatformChanged(sendPlatform->currentIndex());
}

void UnifiedChatDock::UpdateStatusSummary()
{
	if (platformStates.isEmpty()) {
		statusLabel->setText(QTStr("Multistream.Chat.Ready"));
		return;
	}

	QStringList parts;
	for (auto it = platformStates.constBegin(); it != platformStates.constEnd(); ++it) {
		const auto platform = static_cast<StreamPlatform>(it.key());
		parts << StatusText(it.value(), PlatformName(platform), platformDetails.value(it.key()));
	}
	statusLabel->setText(parts.join(QStringLiteral(" · ")));
}
