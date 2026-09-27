/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "NdiNetworkDialog.hpp"

#include <OBSApp.hpp>
#include <qt-wrappers.hpp>

#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPalette>
#include <QPushButton>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace {
QLabel *MutedLabel(const QString &text, QWidget *parent)
{
	auto *label = new QLabel(text, parent);
	label->setProperty("class", QStringLiteral("text-muted"));
	return label;
}

QString AdapterTitle(const NdiNetworkAdapter &adapter)
{
	return QStringLiteral("%1  —  %2").arg(adapter.displayName, adapter.address);
}

void SetElideFriendly(QLabel *label)
{
	label->setTextInteractionFlags(Qt::TextSelectableByMouse);
	label->setWordWrap(true);
}
} // namespace

NdiNetworkDialog::NdiNetworkDialog(QWidget *parent) : QDialog(parent), initialAddress(NdiNetworkConfig::SelectedAddress())
{
	setObjectName(QStringLiteral("ndiNetworkDialog"));
	setWindowTitle(QTStr("NdiNetwork.Title"));
	setModal(true);
	setMinimumSize(700, 590);
	resize(740, 640);

	const QColor accent = palette().color(QPalette::Highlight);
	const QColor surface = palette().color(QPalette::AlternateBase);
	const QColor border = palette().color(QPalette::Mid);
	setStyleSheet(QStringLiteral(
			  "#ndiHeader, #ndiDetails, #ndiRoute, #ndiStatusCard { border: 1px solid %1; border-radius: 8px; }"
			  "#ndiHeader, #ndiDetails, #ndiRoute, #ndiStatusCard { background: %2; }"
			  "#ndiBadge { background: %3; color: %4; border-radius: 7px; padding: 6px 11px; font-weight: 700; }"
			  "#ndiTitle { font-size: 19px; font-weight: 650; }"
			  "#ndiSectionTitle { font-size: 14px; font-weight: 650; }"
			  "#ndiStatusDotReady { color: #45c779; font-size: 17px; }"
			  "#ndiStatusDotMissing { color: #e5a84b; font-size: 17px; }"
			  "#ndiNetworkDialog #infoBanner { background: %2; border: 1px solid %1; border-radius: 8px; padding: 10px 14px; }"
			  "#ndiRouteNode { border: 1px solid %1; border-radius: 6px; padding: 8px 12px; font-weight: 600; }"
			  "#ndiRouteNodeSelected { border: 2px solid %3; border-radius: 6px; padding: 7px 11px; font-weight: 650; }")
			  .arg(border.name(), surface.name(), accent.name(), palette().color(QPalette::HighlightedText).name()));

	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(18, 18, 18, 16);
	root->setSpacing(12);

	auto *header = new QFrame(this);
	header->setObjectName(QStringLiteral("ndiHeader"));
	auto *headerLayout = new QHBoxLayout(header);
	headerLayout->setContentsMargins(16, 14, 16, 14);
	headerLayout->setSpacing(14);
	auto *badge = new QLabel(QStringLiteral("NDI®"), header);
	badge->setObjectName(QStringLiteral("ndiBadge"));
	badge->setAlignment(Qt::AlignCenter);
	headerLayout->addWidget(badge, 0, Qt::AlignTop);
	auto *heading = new QVBoxLayout;
	heading->setSpacing(3);
	auto *title = new QLabel(QTStr("NdiNetwork.Heading"), header);
	title->setObjectName(QStringLiteral("ndiTitle"));
	heading->addWidget(title);
	auto *subtitle = MutedLabel(QTStr("NdiNetwork.Subtitle"), header);
	subtitle->setWordWrap(true);
	heading->addWidget(subtitle);
	headerLayout->addLayout(heading, 1);
	root->addWidget(header);

	auto *statusRow = new QHBoxLayout;
	statusRow->setSpacing(10);
	statusRow->addWidget(CreateStatusCard(QTStr("NdiNetwork.Module"),
		NdiNetworkConfig::ModuleLoaded() ? QTStr("NdiNetwork.Module.Ready") : QTStr("NdiNetwork.Module.Missing"),
		NdiNetworkConfig::ModuleLoaded(), this), 1);
	statusRow->addWidget(CreateStatusCard(QTStr("NdiNetwork.Runtime"),
		NdiNetworkConfig::RuntimeInstalled() ? QTStr("NdiNetwork.Runtime.Ready")
						     : QTStr("NdiNetwork.Runtime.Missing"),
		NdiNetworkConfig::RuntimeInstalled(), this), 1);
	root->addLayout(statusRow);

	auto *choiceTitle = new QLabel(QTStr("NdiNetwork.Adapter.Label"), this);
	choiceTitle->setObjectName(QStringLiteral("ndiSectionTitle"));
	root->addWidget(choiceTitle);
	auto *choiceRow = new QHBoxLayout;
	adapterCombo = new QComboBox(this);
	adapterCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	choiceRow->addWidget(adapterCombo, 1);
	auto *refreshButton = new QPushButton(QTStr("NdiNetwork.Refresh"), this);
	refreshButton->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
	choiceRow->addWidget(refreshButton);
	root->addLayout(choiceRow);

	selectionStatus = new QLabel(this);
	selectionStatus->setWordWrap(true);
	root->addWidget(selectionStatus);

	auto *details = new QFrame(this);
	details->setObjectName(QStringLiteral("ndiDetails"));
	auto *detailsLayout = new QGridLayout(details);
	detailsLayout->setContentsMargins(14, 11, 14, 11);
	detailsLayout->setHorizontalSpacing(18);
	detailsLayout->setVerticalSpacing(6);
	interfaceValue = new QLabel(details);
	addressValue = new QLabel(details);
	subnetValue = new QLabel(details);
	macValue = new QLabel(details);
	for (QLabel *value : {interfaceValue, addressValue, subnetValue, macValue})
		SetElideFriendly(value);
	detailsLayout->addWidget(MutedLabel(QTStr("NdiNetwork.Detail.Interface"), details), 0, 0);
	detailsLayout->addWidget(interfaceValue, 0, 1);
	detailsLayout->addWidget(MutedLabel(QTStr("NdiNetwork.Detail.IPv4"), details), 1, 0);
	detailsLayout->addWidget(addressValue, 1, 1);
	detailsLayout->addWidget(MutedLabel(QTStr("NdiNetwork.Detail.Subnet"), details), 0, 2);
	detailsLayout->addWidget(subnetValue, 0, 3);
	detailsLayout->addWidget(MutedLabel(QTStr("NdiNetwork.Detail.Mac"), details), 1, 2);
	detailsLayout->addWidget(macValue, 1, 3);
	detailsLayout->setColumnStretch(1, 1);
	detailsLayout->setColumnStretch(3, 1);
	root->addWidget(details);

	auto *route = new QFrame(this);
	route->setObjectName(QStringLiteral("ndiRoute"));
	auto *routeLayout = new QHBoxLayout(route);
	routeLayout->setContentsMargins(14, 12, 14, 12);
	auto *thisPc = new QLabel(QTStr("NdiNetwork.Route.ThisPc"), route);
	thisPc->setObjectName(QStringLiteral("ndiRouteNode"));
	routeLayout->addWidget(thisPc);
	routeLayout->addWidget(MutedLabel(QStringLiteral("→"), route));
	routeNode = new QLabel(route);
	routeNode->setObjectName(QStringLiteral("ndiRouteNodeSelected"));
	routeNode->setAlignment(Qt::AlignCenter);
	routeLayout->addWidget(routeNode, 1);
	routeLayout->addWidget(MutedLabel(QStringLiteral("→"), route));
	auto *receiver = new QLabel(QTStr("NdiNetwork.Route.SecondPc"), route);
	receiver->setObjectName(QStringLiteral("ndiRouteNode"));
	routeLayout->addWidget(receiver);
	root->addWidget(route);

	auto *tip = new QLabel(QTStr("NdiNetwork.Help"), this);
	tip->setWordWrap(true);
	tip->setObjectName(QStringLiteral("infoBanner"));
	root->addWidget(tip);

	auto *footer = new QHBoxLayout;
	auto *links = new QVBoxLayout;
	links->setSpacing(1);
	auto *trademark = MutedLabel(QTStr("NdiNetwork.Trademark"), this);
	trademark->setTextInteractionFlags(Qt::TextSelectableByMouse);
	links->addWidget(trademark);
	auto *learnMore = new QLabel(QStringLiteral("<a href=\"https://docs.ndi.video/all/getting-started/white-paper/nic-selection\">%1</a>")
					.arg(QTStr("NdiNetwork.LearnMore")), this);
	learnMore->setOpenExternalLinks(true);
	links->addWidget(learnMore);
	footer->addLayout(links, 1);
	if (!NdiNetworkConfig::RuntimeInstalled()) {
		auto *installRuntime = new QPushButton(QTStr("NdiNetwork.InstallRuntime"), this);
		connect(installRuntime, &QPushButton::clicked, this, []() {
			QDesktopServices::openUrl(QUrl(QStringLiteral("https://ndi.link/NDIRedistV6")));
		});
		footer->addWidget(installRuntime);
	}
	root->addLayout(footer);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
	saveButton = buttons->addButton(QTStr("NdiNetwork.Save"), QDialogButtonBox::AcceptRole);
	saveButton->setDefault(true);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	connect(saveButton, &QPushButton::clicked, this, &NdiNetworkDialog::SaveAndAccept);
	root->addWidget(buttons);

	connect(refreshButton, &QPushButton::clicked, this, &NdiNetworkDialog::RefreshAdapters);
	connect(adapterCombo, &QComboBox::currentIndexChanged, this, &NdiNetworkDialog::UpdateDetails);
	RefreshAdapters();
}

