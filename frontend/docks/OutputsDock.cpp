/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "OutputsDock.hpp"

#include <utility/StreamPlatformDisplay.hpp>

#include <qt-wrappers.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QVBoxLayout>

#include <algorithm>

#include "moc_OutputsDock.cpp"

namespace {
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

const char *StateValue(MultiStreamChannelState state)
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

void Repolish(QWidget *widget)
{
	widget->style()->unpolish(widget);
	widget->style()->polish(widget);
}

QString CanvasName(MultiStreamVideoLayout layout)
{
	return layout == MultiStreamVideoLayout::Portrait ? QTStr("Outputs.Canvas.Portrait")
							  : QTStr("Outputs.Canvas.Main");
}
} // namespace

OutputsDock::OutputsDock(QWidget *parent) : OBSDock(parent)
{
	setObjectName(QStringLiteral("outputsDock"));
	setWindowTitle(QTStr("Outputs.Title"));

	auto *content = new QWidget(this);
	content->setObjectName(QStringLiteral("outputsContent"));
	auto *layout = new QVBoxLayout(content);
	layout->setContentsMargins(8, 8, 8, 8);
	layout->setSpacing(8);

	auto *header = new QFrame(content);
	header->setObjectName(QStringLiteral("outputsHeader"));
	auto *headerLayout = new QVBoxLayout(header);
	headerLayout->setContentsMargins(12, 10, 12, 10);
	headerLayout->setSpacing(3);
	auto *title = new QLabel(QTStr("Outputs.HeaderTitle"), header);
	title->setObjectName(QStringLiteral("outputsHeaderTitle"));
	summary = new QLabel(header);
	summary->setObjectName(QStringLiteral("outputsHint"));
	summary->setWordWrap(true);
	headerLayout->addWidget(title);
	headerLayout->addWidget(summary);
	layout->addWidget(header);

	auto *scroll = new QScrollArea(content);
	scroll->setObjectName(QStringLiteral("outputsScroll"));
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	auto *rowsHost = new QWidget(scroll);
	rowsHost->setObjectName(QStringLiteral("outputsRows"));
	rowsLayout = new QVBoxLayout(rowsHost);
	rowsLayout->setContentsMargins(0, 0, 0, 0);
	rowsLayout->setSpacing(6);
	emptyLabel = new QLabel(QTStr("Outputs.Empty"), rowsHost);
	emptyLabel->setObjectName(QStringLiteral("outputsHint"));
	emptyLabel->setWordWrap(true);
	emptyLabel->setAlignment(Qt::AlignCenter);
	rowsLayout->addWidget(emptyLabel);
	rowsLayout->addStretch();
	scroll->setWidget(rowsHost);
	layout->addWidget(scroll, 1);

	auto *addButton = new QPushButton(QTStr("Multistream.Vertical.AddTikTok"), content);
	addButton->setObjectName(QStringLiteral("outputsAddButton"));
	connect(addButton, &QPushButton::clicked, this, &OutputsDock::addChannelRequested);
	layout->addWidget(addButton);

	setWidget(content);
	RebuildSummary();
}

void OutputsDock::ApplyBadge(QLabel *badge, MultiStreamVideoLayout layout)
{
	const bool portrait = layout == MultiStreamVideoLayout::Portrait;
	badge->setText(portrait ? QStringLiteral("9:16") : QStringLiteral("16:9"));
	badge->setProperty("canvas", portrait ? QStringLiteral("portrait") : QStringLiteral("main"));
	Repolish(badge);
}

