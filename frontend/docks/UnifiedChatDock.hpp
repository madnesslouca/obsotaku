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
#include <QHBoxLayout>
#include <QLabel>
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
	void OnStatusChanged(StreamPlatform platform, ChatConnectionState state, const QString &detail);

private:
	MultiStreamChatAggregator *aggregator = nullptr;

	QTextBrowser *chatView = nullptr;
	QCheckBox *showTwitch = nullptr;
	QCheckBox *showYouTube = nullptr;
	QCheckBox *showKick = nullptr;
	QCheckBox *autoScroll = nullptr;
	QLabel *statusLabel = nullptr;
	QPushButton *clearButton = nullptr;
	bool connected = false;
};
