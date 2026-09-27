/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <docks/OBSDock.hpp>
#include <oauth/PlatformMetadataClient.hpp>
#include <utility/MultiStreamManager.hpp>

#include <QHash>
#include <QPointer>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QTimer;
class QVBoxLayout;

/* Title and category for every connected channel, editable before and during a
 * broadcast: the three platforms with an API accept changes while live.
 *
 * The title at the top applies to all of them; a channel can override it. The
 * category is per channel because each platform has its own catalog and ids. */
class StreamInfoDock : public OBSDock {
	Q_OBJECT

public:
	explicit StreamInfoDock(QWidget *parent = nullptr);
	~StreamInfoDock() override;

	/* Reloads the channel list from the store. */
	void RefreshChannels();
	void Shutdown();

protected:
	void showEvent(QShowEvent *event) override;

private:
	struct ChannelRow {
		MultiStreamChannel channel;
		QWidget *widget = nullptr;
		QLabel *nameLabel = nullptr;
		QLabel *titleStateLabel = nullptr;
		QLineEdit *titleEdit = nullptr;
		QPushButton *overrideButton = nullptr;
		QComboBox *categoryCombo = nullptr;
		QLabel *categoryStatusLabel = nullptr;
		QTimer *categorySearchTimer = nullptr;
		QLabel *resultLabel = nullptr;
		/* Guards against the combo's own repopulation looking like a pick. */
		bool populatingCategories = false;
		uint64_t categorySearchGeneration = 0;
	};

	void BuildRows();
	QWidget *BuildRow(int index);
	void ClearRows();
	ChannelRow *RowAt(int index);
	void SetOverrideEnabled(int index, bool enabled);
	void SearchCategories(int index, const QString &query);
	void ApplyAll();
	void ApplyRow(int index, const QString &sharedTitle);
	void ReportResult(int index, bool success, const QString &message);
	void SaveToStore();
	void UpdateApplyButton();
	void SetEditingEnabled(bool enabled);
	void SetFooterStatus(const QString &text, const char *state);
	void MarkEdited();

	QLineEdit *sharedTitleEdit = nullptr;
	QPushButton *applyButton = nullptr;
	QWidget *emptyState = nullptr;
	QScrollArea *channelsScroll = nullptr;
	QLabel *channelCountLabel = nullptr;
	QLabel *footerStatus = nullptr;
	QVBoxLayout *rowsLayout = nullptr;
	std::vector<std::unique_ptr<ChannelRow>> rows;
	int pendingApplies = 0;
	int successfulApplies = 0;
	int failedApplies = 0;
	uint64_t rowsGeneration = 0;
	std::atomic_bool shuttingDown{false};
};
