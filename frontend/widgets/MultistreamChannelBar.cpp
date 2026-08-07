/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "MultistreamChannelBar.hpp"

#include <utility/ChannelAvatarCache.hpp>
#include <utility/PlatformIconProvider.hpp>
#include <utility/StreamPlatformDisplay.hpp>

#include <OBSApp.hpp>
#include <qt-wrappers.hpp>

#include <QCheckBox>
#include <QColor>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "moc_MultistreamChannelBar.cpp"

namespace {
constexpr int AVATAR_SIZE = 28;
constexpr int HEALTH_REFRESH_MS = 1000;

QString StateText(MultiStreamChannelState state)
{
	switch (state) {
	case MultiStreamChannelState::Idle:
		return QTStr("Multistream.ChannelBar.Offline");
	case MultiStreamChannelState::Starting:
		return QTStr("Multistream.ChannelBar.Connecting");
	case MultiStreamChannelState::Live:
		return QTStr("Multistream.ChannelBar.Live");
	case MultiStreamChannelState::Stopping:
		return QTStr("Multistream.ChannelBar.Stopping");
	case MultiStreamChannelState::Failed:
		return QTStr("Multistream.ChannelBar.Failed");
	}
	return {};
}

const char *StatePropertyValue(MultiStreamChannelState state)
{
	switch (state) {
	case MultiStreamChannelState::Live:
		return "live";
	case MultiStreamChannelState::Failed:
		return "failed";
	case MultiStreamChannelState::Starting:
	case MultiStreamChannelState::Stopping:
		return "busy";
	case MultiStreamChannelState::Idle:
		break;
	}
	return "idle";
}

QString FormatDuration(int64_t seconds)
{
	const int64_t hours = seconds / 3600;
	const int64_t minutes = (seconds % 3600) / 60;
	if (hours > 0)
		return QStringLiteral("%1 h %2 min").arg(hours).arg(minutes);
	if (minutes > 0)
		return QStringLiteral("%1 min").arg(minutes);
	return QStringLiteral("%1 s").arg(seconds);
}

/* Three bars: full and green when the send buffer is keeping up, shrinking and
 * amber as congestion rises. */
QString HealthMark(const MultiStreamChannelHealth &health, MultiStreamChannelState state)
{
	if (state != MultiStreamChannelState::Live)
		return {};
	if (health.congestion >= 0.6f)
		return QStringLiteral("\u2581\u2582_");
	if (health.congestion >= 0.25f)
		return QStringLiteral("\u2581\u2582\u2583");
	return QStringLiteral("\u2581\u2583\u2585");
}

const char *HealthPropertyValue(const MultiStreamChannelHealth &health)
{
	if (health.congestion >= 0.6f)
		return "bad";
	if (health.congestion >= 0.25f)
		return "warn";
	return "good";
}

QString HealthToolTip(const MultiStreamChannelHealth &health, const QString &lastError)
{
	if (!lastError.isEmpty())
		return lastError;

	QStringList lines;
	lines << QTStr("Multistream.ChannelBar.LiveFor").arg(FormatDuration(health.liveSeconds));
	if (health.totalFrames > 0) {
		const double droppedPercent = 100.0 * health.droppedFrames / health.totalFrames;
		lines << QTStr("Multistream.ChannelBar.DroppedFrames")
				 .arg(health.droppedFrames)
				 .arg(QString::number(droppedPercent, 'f', 1));
	}
	lines << QTStr("Multistream.ChannelBar.Congestion")
			 .arg(QString::number(static_cast<double>(health.congestion) * 100.0, 'f', 0));
	if (health.reconnects > 0)
		lines << QTStr("Multistream.ChannelBar.Reconnects").arg(health.reconnects);
	return lines.join(QChar('\n'));
}

} // namespace