void OutputsDock::SetChannels(const std::vector<MultiStreamChannel> &newChannels, const std::string &primaryChannelId)
{
	channels = newChannels;
	for (auto &[id, row] : rows) {
		row.widget->deleteLater();
	}
	rows.clear();

	refreshing = true;
	int insertAt = 0;
	for (const auto &channel : channels) {
		Row row;
		auto *card = new QFrame();
		card->setObjectName(QStringLiteral("outputsRow"));
		card->setProperty("enabledChannel", channel.enabled);
		auto *cardLayout = new QVBoxLayout(card);
		cardLayout->setContentsMargins(10, 8, 10, 8);
		cardLayout->setSpacing(4);

		auto *top = new QHBoxLayout();
		top->setSpacing(6);
		row.badge = new QLabel(card);
		row.badge->setObjectName(QStringLiteral("canvasBadge"));
		ApplyBadge(row.badge, channel.videoLayout);
		QString name = QString::fromStdString(channel.displayName);
		if (channel.id == primaryChannelId) {
			name = QTStr("Outputs.PrimaryName").arg(name);
		}
		auto *nameLabel = new QLabel(
			QStringLiteral("%1 · %2").arg(name, StreamPlatformDisplayName(channel.platform)), card);
		nameLabel->setObjectName(QStringLiteral("outputsRowName"));
		nameLabel->setToolTip(nameLabel->text());
		row.status = new QLabel(
			channel.enabled ? StateText(MultiStreamChannelState::Idle) : QTStr("Outputs.Disabled"), card);
		row.status->setObjectName(QStringLiteral("outputsStatus"));
		row.status->setProperty("state", channel.enabled ? "idle" : "disabled");
		top->addWidget(row.badge);
		top->addWidget(nameLabel, 1);
		top->addWidget(row.status);
		cardLayout->addLayout(top);

		auto *bottom = new QHBoxLayout();
		bottom->setSpacing(6);
		row.canvas = new QComboBox(card);
		row.canvas->setObjectName(QStringLiteral("outputsCanvas"));
		row.canvas->setAccessibleName(QTStr("Outputs.CanvasFor").arg(name));
		row.canvas->addItem(CanvasName(MultiStreamVideoLayout::Main),
				    static_cast<int>(MultiStreamVideoLayout::Main));
		row.canvas->addItem(CanvasName(MultiStreamVideoLayout::Portrait),
				    static_cast<int>(MultiStreamVideoLayout::Portrait));
		row.canvas->setCurrentIndex(channel.videoLayout == MultiStreamVideoLayout::Portrait ? 1 : 0);
		row.canvas->setEnabled(!locked);
		row.layout = channel.videoLayout;
		row.encoder = new QLabel(card);
		row.encoder->setObjectName(QStringLiteral("outputsEncoder"));
		auto *edit = new QPushButton(QTStr("Outputs.Edit"), card);
		edit->setObjectName(QStringLiteral("outputsEditButton"));
		bottom->addWidget(row.canvas, 1);
		bottom->addWidget(edit);
		cardLayout->addLayout(bottom);

		const QString vodChannelId = QString::fromStdString(channel.id);
		if (GetStreamPlatformInfo(channel.platform).supportsVodTrack) {
			/* Twitch: a separate audio track for the saved VOD, so music
			 * marked "off the VOD" in the mixer is not archived. */
			auto *vodRow = new QHBoxLayout();
			vodRow->setSpacing(6);
			row.vodCheck = new QCheckBox(QTStr("Outputs.VodTrack"), card);
			row.vodCheck->setObjectName(QStringLiteral("outputsVodCheck"));
			row.vodCheck->setToolTip(QTStr("Outputs.VodTrack.Tooltip"));
			row.vodCheck->setChecked(channel.vodTrackEnabled);
			row.vodTrack = new QComboBox(card);
			row.vodTrack->setObjectName(QStringLiteral("outputsVodTrack"));
			for (int track = 1; track <= 6; track++)
				row.vodTrack->addItem(QTStr("Outputs.Track").arg(track), track - 1);
			row.vodTrack->setCurrentIndex(std::clamp(static_cast<int>(channel.vodTrackIndex), 0, 5));
			row.vodTrack->setEnabled(channel.vodTrackEnabled && !locked);
			row.vodCheck->setEnabled(!locked);
			vodRow->addWidget(row.vodCheck, 1);
			vodRow->addWidget(row.vodTrack);
			cardLayout->addLayout(vodRow);
			auto emitVod = [this, vodChannelId]() {
				if (refreshing)
					return;
				auto it = rows.find(vodChannelId.toStdString());
				if (it == rows.end() || !it->second.vodCheck)
					return;
				emit vodTrackChangeRequested(vodChannelId, it->second.vodCheck->isChecked(),
							     it->second.vodTrack->currentData().toInt());
			};
			connect(row.vodCheck, &QCheckBox::toggled, this, emitVod);
			connect(row.vodTrack, qOverload<int>(&QComboBox::currentIndexChanged), this, emitVod);
		}
		cardLayout->addWidget(row.encoder);

		const QString channelId = QString::fromStdString(channel.id);
		connect(row.canvas, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, channelId](int) {
			if (refreshing) {
				return;
			}
			auto it = rows.find(channelId.toStdString());
			if (it == rows.end()) {
				return;
			}
			const auto layout =
				static_cast<MultiStreamVideoLayout>(it->second.canvas->currentData().toInt());
			emit canvasChangeRequested(channelId, layout);
		});
		connect(edit, &QPushButton::clicked, this,
			[this, channelId]() { emit editChannelRequested(channelId); });

		rowsLayout->insertWidget(insertAt++, card);
		row.widget = card;
		rows.emplace(channel.id, row);
	}
	refreshing = false;
	emptyLabel->setVisible(channels.empty());
	RebuildSummary();
}

