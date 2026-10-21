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
#include <QActionGroup>
#include <QCryptographicHash>
#include <QWidgetAction>
#include <QGridLayout>
#include <QUuid>
#include <util/config-file.h>
#include <QCoreApplication>
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
#include <algorithm>

#include "moc_UnifiedChatDock.cpp"

namespace {

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
	if (hexColor.match(color).hasMatch()) {
		return color;
	}
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
	if (!muted.isValid() || muted == text) {
		muted = MixColors(background, text, 0.62);
	}

	return {MixColors(background, text, 0.045).name(), MixColors(background, text, 0.15).name(), text.name(),
		muted.name(), background};
}

QString ReadableUserColor(const QString &color, StreamPlatform platform, const QColor &background)
{
	QColor result(SafeUserColor(color, platform));
	if (background.lightness() < 128 && result.lightness() < 145) {
		result = result.lighter(175);
	} else if (background.lightness() >= 128 && result.lightness() > 145) {
		result = result.darker(170);
	}
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
	case ChatConnectionState::SelectBroadcast:
		return QTStr("Multistream.Chat.SelectBroadcast").arg(platformName);
	case ChatConnectionState::WaitingForBroadcast:
		return QTStr("Multistream.Chat.WaitingForBroadcast").arg(platformName);
	case ChatConnectionState::Unsupported:
		return QTStr("Multistream.Chat.Unsupported").arg(platformName);
	case ChatConnectionState::Failed:
		if (detail == QStringLiteral("auth")) {
			return QTStr("Multistream.Chat.AuthFailed").arg(platformName);
		}
		return detail.isEmpty() ? QTStr("Multistream.Chat.Failed").arg(platformName)
					: QTStr("Multistream.Chat.FailedDetail").arg(platformName, detail);
	}
	return {};
}

QString MessageRowHtml(const ChatMessage &msg, const QSet<QString> &drawnRoles, const ChatColors &colors, bool compact,
		       bool timestamps, bool alternate, bool mention, const QString &messageHtml)
{
	const QString platformColor = PlatformColor(msg.platform);
	const QString nickColor = ReadableUserColor(msg.userColor, msg.platform, colors.background);
	QString accent = platformColor;
	QColor surface(alternate ? colors.surface : colors.background.name());
	if (mention) {
		surface = MixColors(colors.background, QColor(QStringLiteral("#eab308")), 0.18);
	}
	if (msg.kind == ChatMessageKind::SuperChat) {
		accent = QStringLiteral("#eab308");
		surface = MixColors(colors.background, QColor(accent), 0.14);
	} else if (msg.kind == ChatMessageKind::Membership) {
		accent = QStringLiteral("#22c55e");
		surface = MixColors(colors.background, QColor(accent), 0.12);
	} else if (msg.kind == ChatMessageKind::Follow) {
		accent = QStringLiteral("#2dd4bf");
		surface = MixColors(colors.background, QColor(accent), 0.10);
	} else if (msg.kind == ChatMessageKind::ChannelPoints) {
		accent = QStringLiteral("#a78bfa");
		surface = MixColors(colors.background, QColor(accent), 0.10);
	} else if (msg.kind == ChatMessageKind::Raid) {
		accent = QStringLiteral("#f472b6");
		surface = MixColors(colors.background, QColor(accent), 0.10);
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
	const QString channelBit =
		QStringLiteral("<span style=\"color:%1; font-size:10px; font-weight:600;\">&nbsp;%2</span>")
			.arg(platformColor, channelName.toHtmlEscaped());

	const QString time = timestamps ? msg.timestamp.toHtmlEscaped() : QString();
	if (compact) {
		return QStringLiteral("<p style=\"margin:2px 0; background-color:%1; color:%2;\">%3 "
				      "<span style=\"color:%4; font-size:9pt;\">%5 · %6</span> %7"
				      "<b style=\"color:%8;\">%9</b>: %10%11</p>")
			.arg(surface.name(), colors.text, PlatformBadgeHtml(msg.platform), colors.muted,
			     channelName.toHtmlEscaped(), time, RolesHtml(msg, drawnRoles), nickColor,
			     msg.senderName.toHtmlEscaped(), messageHtml, paid);
	}
	/* Qt's rich text drops margins on inline spans, so the gap between the
	 * nickname and the message has to be real characters or the two run
	 * together. The colon is what every chat client uses for the same job. */
	return QStringLiteral("<div style=\"%1\">"
			      "<div style=\"margin-bottom:4px; color:%2;\">%3%4"
			      "<span style=\"font-size:10px;\">&nbsp;&nbsp;%5</span>%6</div>"
			      "<div style=\"color:%7;\">%8<b style=\"color:%9;\">%10</b>"
			      "<span style=\"color:%7;\">:&nbsp;%11</span></div></div>")
		.arg(rowStyle, colors.muted, PlatformBadgeHtml(msg.platform), channelBit, time, paid, colors.text,
		     RolesHtml(msg, drawnRoles), nickColor, msg.senderName.toHtmlEscaped(), messageHtml);
}
} // namespace

