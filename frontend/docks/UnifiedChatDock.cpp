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
#include <QEvent>
#include <QFrame>
#include <QIcon>
#include <QPalette>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QStyle>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

#include <utility>

#include "moc_UnifiedChatDock.cpp"

namespace {
constexpr int MAX_CHAT_BLOCKS = 500;
constexpr int MAX_CHAT_MESSAGES = 300;
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

QColor MixColors(const QColor &base, const QColor &accent, qreal accentAmount)
{
	const qreal baseAmount = 1.0 - accentAmount;
	return QColor(qRound(base.red() * baseAmount + accent.red() * accentAmount),
		      qRound(base.green() * baseAmount + accent.green() * accentAmount),
		      qRound(base.blue() * baseAmount + accent.blue() * accentAmount));
}

struct ChatColors {
	QString surface;
	QString border;
	QString text;
	QString muted;
	QColor background;
};

ChatColors ColorsFor(const QTextBrowser *view)
{
	const QPalette palette = view->palette();
	const QColor background = palette.color(QPalette::Base);
	const QColor text = palette.color(QPalette::Text);
	QColor muted = palette.color(QPalette::Disabled, QPalette::Text);
	if (!muted.isValid() || muted == text)
		muted = MixColors(background, text, 0.62);

	return {MixColors(background, text, 0.045).name(), MixColors(background, text, 0.15).name(),
		text.name(), muted.name(), background};
}

QString ReadableUserColor(const QString &color, StreamPlatform platform, const QColor &background)
{
	QColor result(SafeUserColor(color, platform));
	if (background.lightness() < 128 && result.lightness() < 145)
		result = result.lighter(175);
	else if (background.lightness() >= 128 && result.lightness() > 145)
		result = result.darker(170);
	return result.name();
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

QString MessageRowHtml(const ChatMessage &msg, const QSet<QString> &drawnRoles, const ChatColors &colors)
{
	const QString platformColor = PlatformColor(msg.platform);
	const QString nickColor = ReadableUserColor(msg.userColor, msg.platform, colors.background);
	QString accent = platformColor;
	QColor surface(colors.surface);
	if (msg.kind == ChatMessageKind::SuperChat) {
		accent = QStringLiteral("#eab308");
		surface = MixColors(colors.background, QColor(accent), 0.14);
	} else if (msg.kind == ChatMessageKind::Membership) {
		accent = QStringLiteral("#22c55e");
		surface = MixColors(colors.background, QColor(accent), 0.12);
	} else if (msg.isBroadcaster) {
		surface = MixColors(colors.background, QColor(QStringLiteral("#7c3aed")), 0.11);
	}
	const QString rowStyle = QStringLiteral("margin:0 0 7px 0; padding:8px 10px; background-color:%1; "
						"border:1px solid %2; border-left:3px solid %3; border-radius:7px;")
				 .arg(surface.name(), colors.border, accent);

	QString paid;
	if (msg.kind == ChatMessageKind::SuperChat && !msg.paidAmount.isEmpty()) {
		paid = QStringLiteral(" <span style=\"color:#eab308; font-weight:700;\">%1%2</span>")
			       .arg(msg.paidAmount.toHtmlEscaped(),
				    msg.paidCurrency.isEmpty()
					    ? QString()
					    : QStringLiteral(" %1").arg(msg.paidCurrency.toHtmlEscaped()));
	}

	const QString channelName = msg.channelName.isEmpty() ? PlatformName(msg.platform) : msg.channelName;
	const QString channelBit = QStringLiteral(
		"<span style=\"color:%1; font-size:10px; font-weight:600;\">&nbsp;%2</span>")
					   .arg(platformColor, channelName.toHtmlEscaped());

	/* Qt's rich text drops margins on inline spans, so the gap between the
	 * nickname and the message has to be real characters or the two run
	 * together. The colon is what every chat client uses for the same job. */
	return QStringLiteral("<div style=\"%1\">"
			      "<div style=\"margin-bottom:4px; color:%2;\">%3%4"
			      "<span style=\"font-size:10px;\">&nbsp;&nbsp;%5</span>%6</div>"
			      "<div style=\"color:%7;\">%8<b style=\"color:%9;\">%10</b>"
			      "<span style=\"color:%7;\">:&nbsp;%11</span></div></div>")
		.arg(rowStyle, colors.muted, PlatformBadgeHtml(msg.platform), channelBit,
		     msg.timestamp.toHtmlEscaped(), paid, colors.text, RolesHtml(msg, drawnRoles), nickColor,
		     msg.senderName.toHtmlEscaped(), msg.messageText.toHtmlEscaped());
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
	layout->setSpacing(8);

	auto *header = new QFrame(mainWidget);
	header->setObjectName(QStringLiteral("unifiedChatHeader"));
	auto *headerLayout = new QVBoxLayout(header);
	headerLayout->setContentsMargins(12, 10, 10, 10);
	headerLayout->setSpacing(6);

	auto *titleRow = new QHBoxLayout();
	titleRow->setContentsMargins(0, 0, 0, 0);
	titleRow->setSpacing(8);
	auto *headerTitle = new QLabel(QTStr("Multistream.Chat.HeaderTitle"), header);
	headerTitle->setObjectName(QStringLiteral("unifiedChatHeaderTitle"));
	clearButton = new QPushButton(QTStr("Multistream.Chat.Clear"), header);
	clearButton->setObjectName(QStringLiteral("unifiedChatClearButton"));
	clearButton->setToolTip(QTStr("Multistream.Chat.ClearTip"));
	clearButton->setEnabled(false);
	titleRow->addWidget(headerTitle);
	titleRow->addStretch();
	titleRow->addWidget(clearButton);
	headerLayout->addLayout(titleRow);

	auto *statusRow = new QHBoxLayout();
	statusRow->setContentsMargins(0, 0, 0, 0);
	statusRow->setSpacing(7);
	connectionDot = new QFrame(header);
	connectionDot->setObjectName(QStringLiteral("unifiedChatConnectionDot"));
	connectionDot->setFixedSize(8, 8);
	statusLabel = new QLabel(QTStr("Multistream.Chat.Ready"), header);
	statusLabel->setObjectName(QStringLiteral("unifiedChatStatus"));
	statusLabel->setWordWrap(true);
	statusRow->addWidget(connectionDot, 0, Qt::AlignTop);
	statusRow->addWidget(statusLabel, 1);

	autoScroll = new QCheckBox(QTStr("Multistream.Chat.AutoScroll"), header);
	autoScroll->setChecked(true);
	autoScroll->setToolTip(QTStr("Multistream.Chat.AutoScrollTip"));
	statusRow->addWidget(autoScroll, 0, Qt::AlignTop);
	headerLayout->addLayout(statusRow);
	layout->addWidget(header);

	filtersCard = new QFrame(mainWidget);
	filtersCard->setObjectName(QStringLiteral("unifiedChatFilters"));
	auto *filtersCardLayout = new QVBoxLayout(filtersCard);
	filtersCardLayout->setContentsMargins(10, 7, 10, 7);
	filtersCardLayout->setSpacing(5);
	auto *filtersTitle = new QLabel(QTStr("Multistream.Chat.VisibleChannels"), filtersCard);
	filtersTitle->setObjectName(QStringLiteral("unifiedChatSectionLabel"));
	filtersCardLayout->addWidget(filtersTitle);
	auto *filtersScroll = new QScrollArea(filtersCard);
	filtersScroll->setObjectName(QStringLiteral("unifiedChatFiltersScroll"));
	filtersScroll->setWidgetResizable(true);
	filtersScroll->setFrameShape(QFrame::NoFrame);
	filtersScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	filtersScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	/* Leave room for the horizontal scrollbar when many accounts are shown. */
	filtersScroll->setFixedHeight(44);
	auto *filtersContent = new QWidget(filtersScroll);
	filtersContent->setObjectName(QStringLiteral("unifiedChatFiltersContent"));
	filtersLayout = new QHBoxLayout(filtersContent);
	filtersLayout->setContentsMargins(0, 0, 0, 0);
	filtersLayout->setSpacing(6);
	filtersLayout->addStretch();
	filtersScroll->setWidget(filtersContent);
	filtersCardLayout->addWidget(filtersScroll);
	layout->addWidget(filtersCard);

	chatView = new QTextBrowser(mainWidget);
	chatView->setObjectName(QStringLiteral("unifiedChatView"));
	chatView->setOpenExternalLinks(false);
	chatView->setOpenLinks(false);
	chatView->setReadOnly(true);
	RegisterPlatformIcons();

	chatStack = new QStackedWidget(mainWidget);
	chatStack->setObjectName(QStringLiteral("unifiedChatStack"));
	auto *emptyPage = new QWidget(chatStack);
	emptyPage->setObjectName(QStringLiteral("unifiedChatEmpty"));
	auto *emptyLayout = new QVBoxLayout(emptyPage);
	emptyLayout->setContentsMargins(28, 28, 28, 28);
	emptyLayout->setSpacing(7);
	emptyLayout->addStretch();
	auto *emptyIcon = new QLabel(QStringLiteral("•••"), emptyPage);
	emptyIcon->setObjectName(QStringLiteral("unifiedChatEmptyIcon"));
	emptyIcon->setAlignment(Qt::AlignCenter);
	emptyTitle = new QLabel(QTStr("Multistream.Chat.EmptyTitle"), emptyPage);
	emptyTitle->setObjectName(QStringLiteral("unifiedChatEmptyTitle"));
	emptyTitle->setAlignment(Qt::AlignCenter);
	emptyBody = new QLabel(QTStr("Multistream.Chat.EmptyBody"), emptyPage);
	emptyBody->setObjectName(QStringLiteral("unifiedChatEmptyBody"));
	emptyBody->setAlignment(Qt::AlignCenter);
	emptyBody->setWordWrap(true);
	emptyLayout->addWidget(emptyIcon);
	emptyLayout->addWidget(emptyTitle);
	emptyLayout->addWidget(emptyBody);
	emptyLayout->addStretch();
	chatStack->addWidget(emptyPage);
	chatStack->addWidget(chatView);
	layout->addWidget(chatStack, 1);

	auto *composer = new QFrame(mainWidget);
	composer->setObjectName(QStringLiteral("unifiedChatComposer"));
	auto *composerLayout = new QVBoxLayout(composer);
	composerLayout->setContentsMargins(10, 8, 10, 8);
	composerLayout->setSpacing(5);
	auto *composerTitle = new QLabel(QTStr("Multistream.Chat.ReplyOn"), composer);
	composerTitle->setObjectName(QStringLiteral("unifiedChatSectionLabel"));
	composerLayout->addWidget(composerTitle);
	auto *sendRow = new QHBoxLayout();
	sendRow->setContentsMargins(0, 0, 0, 0);
	sendRow->setSpacing(6);

	sendChannel = new QComboBox(composer);
	sendChannel->setObjectName(QStringLiteral("unifiedChatSendPlatform"));
	sendChannel->setMinimumWidth(140);

	sendInput = new QLineEdit(composer);
	sendInput->setObjectName(QStringLiteral("unifiedChatSendInput"));
	sendInput->setPlaceholderText(QTStr("Multistream.Chat.SendPlaceholder"));

	sendButton = new QPushButton(QTStr("Multistream.Chat.Send"), composer);
	sendButton->setObjectName(QStringLiteral("unifiedChatSendButton"));
	sendButton->setEnabled(false);

	sendRow->addWidget(sendChannel);
	sendRow->addWidget(sendInput, 1);
	sendRow->addWidget(sendButton);
	composerLayout->addLayout(sendRow);

	sendHint = new QLabel(composer);
	sendHint->setObjectName(QStringLiteral("unifiedChatSendHint"));
	sendHint->setWordWrap(true);
	composerLayout->addWidget(sendHint);
	layout->addWidget(composer);

	setWidget(mainWidget);

	connect(aggregator, &MultiStreamChatAggregator::messageReceived, this, &UnifiedChatDock::OnChatMessage);
	connect(aggregator, &MultiStreamChatAggregator::statusChanged, this, &UnifiedChatDock::OnStatusChanged);
	connect(clearButton, &QPushButton::clicked, this, &UnifiedChatDock::ClearMessages);
	connect(sendButton, &QPushButton::clicked, this, &UnifiedChatDock::OnSendClicked);
	connect(sendInput, &QLineEdit::returnPressed, this, &UnifiedChatDock::OnSendClicked);
	connect(sendInput, &QLineEdit::textChanged, this, &UnifiedChatDock::UpdateSendButton);
	connect(sendChannel, qOverload<int>(&QComboBox::currentIndexChanged), this,
		&UnifiedChatDock::OnSendChannelChanged);
}

void UnifiedChatDock::changeEvent(QEvent *event)
{
	OBSDock::changeEvent(event);
	if (chatView && chatStack &&
	    (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange)) {
		RegisterPlatformIcons();
		RenderMessages();
	}
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

		auto *box = new QToolButton(this->widget());
		box->setObjectName(QStringLiteral("unifiedChatFilter"));
		box->setText(label);
		box->setIcon(QIcon(PlatformIconProvider::Badge(channel.platform, 16)));
		box->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
		box->setCheckable(true);
		box->setChecked(true);
		box->setProperty("platform", StreamPlatformId(channel.platform));
		box->setToolTip(QTStr("Multistream.Chat.FilterTip").arg(label, PlatformName(channel.platform)));
		connect(box, &QToolButton::toggled, this, &UnifiedChatDock::OnFilterToggled);
		filtersLayout->addWidget(box);
		channelFilters.insert(channel.channelId, box);
	}
	filtersLayout->addStretch();
	filtersCard->setVisible(!channelFilters.isEmpty());
	RenderMessages();
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
		statusLabel->setToolTip(statusLabel->text());
		SetConnectionAppearance("offline");
		aggregator->DisconnectAll();
		UpdateEmptyState();
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
		sendChannel->addItem(QIcon(PlatformIconProvider::Badge(channel.platform, 16)),
				     QStringLiteral("%1 (%2)").arg(label, PlatformName(channel.platform)),
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
		sendTargetReady = false;
		UpdateSendButton();
		sendInput->setEnabled(false);
		sendHint->setText(QTStr("Multistream.Chat.SendUnavailable"));
		return;
	}

	const QString channelId = sendChannel->currentData().toString();
	const bool can = aggregator->CanSend(channelId);
	sendTargetReady = can;
	UpdateSendButton();
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
	if (sendChannel->count() == 0 || !sendTargetReady || sendInput->text().trimmed().isEmpty())
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
	RenderMessages();
}

void UnifiedChatDock::ClearMessages()
{
	messageHistory.clear();
	clearButton->setEnabled(false);
	chatView->clear();
	RegisterPlatformIcons();
	UpdateEmptyState();
}

void UnifiedChatDock::UpdateSendButton()
{
	sendButton->setEnabled(sendTargetReady && !sendInput->text().trimmed().isEmpty());
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

	chatStack->setCurrentWidget(chatView);
	if (autoScroll->isChecked())
		chatView->verticalScrollBar()->setValue(chatView->verticalScrollBar()->maximum());
}

void UnifiedChatDock::OnChatMessage(const ChatMessage &msg)
{
	messageHistory.append(msg);
	clearButton->setEnabled(true);
	while (messageHistory.size() > MAX_CHAT_MESSAGES)
		messageHistory.removeFirst();

	if (!ChannelFilterEnabled(msg.channelId))
	{
		UpdateEmptyState();
		return;
	}
	AppendHtml(MessageRowHtml(msg, drawnRoleBadges, ColorsFor(chatView)));
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
		statusLabel->setToolTip(statusLabel->text());
		SetConnectionAppearance("offline");
		return;
	}