QFrame *NdiNetworkDialog::CreateStatusCard(const QString &title, const QString &description, bool ready,
					   QWidget *parent)
{
	auto *card = new QFrame(parent);
	card->setObjectName(QStringLiteral("ndiStatusCard"));
	auto *layout = new QHBoxLayout(card);
	layout->setContentsMargins(12, 8, 12, 8);
	layout->setSpacing(9);
	auto *dot = new QLabel(QStringLiteral("●"), card);
	dot->setObjectName(ready ? QStringLiteral("ndiStatusDotReady") : QStringLiteral("ndiStatusDotMissing"));
	layout->addWidget(dot);
	auto *text = new QVBoxLayout;
	text->setSpacing(0);
	auto *titleLabel = new QLabel(title, card);
	titleLabel->setObjectName(QStringLiteral("ndiSectionTitle"));
	text->addWidget(titleLabel);
	auto *descriptionLabel = MutedLabel(description, card);
	descriptionLabel->setWordWrap(true);
	text->addWidget(descriptionLabel);
	layout->addLayout(text, 1);
	return card;
}

void NdiNetworkDialog::RefreshAdapters()
{
	const QString wantedAddress = adapterCombo->count() > 0 ? adapterCombo->currentData().toString() : initialAddress;
	adapters = NdiNetworkConfig::AvailableAdapters();
	adapterCombo->blockSignals(true);
	adapterCombo->clear();
	adapterCombo->addItem(QTStr("NdiNetwork.Adapter.Automatic"), QString{});
	for (const auto &adapter : adapters)
		adapterCombo->addItem(AdapterTitle(adapter), adapter.address);

	int selectedIndex = adapterCombo->findData(wantedAddress);
	if (!wantedAddress.isEmpty() && selectedIndex < 0) {
		adapterCombo->addItem(QTStr("NdiNetwork.Adapter.Unavailable").arg(wantedAddress), wantedAddress);
		selectedIndex = adapterCombo->count() - 1;
	}
	adapterCombo->setCurrentIndex(std::max(0, selectedIndex));
	adapterCombo->blockSignals(false);
	UpdateDetails();
}

