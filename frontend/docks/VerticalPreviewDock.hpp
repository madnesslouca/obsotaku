/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <docks/OBSDock.hpp>
#include <utility/MultiStreamManager.hpp>

#include <atomic>
#include <vector>

class QComboBox;
class QLabel;
class QPushButton;
class QStackedWidget;
class OBSQTDisplay;

/* A phone-shaped monitor for the independent portrait output. It renders the
 * current program even before streaming starts, so framing can be checked
 * without sending video to a platform. */
class VerticalPreviewDock : public OBSDock {
	Q_OBJECT

public:
	explicit VerticalPreviewDock(QWidget *parent = nullptr);
	~VerticalPreviewDock() override;

	void SetChannels(const std::vector<MultiStreamChannel> &channels);
	void Shutdown();

signals:
	void addChannelRequested();
	void editChannelRequested(const QString &channelId);
	void portraitFitChanged(const QString &channelId, MultiStreamPortraitFit fit);

private:
	static void RenderPreview(void *data, uint32_t width, uint32_t height);
	void OnDisplayCreated();
	void OnChannelChanged(int index);
	void OnFramingChanged(int index);
	void UpdateControls();

	OBSQTDisplay *preview = nullptr;
	QStackedWidget *previewStack = nullptr;
	QComboBox *channelCombo = nullptr;
	QComboBox *framingCombo = nullptr;
	QLabel *statusLabel = nullptr;
	QPushButton *editButton = nullptr;
	std::vector<MultiStreamChannel> portraitChannels;
	std::atomic<int> activeFit{static_cast<int>(MultiStreamPortraitFit::Fill)};
	std::atomic_bool shuttingDown{false};
	bool refreshing = false;
	bool drawCallbackRegistered = false;
};