	int connectedCount = 0;
	int connectingCount = 0;
	bool hasProblem = false;
	QStringList parts;
	for (auto it = channelStates.constBegin(); it != channelStates.constEnd(); ++it) {
		if (it.value() == ChatConnectionState::Connected)
			++connectedCount;
		else if (it.value() == ChatConnectionState::Connecting)
			++connectingCount;
		else if (it.value() == ChatConnectionState::Failed ||
			 it.value() == ChatConnectionState::MissingCredential)
			hasProblem = true;
		parts << StatusText(it.value(), channelNames.value(it.key(), it.key()), channelDetails.value(it.key()));
	}

	const int total = channelStates.size();
	if (connectedCount == total) {
		statusLabel->setText(QTStr("Multistream.Chat.AllConnected").arg(total));
		SetConnectionAppearance("connected");
	} else if (connectingCount > 0 && connectedCount == 0) {
		statusLabel->setText(QTStr("Multistream.Chat.ConnectingSummary").arg(total));
		SetConnectionAppearance("connecting");
	} else if (connectedCount > 0) {
		statusLabel->setText(QTStr("Multistream.Chat.PartiallyConnected").arg(connectedCount).arg(total));
		SetConnectionAppearance(hasProblem ? "warning" : "connecting");
	} else {
		statusLabel->setText(QTStr("Multistream.Chat.NoneConnected"));
		SetConnectionAppearance(hasProblem ? "warning" : "offline");
	}
	statusLabel->setToolTip(parts.join(QStringLiteral("\n")));
}