UnifiedChatDock::UnifiedChatDock(QWidget *parent, MultiStreamChatAggregator::ConnectionFactory factory,
				 QNetworkAccessManager *emoteNetwork)
	: OBSDock(parent)
{
	setObjectName(QStringLiteral("unifiedChatDock"));
	setWindowTitle(QTStr("Multistream.Chat.Title"));

	aggregator = new MultiStreamChatAggregator(this, std::move(factory));
	emoteCache = new ChatEmoteCache(this, emoteNetwork);
	connect(emoteCache, &ChatEmoteCache::imageReady, this, [this] {
		if (autoScroll && autoScroll->isChecked() && !renderTimer->isActive()) {
			renderTimer->start(50);
		}
	});
	renderTimer = new QTimer(this);
	renderTimer->setSingleShot(true);
	connect(renderTimer, &QTimer::timeout, this, [this] {
		if (autoScroll->isChecked()) {
			RenderMessages();
		}
	});

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
	auto *settingsButton = new QToolButton(header);
	settingsButton->setObjectName(QStringLiteral("unifiedChatSettings"));
	settingsButton->setText(QTStr("Multistream.Chat.Settings"));
	titleRow->addWidget(settingsButton);
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
	statusLabel->setTextFormat(Qt::PlainText);
	statusRow->addWidget(connectionDot, 0, Qt::AlignTop);
	statusRow->addWidget(statusLabel, 1);

	autoScroll = new QCheckBox(QTStr("Multistream.Chat.AutoScroll"), header);
	autoScroll->setObjectName(QStringLiteral("unifiedChatAutoScroll"));
	autoScroll->setChecked(true);
	autoScroll->setToolTip(QTStr("Multistream.Chat.AutoScrollTip"));
	statusRow->addWidget(autoScroll, 0, Qt::AlignTop);
	headerLayout->addLayout(statusRow);
	activityStatus = new QLabel(header);
	activityStatus->setObjectName(QStringLiteral("unifiedChatActivityStatus"));
	activityStatus->setTextFormat(Qt::PlainText);
	activityStatus->setWordWrap(true);
	activityStatus->hide();
	headerLayout->addWidget(activityStatus);
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
	feedSplitter = new QSplitter(Qt::Vertical, mainWidget);
	feedSplitter->setObjectName(QStringLiteral("unifiedChatFeeds"));
	feedSplitter->addWidget(chatStack);
	activityPanel = new QWidget(feedSplitter);
	auto *activityLayout = new QVBoxLayout(activityPanel);
	activityLayout->setContentsMargins(0, 0, 0, 0);
	activityLayout->addWidget(new QLabel(QTStr("Multistream.Chat.Activity"), activityPanel));
	activityView = new QTextBrowser(activityPanel);
	activityView->setObjectName(QStringLiteral("unifiedChatActivity"));
	activityView->setOpenExternalLinks(false);
	activityView->setOpenLinks(false);
	activityLayout->addWidget(activityView);
	feedSplitter->addWidget(activityPanel);
	feedSplitter->setStretchFactor(0, 3);
	feedSplitter->setStretchFactor(1, 1);
	layout->addWidget(feedSplitter, 1);
	resumeButton = new QPushButton(mainWidget);
	resumeButton->setObjectName(QStringLiteral("unifiedChatResume"));
	resumeButton->hide();
	layout->addWidget(resumeButton);
	searchInput = new QLineEdit(mainWidget);
	searchInput->setObjectName(QStringLiteral("unifiedChatSearch"));
	searchInput->setPlaceholderText(QTStr("Multistream.Chat.Search"));
	layout->insertWidget(2, searchInput);

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

	composerLayout->addWidget(sendChannel);
	auto *emojiButton = new QToolButton(composer);
	emojiButton->setObjectName(QStringLiteral("unifiedChatEmojiButton"));
	emojiButton->setText(QStringLiteral("☺"));
	emojiButton->setToolTip(QTStr("Multistream.Chat.EmojiPicker"));
	emojiButton->setAccessibleName(QTStr("Multistream.Chat.EmojiPicker"));
	emojiButton->setPopupMode(QToolButton::InstantPopup);
	auto *emojiMenu = new QMenu(emojiButton);
	auto *emojiGrid = new QWidget(emojiMenu);
	auto *emojiLayout = new QGridLayout(emojiGrid);
	const QStringList emojis{QStringLiteral("😀"), QStringLiteral("😂"), QStringLiteral("😍"),
				 QStringLiteral("😎"), QStringLiteral("👍"), QStringLiteral("👏"),
				 QStringLiteral("🙌"), QStringLiteral("🙏"), QStringLiteral("❤️"),
				 QStringLiteral("🔥"), QStringLiteral("🎉"), QStringLiteral("✨"),
				 QStringLiteral("👋"), QStringLiteral("🎮"), QStringLiteral("🎵"),
				 QStringLiteral("💜")};
	for (int i = 0; i < emojis.size(); ++i) {
		auto *choice = new QPushButton(emojis[i], emojiGrid);
		choice->setObjectName(QStringLiteral("unifiedChatEmojiChoice"));
		choice->setProperty("emoji", emojis[i]);
		choice->setFixedSize(36, 36);
		choice->setAccessibleName(QTStr("Multistream.Chat.InsertEmoji").arg(emojis[i]));
		emojiLayout->addWidget(choice, i / 4, i % 4);
		connect(choice, &QPushButton::clicked, this, [this, emojiMenu, emoji = emojis[i]] {
			sendInput->insert(emoji);
			sendInput->setFocus();
			emojiMenu->hide();
		});
	}
	auto *emojiAction = new QWidgetAction(emojiMenu);
	emojiAction->setDefaultWidget(emojiGrid);
	emojiMenu->addAction(emojiAction);
	emojiButton->setMenu(emojiMenu);
	sendRow->addWidget(emojiButton);
	sendRow->addWidget(sendInput, 1);
	sendRow->addWidget(sendButton);
	composerLayout->addLayout(sendRow);

	sendHint = new QLabel(composer);
	sendHint->setObjectName(QStringLiteral("unifiedChatSendHint"));
	sendHint->setWordWrap(true);
	sendHint->setTextFormat(Qt::PlainText);
	composerLayout->addWidget(sendHint);
	deliveryStatus = new QLabel(composer);
	deliveryStatus->setObjectName(QStringLiteral("unifiedChatDelivery"));
	deliveryStatus->setTextFormat(Qt::PlainText);
	deliveryStatus->setWordWrap(true);
	composerLayout->addWidget(deliveryStatus);
	newMessageButton = new QPushButton(QTStr("Multistream.Chat.NewMessage"), composer);
	newMessageButton->setObjectName(QStringLiteral("unifiedChatNewMessage"));
	newMessageButton->setToolTip(QTStr("Multistream.Chat.NewMessageTip"));
	newMessageButton->hide();
	composerLayout->addWidget(newMessageButton);
	layout->addWidget(composer);
	BuildPreferences(settingsButton);

	setWidget(mainWidget);

	connect(aggregator, &MultiStreamChatAggregator::messageReceived, this, &UnifiedChatDock::OnChatMessage);
	connect(aggregator, &MultiStreamChatAggregator::messageUpdated, this, [this](const ChatMessage &update) {
		for (auto &message : messageHistory) {
			if (message.channelId == update.channelId && message.id == update.id) {
				message = update;
				break;
			}
		}
		if (autoScroll->isChecked() && !renderTimer->isActive()) {
			renderTimer->start(50);
		}
	});
	connect(aggregator, &MultiStreamChatAggregator::sendFinished, this, &UnifiedChatDock::OnSendFinished);
	connect(aggregator, &MultiStreamChatAggregator::broadcastsAvailable, this,
		[this](const QString &id, const QList<ChatBroadcast> &broadcasts) {
			availableBroadcasts[id] = broadcasts;
			UpdateBroadcastMenu();
		});
	connect(aggregator, &MultiStreamChatAggregator::messageRemoved, this,
		[this](const QString &channel, const QString &id, const QString &sender) {
			messageHistory.removeIf([&](const ChatMessage &message) {
				return message.channelId == channel &&
				       ((!id.isEmpty() && message.id == id) ||
					(!sender.isEmpty() && message.senderId == sender) ||
					(id.isEmpty() && sender.isEmpty()));
			});
			RenderMessages();
		});
	connect(searchInput, &QLineEdit::textChanged, this, &UnifiedChatDock::RenderMessages);
	connect(resumeButton, &QPushButton::clicked, this, &UnifiedChatDock::ResumeLive);
	connect(autoScroll, &QCheckBox::toggled, this, [this](bool enabled) {
		if (enabled) {
			ResumeLive();
		} else {
			UpdateUnread();
		}
	});
	for (auto *view : {chatView, activityView}) {
		connect(view, &QTextBrowser::selectionChanged, this, [this, view] {
			if (!rendering && view->textCursor().hasSelection()) {
				autoScroll->setChecked(false);
			}
		});
		connect(view->verticalScrollBar(), &QScrollBar::valueChanged, this, [this, view](int value) {
			if (!rendering && view->isVisible() && value < view->verticalScrollBar()->maximum() - 8) {
				autoScroll->setChecked(false);
			}
		});
	}
	connect(newMessageButton, &QPushButton::clicked, this, [this] {
		if (!pendingRequests.isEmpty()) {
			return;
		}
		deliveries.clear();
		batchText.clear();
		UpdateDeliveryStatus();
		UpdateSendButton();
	});
	connect(aggregator, &MultiStreamChatAggregator::statusChanged, this, &UnifiedChatDock::OnStatusChanged);
	connect(aggregator, &MultiStreamChatAggregator::activityStatusChanged, this,
		[this](const QString &id, ChatConnectionState state, const QString &detail) {
			if (!channelNames.contains(id)) {
				return;
			}
			const char *key =
				state == ChatConnectionState::Connected ? (detail == QStringLiteral("events-partial")
										   ? "Multistream.Chat.EventsPartial"
										   : "Multistream.Chat.EventsConnected")
				: state == ChatConnectionState::MissingCredential ? "Multistream.Chat.EventsAuth"
				: state == ChatConnectionState::Connecting        ? "Multistream.Chat.EventsConnecting"
										  : "Multistream.Chat.EventsFailed";
			activityDetails[id] = QTStr(key).arg(channelNames.value(id));
			UpdateActivityStatus();
		});
	connect(clearButton, &QPushButton::clicked, this, &UnifiedChatDock::ClearMessages);
	connect(sendButton, &QPushButton::clicked, this, &UnifiedChatDock::OnSendClicked);
	connect(sendInput, &QLineEdit::returnPressed, this, &UnifiedChatDock::OnSendClicked);
	connect(sendInput, &QLineEdit::textChanged, this, &UnifiedChatDock::UpdateSendButton);
	connect(sendChannel, qOverload<int>(&QComboBox::currentIndexChanged), this,
		&UnifiedChatDock::OnSendChannelChanged);
}