MultistreamChannelBar::MultistreamChannelBar(QWidget *parent) : QFrame(parent)
{
	setObjectName(QStringLiteral("multistreamChannelBar"));
	setFrameShape(QFrame::StyledPanel);
	/* Colors come from the active theme (see Yami.obt); a widget stylesheet
	 * here would override it and ignore light themes entirely. */

	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(10, 6, 10, 6);
	outer->setSpacing(6);

	auto *row = new QHBoxLayout();
	row->setContentsMargins(0, 0, 0, 0);
	row->setSpacing(12);

	auto *summary = new QVBoxLayout();
	summary->setContentsMargins(0, 0, 0, 0);
	summary->setSpacing(0);
	summaryTitle = new QLabel(QTStr("Multistream.ChannelBar.Title"), this);
	summaryTitle->setObjectName(QStringLiteral("channelBarTitle"));
	summarySubtitle = new QLabel(this);
	summarySubtitle->setObjectName(QStringLiteral("channelBarSubtitle"));
	summary->addWidget(summaryTitle);
	summary->addWidget(summarySubtitle);
	row->addLayout(summary);

	/* Many destinations must not push the preview around, so the strip
	 * scrolls horizontally instead of growing. */
	channelsContainer = new QWidget(this);
	channelsLayout = new QHBoxLayout(channelsContainer);
	channelsLayout->setContentsMargins(0, 0, 0, 0);
	channelsLayout->setSpacing(8);

	channelsArea = new QScrollArea(this);
	channelsArea->setObjectName(QStringLiteral("channelBarScroll"));
	channelsArea->setWidget(channelsContainer);
	channelsArea->setWidgetResizable(true);
	channelsArea->setFrameShape(QFrame::NoFrame);
	channelsArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	channelsArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	channelsArea->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	channelsArea->setFixedHeight(52);
	row->addWidget(channelsArea, 1);

	ShowPlaceholder(QTStr("Multistream.ChannelBar.Empty"));

	addButton = new QPushButton(QStringLiteral("+"), this);
	addButton->setObjectName(QStringLiteral("channelBarAdd"));
	addButton->setToolTip(QTStr("Multistream.ChannelBar.Add"));
	addButton->setFixedSize(30, 30);
	addButton->setCursor(Qt::PointingHandCursor);
	connect(addButton, &QPushButton::clicked, this, &MultistreamChannelBar::addChannelRequested);
	row->addWidget(addButton);

	collapseButton = new QToolButton(this);
	collapseButton->setObjectName(QStringLiteral("channelBarCollapse"));
	collapseButton->setArrowType(Qt::UpArrow);
	collapseButton->setAutoRaise(true);
	collapseButton->setToolTip(QTStr("Multistream.ChannelBar.Collapse"));
	connect(collapseButton, &QToolButton::clicked, this, [this]() { SetCollapsed(!collapsed); });
	row->addWidget(collapseButton);

	outer->addLayout(row);

	warningRow = new QWidget(this);
	auto *warningLayout = new QHBoxLayout(warningRow);
	warningLayout->setContentsMargins(0, 0, 0, 0);
	warningLayout->setSpacing(7);
	warningIcon = new QLabel(QStringLiteral("\u26a0"), warningRow);
	warningIcon->setObjectName(QStringLiteral("channelBarWarningIcon"));
	warningText = new QLabel(warningRow);
	warningText->setObjectName(QStringLiteral("channelBarWarning"));
	warningText->setWordWrap(false);
	warningLayout->addWidget(warningIcon);
	warningLayout->addWidget(warningText, 1);
	warningRow->setVisible(false);
	outer->addWidget(warningRow);

	healthTimer = new QTimer(this);
	healthTimer->setInterval(HEALTH_REFRESH_MS);
	connect(healthTimer, &QTimer::timeout, this, &MultistreamChannelBar::healthRefreshRequested);

	connect(&ChannelAvatarCache::Instance(), &ChannelAvatarCache::avatarReady, this,
		&MultistreamChannelBar::ApplyAvatar);

	UpdateSummary();
}