void UnifiedChatDock::SetConnectionAppearance(const char *state)
{
	connectionDot->setProperty("connectionState", state);
	connectionDot->style()->unpolish(connectionDot);
	connectionDot->style()->polish(connectionDot);
}

void UnifiedChatDock::RenderMessages()
{
	if (!chatView)
		return;

	const int oldScroll = chatView->verticalScrollBar()->value();
	chatView->setUpdatesEnabled(false);
	chatView->clear();
	RegisterPlatformIcons();
	const ChatColors colors = ColorsFor(chatView);
	for (const ChatMessage &message : std::as_const(messageHistory)) {
		if (ChannelFilterEnabled(message.channelId))
			chatView->append(MessageRowHtml(message, drawnRoleBadges, colors));
	}
	chatView->setUpdatesEnabled(true);
	clearButton->setEnabled(!messageHistory.isEmpty());
	UpdateEmptyState();
	if (chatStack->currentWidget() == chatView) {
		if (autoScroll->isChecked())
			chatView->verticalScrollBar()->setValue(chatView->verticalScrollBar()->maximum());
		else
			chatView->verticalScrollBar()->setValue(oldScroll);
	}
}

void UnifiedChatDock::UpdateEmptyState()
{
	bool hasVisibleMessage = false;
	for (const ChatMessage &message : std::as_const(messageHistory)) {
		if (ChannelFilterEnabled(message.channelId)) {
			hasVisibleMessage = true;
			break;
		}
	}

	if (hasVisibleMessage) {
		chatStack->setCurrentWidget(chatView);
		return;
	}

	const bool filtered = !messageHistory.isEmpty();
	emptyTitle->setText(QTStr(filtered ? "Multistream.Chat.FilteredTitle" : "Multistream.Chat.EmptyTitle"));
	emptyBody->setText(QTStr(filtered ? "Multistream.Chat.FilteredBody" : "Multistream.Chat.EmptyBody"));
	chatStack->setCurrentIndex(0);
}