void OutputsDock::UpdateState(const MultiStreamChannelSnapshot &snapshot)
{
	auto it = rows.find(snapshot.id);
	if (it == rows.end()) {
		return;
	}
	auto channel = std::find_if(channels.begin(), channels.end(),
				    [&](const MultiStreamChannel &item) { return item.id == snapshot.id; });
	const bool enabled = channel == channels.end() || channel->enabled;
	Row &row = it->second;
	if (!enabled && snapshot.state == MultiStreamChannelState::Idle) {
		row.status->setText(QTStr("Outputs.Disabled"));
		row.status->setProperty("state", "disabled");
	} else {
		row.status->setText(StateText(snapshot.state));
		row.status->setProperty("state", StateValue(snapshot.state));
	}
	row.status->setToolTip(QString::fromStdString(snapshot.lastError));
	Repolish(row.status);
}

void OutputsDock::ApplySnapshots(const std::vector<MultiStreamChannelSnapshot> &snapshots)
{
	for (const auto &snapshot : snapshots) {
		UpdateState(snapshot);
	}
}

void OutputsDock::SetEncoderGroups(const std::vector<MultiStreamManager::EncoderGroup> &newGroups)
{
	groups = newGroups;
	RebuildSummary();
}

void OutputsDock::SetLocked(bool newLocked)
{
	locked = newLocked;
	for (auto &[id, row] : rows) {
		row.canvas->setEnabled(!locked);
		row.canvas->setToolTip(locked ? QTStr("Multistream.ChannelBar.BusyWhileLive") : QString());
		if (row.vodCheck) {
			row.vodCheck->setEnabled(!locked);
			row.vodTrack->setEnabled(!locked && row.vodCheck->isChecked());
		}
	}
}

void OutputsDock::RebuildSummary()
{
	/* Destinations on the same canvas share one video encoder: count them per
	 * canvas from the channel list so this is right before going live too. */
	int mainCount = 0;
	int portraitCount = 0;
	for (const auto &channel : channels) {
		if (!channel.enabled) {
			continue;
		}
		if (channel.videoLayout == MultiStreamVideoLayout::Portrait) {
			portraitCount++;
		} else {
			mainCount++;
		}
	}
	auto encoderIdFor = [this](MultiStreamVideoLayout layout) {
		for (const auto &group : groups) {
			if (group.layout == layout && !group.encoderId.empty()) {
				return QString::fromStdString(group.encoderId);
			}
		}
		return QString();
	};
	for (auto &[id, row] : rows) {
		const int shared = row.layout == MultiStreamVideoLayout::Portrait ? portraitCount : mainCount;
		const QString encoderId = encoderIdFor(row.layout);
		QString text = shared > 1 ? QTStr("Outputs.EncoderShared").arg(CanvasName(row.layout)).arg(shared)
					  : QTStr("Outputs.EncoderOwn").arg(CanvasName(row.layout));
		if (!encoderId.isEmpty()) {
			text += QStringLiteral(" · %1").arg(encoderId);
		}
		row.encoder->setText(text);
	}
	const int encoders = (mainCount > 0 ? 1 : 0) + (portraitCount > 0 ? 1 : 0);
	summary->setText(channels.empty() ? QTStr("Outputs.HeaderBody")
					  : QTStr("Outputs.Summary").arg(mainCount + portraitCount).arg(encoders));
}