UnifiedChatDock::~UnifiedChatDock()
{
	Shutdown();
}

void UnifiedChatDock::Shutdown()
{
	if (shuttingDown) {
		return;
	}
	shuttingDown = true;
	renderTimer->stop();
	emoteCache->Stop();
	/* Stop socket/timer activity while all child widgets and the Qt network
	 * event dispatcher still exist. Waiting until QObject tears the dock down
	 * lets late status signals touch partially destroyed controls. */
	QObject::disconnect(aggregator, nullptr, this, nullptr);
	aggregator->StopAll();
	aggregator->DisconnectAll();
	connected = false;
	channelStates.clear();
	channelDetails.clear();
	QCoreApplication::removePostedEvents(this);
}

void UnifiedChatDock::changeEvent(QEvent *event)
{
	OBSDock::changeEvent(event);
	if (chatView && chatStack && (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange)) {
		RegisterPlatformIcons();
		RenderMessages();
	}
}

void UnifiedChatDock::showEvent(QShowEvent *event)
{
	OBSDock::showEvent(event);
	if (!shuttingDown) {
		AutoConnectAccounts();
	}
}

void UnifiedChatDock::hideEvent(QHideEvent *event)
{
	/* Layout restoration briefly hides and shows docks several times during
	 * startup. Keeping connections alive avoids duplicate API lookups and also
	 * lets the user retain messages while the dock is temporarily hidden. */
	OBSDock::hideEvent(event);
}

