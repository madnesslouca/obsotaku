/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "StreamInfoDock.hpp"

#include <dialogs/MultistreamAccountsDialog.hpp>
#include <oauth/OAuthTokenSet.hpp>
#include <oauth/PlatformOAuthClient.hpp>
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

QString FromStd(const std::string &value)
{
	return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

/* Refreshes the token when needed so an update during a long broadcast does not
 * fail on an expired access token. Runs on the task pool. */
bool LoadUsableTokens(const MultiStreamChannel &channel, const OAuthClientRegistration &registration,
		      OAuthTokenSet &tokens, std::string &error)
{
	auto stored = OAuthTokenSet::Load(channel.platform, channel.accountId, error);
	if (!stored) {
		if (error.empty())
			error = "No stored credential for this account.";
		return false;
	}

	if (stored->AccessTokenExpired()) {
		OAuthTokenSet refreshed;
		if (!PlatformOAuthClient::RefreshTokens(channel.platform, registration, {}, *stored, refreshed, error))
			return false;
		refreshed.Save(channel.platform, channel.accountId, error);
		*stored = std::move(refreshed);
	}

	tokens = std::move(*stored);
	return true;
}
} // namespace

StreamInfoDock::StreamInfoDock(QWidget *parent) : OBSDock(parent)
{
	setObjectName(QStringLiteral("streamInfoDock"));
	setWindowTitle(QTStr("Multistream.Info.Title"));

	auto *content = new QWidget(this);
	content->setObjectName(QStringLiteral("streamInfoContent"));

	auto *layout = new QVBoxLayout(content);
	layout->setContentsMargins(10, 10, 10, 10);
	layout->setSpacing(8);

	auto *sharedLabel = new QLabel(QTStr("Multistream.Info.SharedTitle"), content);
	sharedLabel->setObjectName(QStringLiteral("streamInfoLabel"));
	layout->addWidget(sharedLabel);

	sharedTitleEdit = new QLineEdit(content);
	sharedTitleEdit->setObjectName(QStringLiteral("streamInfoSharedTitle"));
	sharedTitleEdit->setPlaceholderText(QTStr("Multistream.Info.TitlePlaceholder"));
	const char *storedTitle = config_get_string(App()->GetUserConfig(), SHARED_TITLE_SECTION, SHARED_TITLE_KEY);
	sharedTitleEdit->setText(QString::fromUtf8(storedTitle ? storedTitle : ""));
	layout->addWidget(sharedTitleEdit);

	auto *hint = new QLabel(QTStr("Multistream.Info.SharedHint"), content);
	hint->setObjectName(QStringLiteral("streamInfoHint"));
	hint->setWordWrap(true);
	layout->addWidget(hint);

	/* Channels scroll so the dock stays usable docked to a narrow side. */
	auto *scroll = new QScrollArea(content);
	scroll->setObjectName(QStringLiteral("streamInfoScroll"));
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

	auto *rowsContainer = new QWidget(scroll);
	rowsLayout = new QVBoxLayout(rowsContainer);
	rowsLayout->setContentsMargins(0, 0, 0, 0);
	rowsLayout->setSpacing(8);
	rowsLayout->addStretch(1);
	scroll->setWidget(rowsContainer);
	layout->addWidget(scroll, 1);

	emptyHint = new QLabel(QTStr("Multistream.Info.NoChannels"), content);
	emptyHint->setObjectName(QStringLiteral("streamInfoHint"));
	emptyHint->setWordWrap(true);
	layout->addWidget(emptyHint);

	applyButton = new QPushButton(QTStr("Multistream.Info.Apply"), content);
	applyButton->setObjectName(QStringLiteral("streamInfoApply"));
	connect(applyButton, &QPushButton::clicked, this, &StreamInfoDock::ApplyAll);
	layout->addWidget(applyButton);

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
	emptyHint->setVisible(rows.empty());
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

	row.titleStateLabel = new QLabel(widget);
	row.titleStateLabel->setObjectName(QStringLiteral("streamInfoState"));
	header->addWidget(row.titleStateLabel);
	header->addStretch(1);

	row.overrideButton = new QPushButton(widget);
	row.overrideButton->setObjectName(QStringLiteral("streamInfoOverride"));
	connect(row.overrideButton, &QPushButton::clicked, this, [this, index]() {
		ChannelRow *target = RowAt(index);
		if (target)
			SetOverrideEnabled(index, !target->titleEdit->isVisible());
	});
	header->addWidget(row.overrideButton);
	outer->addLayout(header);

	row.titleEdit = new QLineEdit(FromStd(row.channel.title), widget);
	row.titleEdit->setObjectName(QStringLiteral("streamInfoTitle"));
	row.titleEdit->setPlaceholderText(QTStr("Multistream.Info.TitlePlaceholder"));
	outer->addWidget(row.titleEdit);

	if (PlatformMetadataClient::SupportsCategories(platform)) {
		auto *categoryRow = new QHBoxLayout();
		categoryRow->setContentsMargins(0, 0, 0, 0);
		categoryRow->setSpacing(8);

		auto *categoryLabel = new QLabel(QTStr("Multistream.Info.Category"), widget);
		categoryLabel->setObjectName(QStringLiteral("streamInfoLabel"));
		categoryRow->addWidget(categoryLabel);

		row.categoryCombo = new QComboBox(widget);
		row.categoryCombo->setObjectName(QStringLiteral("streamInfoCategory"));
		row.categoryCombo->setEditable(true);
		row.categoryCombo->setInsertPolicy(QComboBox::NoInsert);
		row.categoryCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		if (!row.channel.categoryName.empty()) {
			row.categoryCombo->addItem(FromStd(row.channel.categoryName),
						   FromStd(row.channel.categoryId));
			row.categoryCombo->setCurrentIndex(0);
		}
		categoryRow->addWidget(row.categoryCombo, 1);
		outer->addLayout(categoryRow);

		row.categorySearchTimer = new QTimer(widget);
		row.categorySearchTimer->setSingleShot(true);
		row.categorySearchTimer->setInterval(CATEGORY_SEARCH_DELAY_MS);
		connect(row.categorySearchTimer, &QTimer::timeout, this, [this, index]() {
			ChannelRow *target = RowAt(index);
			if (target)
				SearchCategories(index, target->categoryCombo->currentText());
		});

		connect(row.categoryCombo, &QComboBox::editTextChanged, this, [this, index](const QString &) {
			ChannelRow *target = RowAt(index);
			if (target && !target->populatingCategories)
				target->categorySearchTimer->start();
		});
		connect(row.categoryCombo, &QComboBox::activated, this, [this, index](int comboIndex) {
			ChannelRow *target = RowAt(index);
			if (!target || comboIndex < 0)
				return;
			target->channel.categoryId = target->categoryCombo->itemData(comboIndex).toString().toStdString();
			target->channel.categoryName = target->categoryCombo->itemText(comboIndex).toStdString();
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
}

void StreamInfoDock::SearchCategories(int index, const QString &query)
{
	ChannelRow *row = RowAt(index);
	if (!row || !row->categoryCombo || query.trimmed().size() < 2)
		return;

	const MultiStreamChannel channel = row->channel;
	const auto registration = MultistreamAccountsDialog::RegistrationForPlatform(channel.platform);
	QPointer<StreamInfoDock> guard(this);

	MultistreamTaskPool().start([guard, index, channel, registration, text = query.trimmed().toStdString()]() {
		OAuthTokenSet tokens;
		std::string error;
		std::vector<StreamCategory> results;
		if (LoadUsableTokens(channel, registration, tokens, error))
			PlatformMetadataClient::SearchCategories(channel.platform, registration, tokens, text, results,
								 error);
		if (!guard)
			return;
		QMetaObject::invokeMethod(
			guard.data(),
			[guard, index, results = std::move(results)]() {
				if (!guard)
					return;
				ChannelRow *target = guard->RowAt(index);
				if (!target || !target->categoryCombo || results.empty())
					return;

				/* Keep what the user typed; only the list changes. */
				const QString typed = target->categoryCombo->currentText();
				target->populatingCategories = true;
				target->categoryCombo->clear();
				for (const auto &category : results)
					target->categoryCombo->addItem(FromStd(category.name), FromStd(category.id));
				target->categoryCombo->setEditText(typed);
				target->populatingCategories = false;
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

	const QString sharedTitle = sharedTitleEdit->text().trimmed();
	config_set_string(App()->GetUserConfig(), SHARED_TITLE_SECTION, SHARED_TITLE_KEY,
			  sharedTitle.toUtf8().constData());
	config_save_safe(App()->GetUserConfig(), "tmp", nullptr);
	SaveToStore();

	pendingApplies = static_cast<int>(rows.size());
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
						  : QTStr("Multistream.Info.FailedWith").arg(message));
		row->resultLabel->setProperty("resultState", success ? "ok" : "error");
		if (row->resultLabel->style()) {
			row->resultLabel->style()->unpolish(row->resultLabel);
			row->resultLabel->style()->polish(row->resultLabel);
		}
	}

	if (pendingApplies > 0)
		--pendingApplies;
	UpdateApplyButton();
}
