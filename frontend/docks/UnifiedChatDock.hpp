/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <docks/OBSDock.hpp>

#include <chat/MultiStreamChatAggregator.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>

class UnifiedChatDock : public OBSDock {
	Q_OBJECT

public:
	explicit UnifiedChatDock(QWidget *parent = nullptr);
	~UnifiedChatDock() override = default;

	MultiStreamChatAggregator *Aggregator() const { return aggregator; }
	void AutoConnectAccounts();
	void DisconnectAccounts();

protected:
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;

private slots:
	void OnChatMessage(const ChatMessage &msg);
	void OnStatusChanged(const QString &channelId, StreamPlatform platform, ChatConnectionState state,
			     const QString &detail);
	void OnSendClicked();
	void OnSendChannelChanged(int index);
	void OnFilterToggled();

private:
	void RebuildFilters(const std::vector<ChatChannelRef> &channels);
	void RefreshSendTargets(const std::vector<ChatChannelRef> &channels);
	void UpdateStatusSummary();
	bool ChannelFilterEnabled(const QString &channelId) const;
	void AppendHtml(const QString &html);
	void RegisterPlatformIcons();
	/* Role labels that ended up with artwork; the rest fall back to a pill. */
	QSet<QString> drawnRoleBadges;

	MultiStreamChatAggregator *aggregator = nullptr;

	QHBoxLayout *filtersLayout = nullptr;
	/* Keyed by channel id, not by platform: two accounts on one platform get
	 * a filter each, otherwise one of them would be invisible. */
	QHash<QString, QCheckBox *> channelFilters;
	QTextBrowser *chatView = nullptr;
	QCheckBox *autoScroll = nullptr;
	QLabel *statusLabel = nullptr;
	QPushButton *clearButton = nullptr;
	QComboBox *sendChannel = nullptr;
	QLineEdit *sendInput = nullptr;
	QPushButton *sendButton = nullptr;
	QLabel *sendHint = nullptr;

	/* Connection state per channel, for the summary line. */
	QHash<QString, ChatConnectionState> channelStates;
	QHash<QString, QString> channelDetails;
	QHash<QString, QString> channelNames;
	QHash<QString, StreamPlatform> channelPlatforms;
	bool connected = false;
};
