/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "ManualChannelDialog.hpp"

#include <utility/MultistreamChannelStore.hpp>
#include <utility/StreamPlatformDisplay.hpp>

#include <OBSApp.hpp>
#include <qt-wrappers.hpp>

#include <QCheckBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

#include "moc_ManualChannelDialog.cpp"

namespace {
QString FromView(std::string_view value)
{
	return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

bool LooksLikeRtmpUrl(const QString &value)
{
	const QString lowered = value.trimmed().toLower();
	return lowered.startsWith(QStringLiteral("rtmp://")) || lowered.startsWith(QStringLiteral("rtmps://"));
}
} // namespace

ManualChannelDialog::ManualChannelDialog(QWidget *parent, StreamPlatform platform, MultiStreamChannel existing)
	: QDialog(parent),
	  channel(std::move(existing))
{
	const auto &info = GetStreamPlatformInfo(platform);
	channel.platform = platform;
	if (channel.id.empty())
		channel.id = MultistreamChannelStore::NewManualChannelId(platform);

	const QString platformName = StreamPlatformDisplayName(platform);
	setWindowTitle(QTStr("Multistream.ManualChannel.Title").arg(platformName));
	setObjectName(QStringLiteral("manualChannelDialog"));
	setModal(true);
	setMinimumWidth(520);

	auto *layout = new QVBoxLayout(this);
	layout->setSpacing(12);

	/* A generic RTMP server is manual by definition; only the real platforms
	 * need the explanation about the missing API. */
	auto *intro = new QLabel(platform == StreamPlatform::CustomRtmp
					 ? QTStr("Multistream.ManualChannel.CustomIntro")
					 : QTStr("Multistream.ManualChannel.Intro").arg(platformName),
				 this);
	intro->setObjectName(QStringLiteral("infoBanner"));
	intro->setWordWrap(true);
	layout->addWidget(intro);

	auto *form = new QFormLayout();
	form->setSpacing(10);
	form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

	nameEdit = new QLineEdit(QString::fromStdString(channel.displayName), this);
	nameEdit->setPlaceholderText(platformName);
	form->addRow(QTStr("Multistream.ManualChannel.Name"), nameEdit);

	serverEdit = new QLineEdit(this);
	serverEdit->setText(channel.server.empty() ? FromView(info.defaultIngestServer)
						   : QString::fromStdString(channel.server));
	serverEdit->setPlaceholderText(QStringLiteral("rtmp://"));
	form->addRow(QTStr("Multistream.ManualChannel.Server"), serverEdit);

	keyEdit = new QLineEdit(QString::fromStdString(channel.streamKey), this);
	/* Masked by default: the key is a credential and is often typed with
	 * someone watching a stream of the setup itself. */
	keyEdit->setEchoMode(QLineEdit::Password);
	form->addRow(QTStr("Multistream.ManualChannel.StreamKey"), keyEdit);

	auto *showKey = new QCheckBox(QTStr("Multistream.ManualChannel.ShowKey"), this);
	connect(showKey, &QCheckBox::toggled, this, [this](bool checked) {
		keyEdit->setEchoMode(checked ? QLineEdit::Normal : QLineEdit::Password);
	});
	form->addRow(QString(), showKey);

	layout->addLayout(form);

	auto *storageNote = new QLabel(QTStr("Multistream.ManualChannel.StorageNote"), this);
	storageNote->setObjectName(QStringLiteral("manualChannelNote"));
	storageNote->setWordWrap(true);
	layout->addWidget(storageNote);

	errorLabel = new QLabel(this);
	errorLabel->setObjectName(QStringLiteral("manualChannelError"));
	errorLabel->setWordWrap(true);
	errorLabel->setVisible(false);
	layout->addWidget(errorLabel);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
	if (!info.streamKeyHelpUrl.empty()) {
		auto *help = buttons->addButton(QTStr("Multistream.ManualChannel.WhereIsMyKey"),
						QDialogButtonBox::HelpRole);
		const QString helpUrl = FromView(info.streamKeyHelpUrl);
		connect(help, &QPushButton::clicked, this, [helpUrl]() { QDesktopServices::openUrl(QUrl(helpUrl)); });
	}
	connect(buttons, &QDialogButtonBox::accepted, this, &ManualChannelDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &ManualChannelDialog::reject);
	layout->addWidget(buttons);
}

void ManualChannelDialog::accept()
{
	const QString name = nameEdit->text().trimmed();
	const QString server = serverEdit->text().trimmed();
	const QString key = keyEdit->text().trimmed();

	auto fail = [this](const QString &message) {
		errorLabel->setText(message);
		errorLabel->setVisible(true);
	};

	if (server.isEmpty() || key.isEmpty()) {
		fail(QTStr("Multistream.ManualChannel.MissingFields"));
		return;
	}
	if (!LooksLikeRtmpUrl(server)) {
		fail(QTStr("Multistream.ManualChannel.InvalidServer"));
		return;
	}

	channel.displayName = name.isEmpty() ? StreamPlatformDisplayName(channel.platform).toStdString()
					     : name.toStdString();
	channel.server = server.toStdString();
	channel.streamKey = key.toStdString();
	QDialog::accept();
}
