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

#include <QString>

#include <map>
#include <vector>

class QComboBox;
class QLabel;
class QVBoxLayout;

/* One place that answers "what is going where": every multistream
 * destination, the canvas it sends (Main 16:9 or Vertical 9:16), its live
 * status and the encoder it shares with the other destinations of the same
 * canvas. */
class OutputsDock : public OBSDock {
	Q_OBJECT

public:
	explicit OutputsDock(QWidget *parent = nullptr);

	void SetChannels(const std::vector<MultiStreamChannel> &channels, const std::string &primaryChannelId);
	void UpdateState(const MultiStreamChannelSnapshot &snapshot);
	void ApplySnapshots(const std::vector<MultiStreamChannelSnapshot> &snapshots);
	void SetEncoderGroups(const std::vector<MultiStreamManager::EncoderGroup> &groups);
	/* While live the canvas of a destination cannot change. */
	void SetLocked(bool locked);

signals:
	void canvasChangeRequested(const QString &channelId, MultiStreamVideoLayout layout);
	/* trackIndex is zero-based (track 2 in the UI = 1). */
	void vodTrackChangeRequested(const QString &channelId, bool enabled, int trackIndex);
	void addChannelRequested();
	void editChannelRequested(const QString &channelId);

private:
	struct Row {
		QWidget *widget = nullptr;
		QLabel *status = nullptr;
		QComboBox *canvas = nullptr;
		class QCheckBox *vodCheck = nullptr;
		QComboBox *vodTrack = nullptr;
		QLabel *badge = nullptr;
		QLabel *encoder = nullptr;
		MultiStreamVideoLayout layout = MultiStreamVideoLayout::Main;
	};

	void RebuildSummary();
	static void ApplyBadge(QLabel *badge, MultiStreamVideoLayout layout);

	QVBoxLayout *rowsLayout = nullptr;
	QLabel *summary = nullptr;
	QLabel *emptyLabel = nullptr;
	std::map<std::string, Row> rows;
	std::vector<MultiStreamChannel> channels;
	std::vector<MultiStreamManager::EncoderGroup> groups;
	bool locked = false;
	bool refreshing = false;
};