void NdiNetworkDialog::UpdateDetails()
{
	const QString address = adapterCombo->currentData().toString();
	const auto found = std::find_if(adapters.begin(), adapters.end(), [&address](const NdiNetworkAdapter &adapter) {
		return adapter.address == address;
	});

	if (address.isEmpty()) {
		selectionStatus->setText(QTStr("NdiNetwork.Adapter.Automatic.Help"));
		interfaceValue->setText(QTStr("NdiNetwork.Value.Automatic"));
		addressValue->setText(QStringLiteral("—"));
		subnetValue->setText(QStringLiteral("—"));
		macValue->setText(QStringLiteral("—"));
		routeNode->setText(QTStr("NdiNetwork.Route.Automatic"));
	} else if (found == adapters.end()) {
		selectionStatus->setText(QTStr("NdiNetwork.Adapter.Unavailable.Help").arg(address));
		interfaceValue->setText(QTStr("NdiNetwork.Value.Unavailable"));
		addressValue->setText(address);
		subnetValue->setText(QStringLiteral("—"));
		macValue->setText(QStringLiteral("—"));
		routeNode->setText(address);
	} else {
		selectionStatus->setText(found->running ? QTStr("NdiNetwork.Adapter.Active")
						       : QTStr("NdiNetwork.Adapter.Inactive"));
		interfaceValue->setText(found->displayName);
		addressValue->setText(QStringLiteral("%1 / %2").arg(found->address).arg(found->prefixLength));
		subnetValue->setText(found->netmask.isEmpty() ? QStringLiteral("—") : found->netmask);
		macValue->setText(found->hardwareAddress.isEmpty() ? QStringLiteral("—") : found->hardwareAddress);
		routeNode->setText(found->displayName + QStringLiteral("\n") + found->address);
	}

	saveButton->setEnabled(address != initialAddress);
}

void NdiNetworkDialog::SaveAndAccept()
{
	const QString address = adapterCombo->currentData().toString();
	if (address == initialAddress) {
		accept();
		return;
	}

	QString error;
	if (!NdiNetworkConfig::SaveSelectedAddress(address, error)) {
		OBSMessageBox::critical(this, QTStr("NdiNetwork.Error.Title"), error);
		return;
	}

	restartRequired = true;
	accept();
}