void UnifiedChatDock::RebuildFilters(const std::vector<ChatChannelRef> &channels)
{
	while (QLayoutItem *item = filtersLayout->takeAt(0)) {
		if (QWidget *widget = item->widget()) {
			widget->deleteLater();
		}
		delete item;
	}
	channelFilters.clear();
	channelNames.clear();
	channelPlatforms.clear();

	/* One filter per channel rather than per platform: with two accounts on
	 * the same platform, a single checkbox would hide both at once and the
	 * second account would have no label of its own anywhere. */
	for (const auto &channel : channels) {
		if (!GetStreamPlatformInfo(channel.platform).supportsChat) {
			continue;
		}

		const QString label = channel.displayName.isEmpty() ? channel.address : channel.displayName;
		channelNames.insert(channel.channelId, label);
		channelPlatforms.insert(channel.channelId, channel.platform);

		auto *box = new QToolButton(this->widget());
		box->setObjectName(QStringLiteral("unifiedChatFilter"));
		box->setText(label);
		box->setIcon(QIcon(PlatformIconProvider::Badge(channel.platform, 16)));
		box->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
		box->setCheckable(true);
		box->setChecked(
			!config_get_bool(App()->GetUserConfig(), "UnifiedChat",
					 (QStringLiteral("Hidden_") +
					  QString::fromLatin1(QCryptographicHash::hash(channel.channelId.toUtf8(),
										       QCryptographicHash::Sha256)
								      .toHex()))
						 .toUtf8()
						 .constData()));
		box->setProperty("platform", StreamPlatformId(channel.platform));
		box->setProperty("channelId", channel.channelId);
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
	if (shuttingDown) {
		return;
	}
	std::vector<ChatChannelRef> targets;
	bool unsupportedOnly = true;

	for (const auto &channel : MultistreamChannelStore::Load()) {
		if (!channel.enabled) {
			continue;
		}
		const auto &info = GetStreamPlatformInfo(channel.platform);
		if (!info.supportsChat) {
			continue;
		}
		unsupportedOnly = false;

		ChatChannelRef ref;
		ref.channelId = QString::fromStdString(channel.id);
		ref.platform = channel.platform;
		ref.displayName = QString::fromStdString(channel.displayName);
		ref.accountId = QString::fromStdString(channel.accountId);
		const QByteArray key =
			QByteArray("Broadcast_") +
			QCryptographicHash::hash(ref.channelId.toUtf8(), QCryptographicHash::Sha256).toHex();
		ref.broadcastId =
			QString::fromUtf8(config_get_string(App()->GetUserConfig(), "UnifiedChat", key.constData()));
		selectedBroadcasts[ref.channelId] = ref.broadcastId;
		/* Twitch/Kick chats are addressed by the platform handle; YouTube by
		 * account id. The handle is stored separately so renaming a channel
		 * in the interface cannot point chat at a channel that does not exist. */
		if (channel.platform == StreamPlatform::YouTube) {
			ref.address = ref.accountId;
		} else {
			ref.address = QString::fromStdString(channel.chatAddress);
			if (ref.address.isEmpty()) {
				ref.address = ref.displayName;
			}
		}
		if (ref.address.isEmpty()) {
			continue;
		}
		targets.push_back(std::move(ref));
	}

	for (const auto &id : channelStates.keys()) {
		if (std::none_of(targets.begin(), targets.end(),
				 [&](const ChatChannelRef &target) { return target.channelId == id; })) {
			channelStates.remove(id);
			channelDetails.remove(id);
			activityDetails.remove(id);
			availableBroadcasts.remove(id);
		}
	}
	RebuildFilters(targets);
	UpdateActivityStatus();
	UpdateBroadcastMenu();
	RefreshSendTargets(targets);
	OnSendChannelChanged(sendChannel->currentIndex());

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
		if (!MultiStreamChatAggregator::PlatformCanSend(channel.platform)) {
			continue;
		}
		/* The platform is in the label because two channels can share a
		 * name across platforms, and the target has to be unambiguous. */
		const QString label = channel.displayName.isEmpty() ? channel.address : channel.displayName;
		sendChannel->addItem(QIcon(PlatformIconProvider::Badge(channel.platform, 16)),
				     QStringLiteral("%1 (%2)").arg(label, PlatformName(channel.platform)),
				     channel.channelId);
	}

	if (sendChannel->count() > 1) {
		sendChannel->addItem(QTStr("Multistream.Chat.SendAll"), QStringLiteral("*"));
	}
	const int restore = sendChannel->findData(previous);
	sendChannel->setCurrentIndex(restore >= 0 ? restore : 0);
	sendChannel->blockSignals(false);
	sendChannel->setEnabled(sendChannel->count() > 0 && pendingRequests.isEmpty());
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
	const auto selected = SelectedSendTargets();
	const bool can = std::any_of(selected.begin(), selected.end(),
				     [this](const QString &id) { return aggregator->CanSend(id); });
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
	case StreamPlatform::Kick:
		sendHint->setText(QTStr("Multistream.Chat.SendNeedsKickAuth"));
		break;
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

QStringList UnifiedChatDock::SelectedSendTargets() const
{
	if (sendChannel->currentData().toString() != QStringLiteral("*")) {
		return sendChannel->count() ? QStringList{sendChannel->currentData().toString()} : QStringList{};
	}
	QStringList ids;
	for (int i = 0; i < sendChannel->count(); ++i) {
		if (sendChannel->itemData(i).toString() != QStringLiteral("*")) {
			ids.append(sendChannel->itemData(i).toString());
		}
	}
	return ids;
}

void UnifiedChatDock::OnSendClicked()
{
	if (!pendingRequests.isEmpty() || !sendTargetReady || sendInput->text().trimmed().isEmpty()) {
		return;
	}
	const QString text = sendInput->text();
	if (batchText != text) {
		deliveries.clear();
		batchText = text;
	}
	const auto targets = SelectedSendTargets();
	QHash<QString, QString> requests;
	for (const auto &id : targets) {
		if (deliveries.contains(id) && deliveries[id].state != ChatSendState::Rejected) {
			continue;
		}
		const auto requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
		requests[requestId] = id;
		pendingRequests[requestId] = id;
		deliveries.remove(id);
	}
	// Register the complete batch before providers can complete synchronously.
	UpdateDeliveryStatus();
	UpdateSendButton();
	for (auto it = requests.constBegin(); it != requests.constEnd(); ++it) {
		QString error;
		if (!aggregator->SendText(it.value(), text, it.key(), error)) {
			OnSendFinished({it.value(), it.key(), ChatSendState::Rejected, {}, error});
		}
	}
}

void UnifiedChatDock::OnSendFinished(const ChatSendResult &result)
{
	if (!pendingRequests.contains(result.requestId) ||
	    pendingRequests.value(result.requestId) != result.channelId) {
		return;
	}
	pendingRequests.remove(result.requestId);
	deliveries[result.channelId] = result;
	bool delivered = !deliveries.isEmpty();
	for (const auto &entry : std::as_const(deliveries)) {
		delivered &= entry.state == ChatSendState::Delivered;
	}
	if (pendingRequests.isEmpty() && delivered && sendInput->text() == batchText) {
		sendInput->clear();
	}
	UpdateDeliveryStatus();
	UpdateSendButton();
}

void UnifiedChatDock::UpdateDeliveryStatus()
{
	QStringList lines;
	for (const auto &id : std::as_const(pendingRequests)) {
		lines << QTStr("Multistream.Chat.DeliveryPending").arg(channelNames.value(id, id));
	}
	for (const auto &entry : std::as_const(deliveries)) {
		const QString name = channelNames.value(entry.channelId, entry.channelId);
		const char *key = entry.state == ChatSendState::Delivered ? "Multistream.Chat.DeliverySent"
				  : entry.state == ChatSendState::Unknown ? "Multistream.Chat.DeliveryUnknown"
									  : "Multistream.Chat.DeliveryFailed";
		QString line = QTStr(key).arg(name);
		if (entry.state == ChatSendState::Rejected) {
			const char *reason = entry.detail == QStringLiteral("too-long") ? "Multistream.Chat.TooLong"
					     : entry.detail == QStringLiteral("auth")   ? "Multistream.Chat.Reauthorize"
					     : entry.detail == QStringLiteral("rate-limit")
						     ? "Multistream.Chat.RateLimit"
						     : "Multistream.Chat.SendFailed";
			line += QStringLiteral(" — ") + QTStr(reason);
		}
		lines << line;
	}
	deliveryStatus->setText(lines.join(QLatin1Char('\n')));
	newMessageButton->setVisible(!deliveries.isEmpty() && pendingRequests.isEmpty());
	sendChannel->setEnabled(pendingRequests.isEmpty() && sendChannel->count() > 0);
}

void UnifiedChatDock::OnFilterToggled()
{
	SavePreferences();
	RenderMessages();
}

void UnifiedChatDock::ClearMessages()
{
	unread = 0;
	UpdateUnread();
	activityView->clear();
	activityPanel->hide();
	messageHistory.clear();
	clearButton->setEnabled(false);
	chatView->clear();
	RegisterPlatformIcons();
	UpdateEmptyState();
}

void UnifiedChatDock::UpdateSendButton()
{
	bool eligible = false;
	const bool same = batchText == sendInput->text();
	for (const auto &id : SelectedSendTargets()) {
		eligible |= !same || !deliveries.contains(id) || deliveries.value(id).state == ChatSendState::Rejected;
	}
	sendButton->setText(
		QTStr(same && !deliveries.isEmpty() ? "Multistream.Chat.RetryFailed" : "Multistream.Chat.Send"));
	sendButton->setEnabled(pendingRequests.isEmpty() && sendTargetReady && eligible &&
			       !sendInput->text().trimmed().isEmpty());
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
		if (glyph.isNull()) {
			continue;
		}
		for (auto *view : {chatView, activityView}) {
			if (view) {
				view->document()->addResource(QTextDocument::ImageResource,
							      QUrl(PlatformIconUrl(platform)), QVariant(glyph));
			}
		}
	}

	/* A role with no artwork is left out of the set and keeps the text pill. */
	drawnRoleBadges.clear();
	for (const QString &label : RoleBadgeLabels()) {
		QPixmap glyph = ChatBadgeIcons::Glyph(label, ROLE_BADGE_SIZE, ratio);
		if (glyph.isNull()) {
			continue;
		}
		for (auto *view : {chatView, activityView}) {
			if (view) {
				view->document()->addResource(QTextDocument::ImageResource, QUrl(RoleBadgeUrl(label)),
							      QVariant(glyph));
			}
		}
		drawnRoleBadges.insert(label);
	}
}