void MultistreamChannelBar::SetCollapsed(bool value)
{
	collapsed = value;
	channelsArea->setVisible(!collapsed);
	addButton->setVisible(!collapsed);
	warningRow->setVisible(!collapsed && !warningText->text().isEmpty());
	collapseButton->setArrowType(collapsed ? Qt::DownArrow : Qt::UpArrow);
	collapseButton->setToolTip(collapsed ? QTStr("Multistream.ChannelBar.Expand")
					     : QTStr("Multistream.ChannelBar.Collapse"));
}

void MultistreamChannelBar::ShowPlaceholder(const QString &text, const QString &toolTip)
{
	auto *label = new QLabel(text, channelsContainer);
	label->setObjectName(QStringLiteral("channelBarEmpty"));
	label->setToolTip(toolTip);
	emptyState = label;
	channelsLayout->addWidget(label);
	channelsLayout->addStretch(1);
}

void MultistreamChannelBar::ClearChannels()
{
	while (QLayoutItem *item = channelsLayout->takeAt(0)) {
		if (item->widget())
			item->widget()->deleteLater();
		delete item;
	}
	channelWidgets.clear();
	channelStates.clear();
	emptyState = nullptr;
}

void MultistreamChannelBar::SetChannels(const std::vector<MultiStreamChannel> &channels)
{
	ClearChannels();
	totalChannels = static_cast<int>(channels.size());

	if (channels.empty()) {
		ShowPlaceholder(QTStr("Multistream.ChannelBar.Empty"));
		UpdateSummary();
		return;
	}

	for (const auto &channel : channels)
		channelsLayout->addWidget(CreateChannelCard(channel));
	channelsLayout->addStretch(1);
	UpdateSummary();
}

