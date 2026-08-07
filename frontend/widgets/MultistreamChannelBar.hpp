/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <utility/MultiStreamManager.hpp>
#include <utility/MultistreamPreflight.hpp>

#include <QFrame>
#include <QHash>
#include <QPointer>

class QCheckBox;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QScrollArea;
class QTimer;
class QToolButton;

/* Persistent strip above the preview: one card per destination, with the
 * platform accent, the account name, the live state and a per-destination
 * switch that also works while streaming. */
class MultistreamChannelBar : public QFrame {
	Q_OBJECT

public:
	explicit MultistreamChannelBar(QWidget *parent = nullptr);
	void SetChannels(const std::vector<MultiStreamChannel> &channels);
	void ApplySnapshots(const std::vector<MultiStreamChannelSnapshot> &snapshots);
	void ShowRestoringAccounts();
	void ShowRestoreFailure(const QString &details);
	void UpdateState(const MultiStreamChannelSnapshot &snapshot);
	void ShowPreflightFindings(const std::vector<PreflightFinding> &findings);

signals:
	void channelEnabledChanged(const QString &channelId, bool enabled);
	void addChannelRequested();
	/* Carries the channel so the account dialog can open on that one alone. */
	void manageAccountRequested(const QString &channelId);
	void editChannelRequested(const QString &channelId);
	void removeChannelRequested(const QString &channelId);
	void reconnectChannelRequested(const QString &channelId);
	/* Emitted every second while at least one destination is live, so the
	 * owner can push a fresh snapshot without the bar reaching into it. */
	void healthRefreshRequested();

private:
	struct ChannelWidgets {
		QLabel *avatar = nullptr;
		QLabel *state = nullptr;
		QLabel *health = nullptr;
		QCheckBox *enabled = nullptr;
		QFrame *card = nullptr;
		StreamPlatform platform = StreamPlatform::CustomRtmp;
		QString displayName;
		QString avatarUrl;
	};

	void ShowPlaceholder(const QString &text, const QString &toolTip = {});
	void ClearChannels();
	QWidget *CreateChannelCard(const MultiStreamChannel &channel);
	void SetCollapsed(bool collapsed);
	void UpdateSummary();
	void ApplyAvatar(const QString &channelId);

	QHBoxLayout *channelsLayout = nullptr;
	QScrollArea *channelsArea = nullptr;
	QWidget *channelsContainer = nullptr;
	QLabel *summaryTitle = nullptr;
	QLabel *summarySubtitle = nullptr;
	QLabel *warningIcon = nullptr;
	QLabel *warningText = nullptr;
	QWidget *warningRow = nullptr;
	QPointer<QLabel> emptyState;
	QToolButton *collapseButton = nullptr;
	QPushButton *addButton = nullptr;
	QTimer *healthTimer = nullptr;
	QHash<QString, ChannelWidgets> channelWidgets;
	QHash<QString, MultiStreamChannelState> channelStates;
	int totalChannels = 0;
	bool collapsed = false;
};
