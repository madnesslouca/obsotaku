/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "StreamInfoDock.hpp"

#include <dialogs/MultistreamAccountsDialog.hpp>
#include <oauth/ConnectedAccountManager.hpp>
#include <utility/MultistreamChannelStore.hpp>
#include <utility/MultistreamTaskPool.hpp>
#include <utility/PlatformIconProvider.hpp>
#include <utility/StreamPlatformDisplay.hpp>

#include <OBSApp.hpp>
#include <qt-wrappers.hpp>

#include <util/config-file.h>

#include <QComboBox>
#include <QDesktopServices>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

#include "moc_StreamInfoDock.cpp"

namespace {
constexpr const char *SHARED_TITLE_SECTION = "MultistreamChannels";
constexpr const char *SHARED_TITLE_KEY = "SharedTitle";
/* Long enough that typing a game name does not fire a request per keystroke. */
constexpr int CATEGORY_SEARCH_DELAY_MS = 500;

void RefreshStyle(QWidget *widget)
{
	if (!widget || !widget->style())
		return;
	widget->style()->unpolish(widget);
	widget->style()->polish(widget);
}

QString FromStd(const std::string &value)
{
	return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

/* Refreshes the token when needed so an update during a long broadcast does not
 * fail on an expired access token. Runs on the task pool. */
bool LoadUsableTokens(const MultiStreamChannel &channel, const OAuthClientRegistration &registration,
		      OAuthTokenSet &tokens, std::string &error)
{
	return ConnectedAccountManager::LoadUsableTokens(channel.platform, channel.accountId, registration, {}, tokens,
							 error);
}
} // namespace

StreamInfoDock::StreamInfoDock(QWidget *parent) : OBSDock(parent)
{
	setObjectName(QStringLiteral("streamInfoDock"));
	setWindowTitle(QTStr("Multistream.Info.Title"));

	auto *content = new QWidget(this);
	content->setObjectName(QStringLiteral("streamInfoContent"));

	auto *layout = new QVBoxLayout(content);
	layout->setContentsMargins(8, 8, 8, 8);
	layout->setSpacing(8);

	auto *header = new QFrame(content);
	header->setObjectName(QStringLiteral("streamInfoHeader"));
	auto *headerLayout = new QVBoxLayout(header);
	headerLayout->setContentsMargins(12, 10, 12, 10);
	headerLayout->setSpacing(3);
	auto *headerTitle = new QLabel(QTStr("Multistream.Info.HeaderTitle"), header);
	headerTitle->setObjectName(QStringLiteral("streamInfoHeaderTitle"));
	auto *headerBody = new QLabel(QTStr("Multistream.Info.HeaderBody"), header);
	headerBody->setObjectName(QStringLiteral("streamInfoHint"));
	headerBody->setWordWrap(true);
	headerLayout->addWidget(headerTitle);
	headerLayout->addWidget(headerBody);
	layout->addWidget(header);

	auto *sharedCard = new QFrame(content);
	sharedCard->setObjectName(QStringLiteral("streamInfoSharedCard"));
	auto *sharedLayout = new QVBoxLayout(sharedCard);
	sharedLayout->setContentsMargins(12, 9, 12, 10);
	sharedLayout->setSpacing(6);
	auto *sharedHeader = new QHBoxLayout();
	sharedHeader->setContentsMargins(0, 0, 0, 0);
	auto *sharedLabel = new QLabel(QTStr("Multistream.Info.SharedTitle"), sharedCard);
	sharedLabel->setObjectName(QStringLiteral("streamInfoSectionTitle"));
	auto *sharedBadge = new QLabel(QTStr("Multistream.Info.SharedBadge"), sharedCard);
	sharedBadge->setObjectName(QStringLiteral("streamInfoSharedBadge"));
	sharedHeader->addWidget(sharedLabel);
	sharedHeader->addStretch();
	sharedHeader->addWidget(sharedBadge);
	sharedLayout->addLayout(sharedHeader);

	sharedTitleEdit = new QLineEdit(sharedCard);
	sharedTitleEdit->setObjectName(QStringLiteral("streamInfoSharedTitle"));
	sharedTitleEdit->setPlaceholderText(QTStr("Multistream.Info.TitlePlaceholder"));
	const char *storedTitle = config_get_string(App()->GetUserConfig(), SHARED_TITLE_SECTION, SHARED_TITLE_KEY);
	sharedTitleEdit->setText(QString::fromUtf8(storedTitle ? storedTitle : ""));
	sharedLayout->addWidget(sharedTitleEdit);

	auto *hint = new QLabel(QTStr("Multistream.Info.SharedHint"), sharedCard);
	hint->setObjectName(QStringLiteral("streamInfoHint"));
	hint->setWordWrap(true);
	sharedLayout->addWidget(hint);
	layout->addWidget(sharedCard);

	auto *channelsHeader = new QHBoxLayout();
	channelsHeader->setContentsMargins(2, 1, 2, 0);
	auto *channelsTitle = new QLabel(QTStr("Multistream.Info.Channels"), content);
	channelsTitle->setObjectName(QStringLiteral("streamInfoSectionTitle"));
	channelCountLabel = new QLabel(content);
	channelCountLabel->setObjectName(QStringLiteral("streamInfoCount"));
	channelsHeader->addWidget(channelsTitle);
	channelsHeader->addStretch();
	channelsHeader->addWidget(channelCountLabel);
	layout->addLayout(channelsHeader);

	/* Channels scroll so the dock stays usable docked to a narrow side. */
	channelsScroll = new QScrollArea(content);
	channelsScroll->setObjectName(QStringLiteral("streamInfoScroll"));
	channelsScroll->setWidgetResizable(true);
	channelsScroll->setFrameShape(QFrame::NoFrame);
	channelsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

	auto *rowsContainer = new QWidget(channelsScroll);
	rowsContainer->setObjectName(QStringLiteral("streamInfoRows"));
	rowsLayout = new QVBoxLayout(rowsContainer);
	rowsLayout->setContentsMargins(0, 0, 0, 0);
	rowsLayout->setSpacing(8);
	rowsLayout->addStretch(1);
	channelsScroll->setWidget(rowsContainer);
	layout->addWidget(channelsScroll, 1);

	emptyState = new QFrame(content);
	emptyState->setObjectName(QStringLiteral("streamInfoEmpty"));
	auto *emptyLayout = new QVBoxLayout(emptyState);
	emptyLayout->setContentsMargins(24, 24, 24, 24);
	emptyLayout->setSpacing(6);
	auto *emptyIcon = new QLabel(QStringLiteral("•••"), emptyState);
	emptyIcon->setObjectName(QStringLiteral("streamInfoEmptyIcon"));
	emptyIcon->setAlignment(Qt::AlignCenter);
	auto *emptyTitle = new QLabel(QTStr("Multistream.Info.EmptyTitle"), emptyState);
	emptyTitle->setObjectName(QStringLiteral("streamInfoEmptyTitle"));
	emptyTitle->setAlignment(Qt::AlignCenter);
	auto *emptyBody = new QLabel(QTStr("Multistream.Info.NoChannels"), emptyState);
	emptyBody->setObjectName(QStringLiteral("streamInfoHint"));
	emptyBody->setAlignment(Qt::AlignCenter);
	emptyBody->setWordWrap(true);
	emptyLayout->addWidget(emptyIcon);
	emptyLayout->addWidget(emptyTitle);
	emptyLayout->addWidget(emptyBody);
	layout->addWidget(emptyState, 1);

	auto *footer = new QFrame(content);
	footer->setObjectName(QStringLiteral("streamInfoFooter"));
	auto *footerLayout = new QHBoxLayout(footer);
	footerLayout->setContentsMargins(10, 8, 10, 8);
	footerLayout->setSpacing(10);
	footerStatus = new QLabel(footer);
	footerStatus->setObjectName(QStringLiteral("streamInfoFooterStatus"));
	footerStatus->setWordWrap(true);
	applyButton = new QPushButton(QTStr("Multistream.Info.Apply"), footer);
	applyButton->setObjectName(QStringLiteral("streamInfoApply"));
	connect(applyButton, &QPushButton::clicked, this, &StreamInfoDock::ApplyAll);
	footerLayout->addWidget(footerStatus, 1);
	footerLayout->addWidget(applyButton);
	layout->addWidget(footer);
	connect(sharedTitleEdit, &QLineEdit::textEdited, this, &StreamInfoDock::MarkEdited);

	setWidget(content);
	RefreshChannels();
}

void StreamInfoDock::showEvent(QShowEvent *event)
{
	OBSDock::showEvent(event);
	RefreshChannels();
}

StreamInfoDock::ChannelRow *StreamInfoDock::RowAt(int index)
{
	if (index < 0 || index >= static_cast<int>(rows.size()))
		return nullptr;
	return rows[static_cast<size_t>(index)].get();
}

void StreamInfoDock::ClearRows()
{
	++rowsGeneration;
	for (auto &row : rows) {
		if (row->widget) {
			rowsLayout->removeWidget(row->widget);
			row->widget->deleteLater();
		}
	}
	rows.clear();
}

void StreamInfoDock::RefreshChannels()
{
	if (pendingApplies > 0)
		return;

	ClearRows();
	for (auto &channel : MultistreamChannelStore::Load()) {
		if (!GetStreamPlatformInfo(channel.platform).supportsMetadataUpdates)
			continue;
		auto row = std::make_unique<ChannelRow>();
		row->channel = std::move(channel);
		rows.push_back(std::move(row));
	}

	BuildRows();
	const bool hasRows = !rows.empty();
	channelsScroll->setVisible(hasRows);
	emptyState->setVisible(!hasRows);
	channelCountLabel->setText(QString::number(rows.size()));
	SetFooterStatus(hasRows ? QTStr("Multistream.Info.ReadySummary").arg(rows.size())
				    : QTStr("Multistream.Info.NoChannelsShort"),
			hasRows ? "ready" : "idle");
	UpdateApplyButton();
}

void StreamInfoDock::BuildRows()
{
	for (int index = 0; index < static_cast<int>(rows.size()); ++index) {
		if (QWidget *widget = BuildRow(index))
			rowsLayout->insertWidget(index, widget);
	}
}

QWidget *StreamInfoDock::BuildRow(int index)
{
	ChannelRow *rowPtr = RowAt(index);
	if (!rowPtr)
		return nullptr;
	ChannelRow &row = *rowPtr;
	const StreamPlatform platform = row.channel.platform;

	auto *widget = new QFrame(this);
	widget->setObjectName(QStringLiteral("streamInfoRow"));
	widget->setProperty("platform", StreamPlatformId(platform));
	row.widget = widget;

	auto *outer = new QVBoxLayout(widget);
	outer->setContentsMargins(10, 8, 10, 8);
	outer->setSpacing(6);

	auto *header = new QHBoxLayout();
	header->setContentsMargins(0, 0, 0, 0);
	header->setSpacing(8);

	auto *icon = new QLabel(widget);
	icon->setFixedSize(20, 20);
	icon->setPixmap(PlatformIconProvider::Badge(platform, 20));
	header->addWidget(icon);

	row.nameLabel = new QLabel(FromStd(row.channel.displayName), widget);
	row.nameLabel->setObjectName(QStringLiteral("streamInfoName"));
	header->addWidget(row.nameLabel);
	header->addStretch(1);
	auto *platformLabel = new QLabel(StreamPlatformDisplayName(platform), widget);
	platformLabel->setObjectName(QStringLiteral("streamInfoPlatform"));
	header->addWidget(platformLabel);
	outer->addLayout(header);

	auto *titleModeRow = new QHBoxLayout();
	titleModeRow->setContentsMargins(0, 0, 0, 0);
	titleModeRow->setSpacing(8);
	row.titleStateLabel = new QLabel(widget);
	row.titleStateLabel->setObjectName(QStringLiteral("streamInfoState"));
	titleModeRow->addWidget(row.titleStateLabel);
	titleModeRow->addStretch(1);

	row.overrideButton = new QPushButton(widget);
	row.overrideButton->setObjectName(QStringLiteral("streamInfoOverride"));
	connect(row.overrideButton, &QPushButton::clicked, this, [this, index]() {
		ChannelRow *target = RowAt(index);
		if (target) {
			SetOverrideEnabled(index, !target->titleEdit->isVisible());
			MarkEdited();
		}
	});
	titleModeRow->addWidget(row.overrideButton);
	outer->addLayout(titleModeRow);

	row.titleEdit = new QLineEdit(FromStd(row.channel.title), widget);
	row.titleEdit->setObjectName(QStringLiteral("streamInfoTitle"));
	row.titleEdit->setPlaceholderText(QTStr("Multistream.Info.TitlePlaceholder"));
	row.titleEdit->setToolTip(QTStr("Multistream.Info.OwnTitleTip"));
	connect(row.titleEdit, &QLineEdit::textEdited, this, &StreamInfoDock::MarkEdited);
	outer->addWidget(row.titleEdit);

	if (PlatformMetadataClient::SupportsCategories(platform)) {
		auto *categoryLabel = new QLabel(QTStr("Multistream.Info.Category"), widget);
		categoryLabel->setObjectName(QStringLiteral("streamInfoLabel"));
		outer->addWidget(categoryLabel);

		row.categoryCombo = new QComboBox(widget);
		row.categoryCombo->setObjectName(QStringLiteral("streamInfoCategory"));
		row.categoryCombo->setEditable(true);
		row.categoryCombo->setInsertPolicy(QComboBox::NoInsert);
		row.categoryCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		row.categoryCombo->setToolTip(QTStr("Multistream.Info.CategoryTip"));
		row.categoryCombo->lineEdit()->setPlaceholderText(QTStr("Multistream.Info.CategoryPlaceholder"));
		if (!row.channel.categoryName.empty()) {
			row.categoryCombo->addItem(FromStd(row.channel.categoryName),
						   FromStd(row.channel.categoryId));
			row.categoryCombo->setCurrentIndex(0);
		}
		outer->addWidget(row.categoryCombo);

		row.categoryStatusLabel = new QLabel(widget);
		row.categoryStatusLabel->setObjectName(QStringLiteral("streamInfoCategoryStatus"));
		row.categoryStatusLabel->setVisible(false);
		outer->addWidget(row.categoryStatusLabel);

		row.categorySearchTimer = new QTimer(widget);
		row.categorySearchTimer->setSingleShot(true);
		row.categorySearchTimer->setInterval(CATEGORY_SEARCH_DELAY_MS);
		connect(row.categorySearchTimer, &QTimer::timeout, this, [this, index]() {
			ChannelRow *target = RowAt(index);
			if (target)
				SearchCategories(index, target->categoryCombo->currentText());
		});

		connect(row.categoryCombo, &QComboBox::editTextChanged, this, [this, index](const QString &text) {
			ChannelRow *target = RowAt(index);
			if (target && !target->populatingCategories) {
				const int selected = target->categoryCombo->currentIndex();
				if (selected < 0 || target->categoryCombo->itemText(selected) != text) {
					target->channel.categoryId.clear();
					target->channel.categoryName.clear();
				}
				target->categoryStatusLabel->setVisible(false);
				target->categorySearchTimer->start();
				MarkEdited();
			}
		});
		connect(row.categoryCombo, &QComboBox::activated, this, [this, index](int comboIndex) {
			ChannelRow *target = RowAt(index);
			if (!target || comboIndex < 0)
				return;
			target->channel.categoryId = target->categoryCombo->itemData(comboIndex).toString().toStdString();
			target->channel.categoryName = target->categoryCombo->itemText(comboIndex).toStdString();
			target->categoryStatusLabel->setVisible(false);
			MarkEdited();
		});
	}

	row.resultLabel = new QLabel(widget);
	row.resultLabel->setObjectName(QStringLiteral("streamInfoResult"));
	row.resultLabel->setWordWrap(true);
	row.resultLabel->setVisible(false);
	outer->addWidget(row.resultLabel);

	SetOverrideEnabled(index, !row.channel.title.empty());
	return widget;
}

void StreamInfoDock::SetOverrideEnabled(int index, bool enabled)
{
	ChannelRow *row = RowAt(index);
	if (!row)
		return;

	row->titleEdit->setVisible(enabled);
	row->titleStateLabel->setText(enabled ? QTStr("Multistream.Info.OwnTitle")
					      : QTStr("Multistream.Info.SharedTitleUsed"));
	row->titleStateLabel->setProperty("titleState", enabled ? "own" : "shared");
	row->overrideButton->setText(enabled ? QTStr("Multistream.Info.UseShared")
					     : QTStr("Multistream.Info.UseOwn"));
	if (!enabled)
		row->titleEdit->clear();
	RefreshStyle(row->titleStateLabel);
	RefreshStyle(row->overrideButton);
}

void StreamInfoDock::SearchCategories(int index, const QString &query)
{
	ChannelRow *row = RowAt(index);
	if (!row || !row->categoryCombo)
		return;
	if (query.trimmed().size() < 2) {
		row->categoryStatusLabel->setVisible(false);
		return;
	}

	row->categoryStatusLabel->setText(QTStr("Multistream.Info.CategorySearching"));
	row->categoryStatusLabel->setProperty("resultState", "pending");
	row->categoryStatusLabel->setVisible(true);
	RefreshStyle(row->categoryStatusLabel);

	const MultiStreamChannel channel = row->channel;
	const auto registration = MultistreamAccountsDialog::RegistrationForPlatform(channel.platform);
	const uint64_t rowSet = rowsGeneration;
	const uint64_t searchGeneration = ++row->categorySearchGeneration;
	const QString requestedText = query.trimmed();
	QPointer<StreamInfoDock> guard(this);

	MultistreamTaskPool().start([guard, index, channel, registration, rowSet, searchGeneration, requestedText,
				     text = requestedText.toStdString()]() {
		OAuthTokenSet tokens;
		std::string error;
		std::vector<StreamCategory> results;
		bool success = LoadUsableTokens(channel, registration, tokens, error);
		if (success)
			success = PlatformMetadataClient::SearchCategories(channel.platform, registration, tokens, text,
									    results, error);
		const QString message = QString::fromUtf8(error.data(), static_cast<qsizetype>(error.size()));
		if (!guard)
			return;
		QMetaObject::invokeMethod(
			guard.data(),
			[guard, index, channelId = channel.id, rowSet, searchGeneration, requestedText,
			 success, message, results = std::move(results)]() {
				if (!guard)
					return;
				ChannelRow *target = guard->RowAt(index);
				if (!target || !target->categoryCombo || guard->rowsGeneration != rowSet ||
				    target->channel.id != channelId ||
				    target->categorySearchGeneration != searchGeneration ||
				    target->categoryCombo->currentText().trimmed() != requestedText) {
					return;
				}
				if (!success) {
					target->categoryStatusLabel->setText(
						message.isEmpty() ? QTStr("Multistream.Info.CategorySearchFailed")
								  : QTStr("Multistream.Info.CategorySearchFailedWith").arg(message));
					target->categoryStatusLabel->setProperty("resultState", "error");
					target->categoryStatusLabel->setVisible(true);
					RefreshStyle(target->categoryStatusLabel);
					return;
				}
				if (results.empty()) {
					target->categoryStatusLabel->setText(QTStr("Multistream.Info.CategoryNoResults"));
					target->categoryStatusLabel->setProperty("resultState", "idle");
					target->categoryStatusLabel->setVisible(true);
					RefreshStyle(target->categoryStatusLabel);
					return;
				}

				/* Keep what the user typed; only the list changes. */
				const QString typed = target->categoryCombo->currentText();
				target->populatingCategories = true;
				target->categoryCombo->clear();
				for (const auto &category : results)
					target->categoryCombo->addItem(FromStd(category.name), FromStd(category.id));
				target->categoryCombo->setEditText(typed);
				target->populatingCategories = false;
				target->categoryStatusLabel->setVisible(false);
				if (target->categoryCombo->hasFocus())
					target->categoryCombo->showPopup();
			},
			Qt::QueuedConnection);
	});
}

void StreamInfoDock::UpdateApplyButton()
{
	applyButton->setEnabled(!rows.empty() && pendingApplies == 0);
	applyButton->setText(pendingApplies > 0 ? QTStr("Multistream.Info.Applying")
						: QTStr("Multistream.Info.Apply"));
}

void StreamInfoDock::SetEditingEnabled(bool enabled)
{
	sharedTitleEdit->setEnabled(enabled);
	for (const auto &row : rows) {
		if (!enabled && row->categorySearchTimer) {
			row->categorySearchTimer->stop();
			++row->categorySearchGeneration;
		}
		row->overrideButton->setEnabled(enabled);
		row->titleEdit->setEnabled(enabled);
		if (row->categoryCombo)
			row->categoryCombo->setEnabled(enabled);
	}
}

void StreamInfoDock::SetFooterStatus(const QString &text, const char *state)
{
	footerStatus->setText(text);
	footerStatus->setProperty("resultState", state);
	RefreshStyle(footerStatus);
}

void StreamInfoDock::MarkEdited()
{
	if (pendingApplies == 0 && !rows.empty())
		SetFooterStatus(QTStr("Multistream.Info.ReadySummary").arg(rows.size()), "ready");
}

void StreamInfoDock::SaveToStore()
{
	auto stored = MultistreamChannelStore::Load();
	for (const auto &row : rows) {
		auto match = std::find_if(stored.begin(), stored.end(), [&](const MultiStreamChannel &channel) {
			return channel.id == row->channel.id;
		});
		if (match == stored.end())
			continue;
		match->title = row->titleEdit->isVisible() ? row->titleEdit->text().trimmed().toStdString()
							   : std::string{};
		match->categoryId = row->channel.categoryId;
		match->categoryName = row->channel.categoryName;
	}

	std::string error;
	if (!MultistreamChannelStore::Save(stored, error))
		blog(LOG_WARNING, "Could not store the broadcast metadata: %s", error.c_str());
}

void StreamInfoDock::ApplyAll()
{
	if (rows.empty() || pendingApplies > 0)
		return;
	for (const auto &row : rows) {
		if (!row->categoryCombo || row->categoryCombo->currentText().trimmed().isEmpty() ||
		    !row->channel.categoryId.empty()) {
			continue;
		}
		row->categoryStatusLabel->setText(QTStr("Multistream.Info.CategorySelectionRequired"));
		row->categoryStatusLabel->setProperty("resultState", "error");
		row->categoryStatusLabel->setVisible(true);
		RefreshStyle(row->categoryStatusLabel);
		SetFooterStatus(QTStr("Multistream.Info.CheckFields"), "error");
		channelsScroll->ensureWidgetVisible(row->widget);
		row->categoryCombo->setFocus();
		return;
	}

	const QString sharedTitle = sharedTitleEdit->text().trimmed();
	config_set_string(App()->GetUserConfig(), SHARED_TITLE_SECTION, SHARED_TITLE_KEY,
			  sharedTitle.toUtf8().constData());
	config_save_safe(App()->GetUserConfig(), "tmp", nullptr);
	SaveToStore();

	pendingApplies = static_cast<int>(rows.size());
	successfulApplies = 0;
	failedApplies = 0;
	SetEditingEnabled(false);
	SetFooterStatus(QTStr("Multistream.Info.ApplyingSummary").arg(pendingApplies), "pending");
	UpdateApplyButton();
	for (int index = 0; index < static_cast<int>(rows.size()); ++index)
		ApplyRow(index, sharedTitle);
}

void StreamInfoDock::ApplyRow(int index, const QString &sharedTitle)
{
	ChannelRow *row = RowAt(index);
	if (!row) {
		ReportResult(index, false, QTStr("Multistream.Info.Failed"));
		return;
	}

	row->resultLabel->setVisible(true);
	row->resultLabel->setText(QTStr("Multistream.Info.Sending"));
	row->resultLabel->setProperty("resultState", "pending");
	RefreshStyle(row->resultLabel);

	StreamMetadata metadata;
	metadata.title = (row->titleEdit->isVisible() ? row->titleEdit->text().trimmed() : sharedTitle).toStdString();
	metadata.categoryId = row->channel.categoryId;
	metadata.categoryName = row->channel.categoryName;

	const MultiStreamChannel channel = row->channel;
	const auto registration = MultistreamAccountsDialog::RegistrationForPlatform(channel.platform);
	QPointer<StreamInfoDock> guard(this);

	MultistreamTaskPool().start([guard, index, channel, registration, metadata]() {
		OAuthTokenSet tokens;
		std::string error;
		bool success = LoadUsableTokens(channel, registration, tokens, error);
		if (success) {
			success = PlatformMetadataClient::Update(channel.platform, registration, tokens,
								 channel.accountId, metadata, error);
		}
		const QString message = QString::fromUtf8(error.data(), static_cast<qsizetype>(error.size()));
		if (!guard)
			return;
		QMetaObject::invokeMethod(
			guard.data(),
			[guard, index, success, message]() {
				if (guard)
					guard->ReportResult(index, success, message);
			},
			Qt::QueuedConnection);
	});
}

void StreamInfoDock::ReportResult(int index, bool success, const QString &message)
{
	if (ChannelRow *row = RowAt(index)) {
		row->resultLabel->setVisible(true);
		row->resultLabel->setText(success ? QTStr("Multistream.Info.Updated")
						  : message.isEmpty() ? QTStr("Multistream.Info.Failed")
								      : QTStr("Multistream.Info.FailedWith").arg(message));
		row->resultLabel->setProperty("resultState", success ? "ok" : "error");
		RefreshStyle(row->resultLabel);
	}
	if (success)
		++successfulApplies;
	else
		++failedApplies;

	if (pendingApplies > 0)
		--pendingApplies;
	if (pendingApplies == 0) {
		SetEditingEnabled(true);
		if (failedApplies == 0) {
			SetFooterStatus(QTStr("Multistream.Info.AllUpdated").arg(successfulApplies), "ok");
		} else {
			SetFooterStatus(QTStr("Multistream.Info.UpdateSummary")
					.arg(successfulApplies)
					.arg(failedApplies),
					"error");
		}
	}
	UpdateApplyButton();
}