QWidget *MultistreamChannelBar::CreateChannelCard(const MultiStreamChannel &channel)
{
	const QString id = QString::fromStdString(channel.id);
	const QString displayName = QString::fromStdString(channel.displayName);

	auto *card = new QFrame(channelsContainer);
	card->setObjectName(QStringLiteral("channelCard"));
	card->setProperty("channelCard", true);
	card->setProperty("platform", StreamPlatformId(channel.platform));
	card->setProperty("channelEnabled", channel.enabled);

	auto *cardLayout = new QHBoxLayout(card);
	cardLayout->setContentsMargins(8, 4, 6, 4);
	cardLayout->setSpacing(8);

	auto *avatar = new QLabel(card);
	avatar->setObjectName(QStringLiteral("channelAvatar"));
	avatar->setAlignment(Qt::AlignCenter);
	avatar->setFixedSize(AVATAR_SIZE, AVATAR_SIZE);
	avatar->setToolTip(StreamPlatformDisplayName(channel.platform));
	/* Platform badge until the channel's own picture arrives. */
	avatar->setPixmap(PlatformIconProvider::Badge(channel.platform, AVATAR_SIZE));
	cardLayout->addWidget(avatar);

	auto *labels = new QVBoxLayout();
	labels->setContentsMargins(0, 0, 0, 0);
	labels->setSpacing(0);

	auto *name = new QLabel(displayName, card);
	name->setObjectName(QStringLiteral("channelName"));
	name->setToolTip(QStringLiteral("%1 · %2").arg(StreamPlatformDisplayName(channel.platform), displayName));

	auto *stateRow = new QHBoxLayout();
	stateRow->setContentsMargins(0, 0, 0, 0);
	stateRow->setSpacing(5);

	auto *state = new QLabel(StateText(MultiStreamChannelState::Idle), card);
	state->setObjectName(QStringLiteral("channelState"));
	state->setProperty("channelState", "idle");

	auto *health = new QLabel(card);
	health->setObjectName(QStringLiteral("channelHealth"));
	health->setProperty("healthState", "good");

	stateRow->addWidget(state);
	stateRow->addWidget(health);
	stateRow->addStretch(1);

	labels->addWidget(name);
	labels->addLayout(stateRow);
	cardLayout->addLayout(labels);

	auto *enabled = new QCheckBox(card);
	enabled->setObjectName(QStringLiteral("channelToggle"));
	enabled->setChecked(channel.enabled);
	enabled->setToolTip(QTStr("Multistream.ChannelBar.ToggleTip"));
	connect(enabled, &QCheckBox::toggled, this, [this, id](bool checked) {
		if (auto item = channelWidgets.find(id); item != channelWidgets.end()) {
			item->card->setProperty("channelEnabled", checked);
			if (item->card->style()) {
				item->card->style()->unpolish(item->card);
				item->card->style()->polish(item->card);
			}
		}
		emit channelEnabledChanged(id, checked);
	});
	cardLayout->addWidget(enabled);

	auto *menuButton = new QToolButton(card);
	menuButton->setObjectName(QStringLiteral("channelMenu"));
	menuButton->setText(QStringLiteral("\u22ee"));
	menuButton->setAutoRaise(true);
	menuButton->setPopupMode(QToolButton::InstantPopup);
	/* The button already shows a menu glyph; Qt's own drop-down arrow on top
	 * of it reads as two controls. */
	menuButton->setStyleSheet(QStringLiteral("QToolButton::menu-indicator { image: none; width: 0; }"));
	menuButton->setToolTip(QTStr("Multistream.ChannelBar.ChannelMenu"));

	auto *menu = new QMenu(menuButton);
	connect(menu->addAction(QTStr("Multistream.ChannelBar.Reconnect")), &QAction::triggered, this,
		[this, id]() { emit reconnectChannelRequested(id); });
	menu->addSeparator();
	const bool manual = GetStreamPlatformInfo(channel.platform).ingestMode == StreamIngestMode::ManualStreamKey;
	if (manual) {
		connect(menu->addAction(QTStr("Multistream.ChannelBar.EditChannel")), &QAction::triggered, this,
			[this, id]() { emit editChannelRequested(id); });
	} else {
		connect(menu->addAction(QTStr("Multistream.ChannelBar.ManageAccounts")), &QAction::triggered, this,
			[this, id]() { emit manageAccountRequested(id); });
	}
	connect(menu->addAction(QTStr("Multistream.ChannelBar.RemoveChannel")), &QAction::triggered, this,
		[this, id]() { emit removeChannelRequested(id); });
	menuButton->setMenu(menu);
	cardLayout->addWidget(menuButton);

	channelWidgets.insert(id, {avatar, state, health, enabled, card, channel.platform, displayName,
				   QString::fromStdString(channel.avatarUrl)});
	channelStates.insert(id, MultiStreamChannelState::Idle);
	ApplyAvatar(id);
	return card;
}

void MultistreamChannelBar::ApplyAvatar(const QString &channelId)
{
	auto item = channelWidgets.find(channelId);
	if (item == channelWidgets.end() || item->avatarUrl.isEmpty())
		return;

	const QPixmap pixmap = ChannelAvatarCache::Instance().Avatar(channelId, item->avatarUrl, AVATAR_SIZE);
	if (pixmap.isNull())
		return;

	/* The channel's own picture replaces the platform badge once it loads. */
	item->avatar->setPixmap(pixmap);
}

void MultistreamChannelBar::ApplySnapshots(const std::vector<MultiStreamChannelSnapshot> &snapshots)
{
	for (const auto &snapshot : snapshots)
		UpdateState(snapshot);
}

void MultistreamChannelBar::ShowRestoringAccounts()
{
	SetChannels({});
	if (emptyState)
		emptyState->setText(QTStr("Multistream.ChannelBar.Restoring"));
}

void MultistreamChannelBar::ShowRestoreFailure(const QString &details)
{
	SetChannels({});
	if (emptyState) {
		emptyState->setText(QTStr("Multistream.ChannelBar.RestoreFailed"));
		emptyState->setToolTip(details);
	}
}