void UnifiedChatDock::OnChatMessage(const ChatMessage &msg)
{
	messageHistory.append(msg);
	while (messageHistory.size() > MAX_CHAT_MESSAGES) {
		messageHistory.removeFirst();
	}
	clearButton->setEnabled(true);
	if (!autoScroll->isChecked()) {
		if (MessageVisible(msg)) {
			++unread;
			UpdateUnread();
		}
		return; // Freeze the document while the user reads; history remains bounded.
	}
	if (!renderTimer->isActive()) {
		renderTimer->start(50);
	}
}

void UnifiedChatDock::OnStatusChanged(const QString &channelId, StreamPlatform platform, ChatConnectionState state,
				      const QString &detail)
{
	if (!channelPlatforms.contains(channelId)) {
		return;
	}
	channelStates.insert(channelId, state);
	channelDetails.insert(channelId, detail);
	if (!channelNames.contains(channelId)) {
		channelNames.insert(channelId, PlatformName(platform));
	}
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
		if (it.value() == ChatConnectionState::Connected) {
			++connectedCount;
		} else if (it.value() == ChatConnectionState::Connecting) {
			++connectingCount;
		} else if (it.value() == ChatConnectionState::Failed ||
			   it.value() == ChatConnectionState::MissingCredential) {
			hasProblem = true;
		}
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

bool UnifiedChatDock::MessageVisible(const ChatMessage &message) const
{
	if (!ChannelFilterEnabled(message.channelId)) {
		return false;
	}
	if ((message.kind == ChatMessageKind::Follow && !followAction->isChecked()) ||
	    (message.kind == ChatMessageKind::Raid && !raidAction->isChecked()) ||
	    (message.kind == ChatMessageKind::ChannelPoints && !pointsAction->isChecked())) {
		return false;
	}
	if (message.kind == ChatMessageKind::SuperChat && !paidAction->isChecked()) {
		return false;
	}
	if (message.kind == ChatMessageKind::Membership && !membershipAction->isChecked()) {
		return false;
	}
	if (message.kind == ChatMessageKind::System && !systemAction->isChecked()) {
		return false;
	}
	const QString query = searchInput->text().trimmed();
	return query.isEmpty() || message.messageText.contains(query, Qt::CaseInsensitive) ||
	       message.senderName.contains(query, Qt::CaseInsensitive) ||
	       message.eventTitle.contains(query, Qt::CaseInsensitive) ||
	       message.channelName.contains(query, Qt::CaseInsensitive);
}

QString UnifiedChatDock::RowHtml(const ChatMessage &message, int index, QTextBrowser *view) const
{
	bool mention = false;
	const auto words = mentionInput->text().split(',', Qt::SkipEmptyParts);
	for (const auto &word : words) {
		if (!word.trimmed().isEmpty() && message.messageText.contains(word.trimmed(), Qt::CaseInsensitive)) {
			mention = true;
		}
	}
	QString summary;
	const auto &type = message.eventType;
	if (type == QStringLiteral("follow")) {
		summary = QTStr("Multistream.Chat.EventFollow");
	} else if (type == QStringLiteral("raid")) {
		summary = QTStr("Multistream.Chat.EventRaid").arg(message.eventCount);
	} else if (type == QStringLiteral("points") || type == QStringLiteral("points-automatic")) {
		summary = QTStr("Multistream.Chat.EventPoints")
				  .arg(type == QStringLiteral("points-automatic")
					       ? QTStr(message.eventTitle == QStringLiteral("send_highlighted_message")
							       ? "Multistream.Chat.HighlightedReward"
							       : "Multistream.Chat.AutomaticReward")
					       : message.eventTitle)
				  .arg(message.eventCount);
	} else if (type == QStringLiteral("subscription")) {
		summary = QTStr("Multistream.Chat.EventSubscription");
	} else if (type == QStringLiteral("resubscription")) {
		summary = message.eventCount > 0 ? QTStr("Multistream.Chat.EventResubscription").arg(message.eventCount)
						 : QTStr("Multistream.Chat.EventRenewal");
	} else if (type == QStringLiteral("subscription-gift") || type == QStringLiteral("membership-gift")) {
		summary = message.eventCount > 0 ? QTStr("Multistream.Chat.EventGiftCount").arg(message.eventCount)
						 : QTStr("Multistream.Chat.EventGift").arg(message.eventTitle);
	} else if (type == QStringLiteral("membership-received")) {
		summary = QTStr("Multistream.Chat.EventGiftReceived");
	}
	QString body = emotesAction->isChecked()
			       ? emoteCache->Html(message, view->document(), std::clamp(fontSize * 2, 20, 36))
			       : message.messageText.toHtmlEscaped();
	if (!summary.isEmpty()) {
		body.prepend(QStringLiteral("<b>%1</b>%2")
				     .arg(summary.toHtmlEscaped(), body.isEmpty() ? QString() : QStringLiteral(" · ")));
	}
	return MessageRowHtml(message, drawnRoleBadges, ColorsFor(view), compactAction->isChecked(),
			      timestampsAction->isChecked(), alternatingAction->isChecked() && (index % 2 == 0),
			      mention, body);
}

void UnifiedChatDock::RenderMessages()
{
	if (!chatView || !compactAction) {
		return;
	}
	rendering = true;
	const int oldScroll = chatView->verticalScrollBar()->value();
	const int oldActivity = activityView->verticalScrollBar()->value();
	chatView->setUpdatesEnabled(false);
	activityView->setUpdatesEnabled(false);
	chatView->clear();
	activityView->clear();
	RegisterPlatformIcons();
	int rows = 0, events = 0;
	for (const auto &message : std::as_const(messageHistory)) {
		if (!MessageVisible(message)) {
			continue;
		}
		const bool activity = splitAction->isChecked() && message.kind != ChatMessageKind::Normal;
		auto *view = activity ? activityView : chatView;
		view->append(RowHtml(message, activity ? events++ : rows++, view));
	}
	activityPanel->setVisible(splitAction->isChecked() && events > 0);
	chatView->setUpdatesEnabled(true);
	activityView->setUpdatesEnabled(true);
	clearButton->setEnabled(!messageHistory.isEmpty());
	UpdateEmptyState();
	for (auto *view : {chatView, activityView}) {
		view->verticalScrollBar()->setValue(autoScroll->isChecked() ? view->verticalScrollBar()->maximum()
						    : view == chatView      ? oldScroll
									    : oldActivity);
	}
	rendering = false;
}

void UnifiedChatDock::UpdateEmptyState()
{
	for (const auto &message : std::as_const(messageHistory)) {
		if (MessageVisible(message) && (!splitAction->isChecked() || message.kind == ChatMessageKind::Normal)) {
			chatStack->setCurrentWidget(chatView);
			return;
		}
	}
	const bool filtered = !messageHistory.isEmpty();
	emptyTitle->setText(QTStr(filtered ? "Multistream.Chat.FilteredTitle" : "Multistream.Chat.EmptyTitle"));
	emptyBody->setText(QTStr(filtered ? "Multistream.Chat.FilteredBody" : "Multistream.Chat.EmptyBody"));
	chatStack->setCurrentIndex(0);
}

void UnifiedChatDock::ResumeLive()
{
	unread = 0;
	autoScroll->blockSignals(true);
	autoScroll->setChecked(true);
	autoScroll->blockSignals(false);
	RenderMessages();
	UpdateUnread();
}

void UnifiedChatDock::UpdateUnread()
{
	resumeButton->setVisible(!autoScroll->isChecked());
	resumeButton->setText(QTStr("Multistream.Chat.Resume").arg(unread));
}

void UnifiedChatDock::BuildPreferences(QToolButton *button)
{
	auto *menu = new QMenu(button);
	button->setMenu(menu);
	button->setPopupMode(QToolButton::InstantPopup);
	auto *config = App()->GetUserConfig();
	fontSize = config_has_user_value(config, "UnifiedChat", "FontSize")
			   ? config_get_int(config, "UnifiedChat", "FontSize")
			   : 12;
	fontSize = std::clamp(fontSize, 10, 18);
	auto *fonts = menu->addMenu(QTStr("Multistream.Chat.FontSize"));
	auto *group = new QActionGroup(fonts);
	for (int size : {10, 12, 15, 18}) {
		auto *action = fonts->addAction(QStringLiteral("%1 pt").arg(size));
		action->setCheckable(true);
		action->setChecked(fontSize == size);
		group->addAction(action);
		connect(action, &QAction::triggered, this, [this, size] {
			fontSize = size;
			SavePreferences();
			RenderMessages();
		});
	}
	auto preference = [&](const char *key, bool fallback) {
		auto *action = menu->addAction(QTStr((std::string("Multistream.Chat.") + key).c_str()));
		action->setObjectName(QString::fromUtf8(key));
		action->setCheckable(true);
		action->setChecked(config_has_user_value(config, "UnifiedChat", key)
					   ? config_get_bool(config, "UnifiedChat", key)
					   : fallback);
		connect(action, &QAction::toggled, this, [this] {
			SavePreferences();
			RenderMessages();
		});
		return action;
	};
	compactAction = preference("Compact", true);
	alternatingAction = preference("Alternating", true);
	timestampsAction = preference("Timestamps", true);
	splitAction = preference("SplitActivity", true);
	menu->addSeparator();
	paidAction = preference("PaidEvents", true);
	membershipAction = preference("MembershipEvents", true);
	systemAction = preference("SystemEvents", true);
	followAction = preference("FollowEvents", true);
	raidAction = preference("RaidEvents", true);
	pointsAction = preference("PointEvents", true);
	auto *disableEvents = menu->addAction(QTStr("Multistream.Chat.DisableEvents"));
	disableEvents->setObjectName(QStringLiteral("DisableAllEvents"));
	connect(disableEvents, &QAction::triggered, this, [this] {
		for (auto *action :
		     {paidAction, membershipAction, systemAction, followAction, raidAction, pointsAction}) {
			action->setChecked(false);
		}
	});
	emotesAction = preference("ShowEmotes", true);
	menu->addSeparator();
	auto *mentionWidget = new QWidget(menu);
	auto *mentionLayout = new QVBoxLayout(mentionWidget);
	mentionLayout->addWidget(new QLabel(QTStr("Multistream.Chat.Highlight"), mentionWidget));
	mentionInput = new QLineEdit(mentionWidget);
	mentionInput->setObjectName(QStringLiteral("unifiedChatHighlight"));
	mentionInput->setText(QString::fromUtf8(config_get_string(config, "UnifiedChat", "Highlight")));
	mentionInput->setPlaceholderText(QTStr("Multistream.Chat.HighlightExample"));
	mentionLayout->addWidget(mentionInput);
	auto *widgetAction = new QWidgetAction(menu);
	widgetAction->setDefaultWidget(mentionWidget);
	menu->addAction(widgetAction);
	connect(mentionInput, &QLineEdit::editingFinished, this, [this] {
		SavePreferences();
		RenderMessages();
	});
	broadcastMenu = menu->addMenu(QTStr("Multistream.Chat.YouTubeBroadcast"));
	UpdateBroadcastMenu();
	activityPanel->setVisible(splitAction->isChecked());
	for (auto *view : {chatView, activityView}) {
		auto font = view->font();
		font.setPointSize(fontSize);
		view->setFont(font);
	}
}

void UnifiedChatDock::SavePreferences()
{
	auto *config = App()->GetUserConfig();
	for (auto *action : {compactAction, alternatingAction, timestampsAction, splitAction, paidAction,
			     membershipAction, systemAction, followAction, raidAction, pointsAction, emotesAction}) {
		config_set_bool(config, "UnifiedChat", action->objectName().toUtf8().constData(), action->isChecked());
	}
	config_set_int(config, "UnifiedChat", "FontSize", fontSize);
	config_set_string(config, "UnifiedChat", "Highlight", mentionInput->text().toUtf8().constData());
	for (auto it = channelFilters.constBegin(); it != channelFilters.constEnd(); ++it) {
		const QByteArray key = QByteArray("Hidden_") +
				       QCryptographicHash::hash(it.key().toUtf8(), QCryptographicHash::Sha256).toHex();
		config_set_bool(config, "UnifiedChat", key.constData(), !it.value()->isChecked());
	}
	config_save_safe(config, "tmp", "bak");
	activityPanel->setVisible(splitAction->isChecked());
	for (auto *view : {chatView, activityView}) {
		auto font = view->font();
		font.setPointSize(fontSize);
		view->setFont(font);
	}
}

void UnifiedChatDock::UpdateBroadcastMenu()
{
	if (!broadcastMenu) {
		return;
	}
	broadcastMenu->clear();
	for (auto it = availableBroadcasts.constBegin(); it != availableBroadcasts.constEnd(); ++it) {
		auto *channelMenu = broadcastMenu->addMenu(channelNames.value(it.key(), it.key()));
		auto *group = new QActionGroup(channelMenu);
		const QString id = it.key();
		auto addChoice = [&](const QString &broadcastId, const QString &title) {
			auto *action = channelMenu->addAction(title);
			action->setCheckable(true);
			group->addAction(action);
			action->setChecked(selectedBroadcasts.value(id) == broadcastId);
			connect(action, &QAction::triggered, this, [this, id, broadcastId] {
				const QByteArray key =
					QByteArray("Broadcast_") +
					QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha256).toHex();
				config_set_string(App()->GetUserConfig(), "UnifiedChat", key.constData(),
						  broadcastId.toUtf8().constData());
				config_save_safe(App()->GetUserConfig(), "tmp", "bak");
				QTimer::singleShot(0, this, &UnifiedChatDock::AutoConnectAccounts);
			});
		};
		addChoice({}, QTStr("Multistream.Chat.AutoSingleBroadcast"));
		for (const auto &broadcast : it.value()) {
			addChoice(broadcast.id, broadcast.title + QStringLiteral(" · ") + broadcast.id);
		}
	}
	broadcastMenu->setEnabled(!availableBroadcasts.isEmpty());
}

void UnifiedChatDock::UpdateActivityStatus()
{
	const auto details = activityDetails.values();
	activityStatus->setVisible(!details.isEmpty());
	activityStatus->setText(details.size() == 1 ? details.first()
						    : QTStr("Multistream.Chat.EventAccounts").arg(details.size()));
	activityStatus->setToolTip(details.join(QStringLiteral("\n")));
}