void MultistreamChannelBar::ShowPreflightFindings(const std::vector<PreflightFinding> &findings)
{
	if (findings.empty()) {
		warningText->clear();
		warningText->setToolTip(QString());
		warningRow->setVisible(false);
		return;
	}

	QStringList messages;
	bool hasWarning = false;
	for (const auto &finding : findings) {
		messages << QString::fromStdString(finding.message);
		hasWarning = hasWarning || finding.severity == PreflightSeverity::Warning;
	}

	/* One line in the bar, the full list in the tooltip: the strip must not
	 * grow and push the preview down. */
	warningText->setText(messages.size() == 1
				     ? messages.first()
				     : QTStr("Multistream.Preflight.Summary")
					       .arg(messages.first())
					       .arg(messages.size() - 1));
	warningText->setToolTip(messages.join(QChar('\n')));
	warningIcon->setProperty("severity", hasWarning ? "warning" : "advisory");
	warningText->setProperty("severity", hasWarning ? "warning" : "advisory");
	for (QWidget *widget : {static_cast<QWidget *>(warningIcon), static_cast<QWidget *>(warningText)}) {
		if (widget->style()) {
			widget->style()->unpolish(widget);
			widget->style()->polish(widget);
		}
	}
	warningRow->setVisible(!collapsed);
}

void MultistreamChannelBar::UpdateSummary()
{
	int liveCount = 0;
	for (const auto state : channelStates) {
		if (state == MultiStreamChannelState::Live)
			++liveCount;
	}

	if (totalChannels == 0) {
		/* The placeholder in the strip already says there is nothing set up;
		 * repeating it in the subtitle just adds noise. */
		summarySubtitle->clear();
	} else if (liveCount == 0) {
		summarySubtitle->setText(totalChannels == 1
						 ? QTStr("Multistream.ChannelBar.SummaryIdleOne")
						 : QTStr("Multistream.ChannelBar.SummaryIdle").arg(totalChannels));
	} else {
		summarySubtitle->setText(QTStr("Multistream.ChannelBar.SummaryLive").arg(liveCount).arg(totalChannels));
	}

	/* Only poll health while something is actually sending. */
	if (liveCount > 0 && !healthTimer->isActive())
		healthTimer->start();
	else if (liveCount == 0 && healthTimer->isActive())
		healthTimer->stop();
}

void MultistreamChannelBar::UpdateState(const MultiStreamChannelSnapshot &snapshot)
{
	const QString id = QString::fromStdString(snapshot.id);
	auto item = channelWidgets.find(id);
	if (item == channelWidgets.end())
		return;

	const QString lastError = QString::fromStdString(snapshot.lastError);
	item->state->setText(StateText(snapshot.state));
	item->state->setProperty("channelState", StatePropertyValue(snapshot.state));
	if (item->state->style()) {
		item->state->style()->unpolish(item->state);
		item->state->style()->polish(item->state);
	}

	item->health->setText(HealthMark(snapshot.health, snapshot.state));
	item->health->setProperty("healthState", HealthPropertyValue(snapshot.health));
	if (item->health->style()) {
		item->health->style()->unpolish(item->health);
		item->health->style()->polish(item->health);
	}

	const QString tip = snapshot.state == MultiStreamChannelState::Live
				    ? HealthToolTip(snapshot.health, lastError)
				    : lastError;
	item->state->setToolTip(tip);
	item->health->setToolTip(tip);

	/* A destination that dropped on its own must not keep showing as enabled,
	 * otherwise the toggle no longer matches what is actually being sent. */
	if (snapshot.state == MultiStreamChannelState::Failed && item->enabled->isChecked()) {
		QSignalBlocker blocker(item->enabled);
		item->enabled->setChecked(false);
		item->card->setProperty("channelEnabled", false);
		if (item->card->style()) {
			item->card->style()->unpolish(item->card);
			item->card->style()->polish(item->card);
		}
	}

	channelStates.insert(id, snapshot.state);
	UpdateSummary();
}
