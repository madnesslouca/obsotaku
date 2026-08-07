/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "AddChannelDialog.hpp"

#include <utility/PlatformIconProvider.hpp>
#include <utility/StreamPlatformDisplay.hpp>

#include <OBSApp.hpp>
#include <qt-wrappers.hpp>

#include <QGridLayout>
#include <QIcon>
#include <QLabel>
#include <QToolButton>
#include <QVBoxLayout>

#include "moc_AddChannelDialog.cpp"

namespace {
constexpr int TILES_PER_ROW = 4;
constexpr int BADGE_SIZE = 40;
constexpr int TILE_WIDTH = 132;
constexpr int TILE_HEIGHT = 104;

QString TileHint(StreamPlatform platform)
{
	const auto &info = GetStreamPlatformInfo(platform);
	if (platform == StreamPlatform::CustomRtmp)
		return QTStr("Multistream.AddChannel.CustomHint");
	return info.ingestMode == StreamIngestMode::ResolvedByApi ? QTStr("Multistream.AddChannel.SignInHint")
								  : QTStr("Multistream.AddChannel.StreamKeyHint");
}
} // namespace

AddChannelDialog::AddChannelDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(QTStr("Multistream.AddChannel.Title"));
	setObjectName(QStringLiteral("addChannelDialog"));
	setModal(true);

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(24, 18, 24, 20);
	layout->setSpacing(6);
	/* Without this the dialog keeps the size it was first given and clips the
	 * last row of tiles. */
	layout->setSizeConstraint(QLayout::SetFixedSize);

	/* No heading here: the window title bar already names the dialog. */
	auto *subtitle = new QLabel(QTStr("Multistream.AddChannel.Subtitle"), this);
	subtitle->setObjectName(QStringLiteral("addChannelSubtitle"));
	subtitle->setAlignment(Qt::AlignCenter);
	subtitle->setWordWrap(true);
	layout->addWidget(subtitle);

	/* Two groups, because the two kinds of destination behave differently:
	 * one signs you in, the other asks for a key you paste yourself. */
	std::vector<StreamPlatform> signIn;
	std::vector<StreamPlatform> manual;
	for (const auto platform : SelectableStreamPlatforms()) {
		if (GetStreamPlatformInfo(platform).ingestMode == StreamIngestMode::ResolvedByApi)
			signIn.push_back(platform);
		else
			manual.push_back(platform);
	}

	AddSection(QTStr("Multistream.AddChannel.SignInSection"), signIn, layout);
	AddSection(QTStr("Multistream.AddChannel.ManualSection"), manual, layout);
}

void AddChannelDialog::AddSection(const QString &title, const std::vector<StreamPlatform> &platforms,
				  QVBoxLayout *layout)
{
	if (platforms.empty())
		return;

	layout->addSpacing(12);

	auto *heading = new QLabel(title, this);
	heading->setObjectName(QStringLiteral("addChannelSection"));
	layout->addWidget(heading);
	layout->addSpacing(4);

	auto *grid = new QGridLayout();
	grid->setSpacing(10);
	layout->addLayout(grid);

	int index = 0;
	for (const auto platform : platforms) {
		AddPlatformTile(platform, grid, index / TILES_PER_ROW, index % TILES_PER_ROW);
		++index;
	}

	/* Keep the columns aligned between sections even when the last row is
	 * short, so the tiles form one grid instead of two ragged ones. */
	for (int column = 0; column < TILES_PER_ROW; ++column)
		grid->setColumnMinimumWidth(column, TILE_WIDTH);
}

void AddChannelDialog::AddPlatformTile(StreamPlatform platform, QGridLayout *grid, int row, int column)
{
	/* QToolButton in text-under-icon mode: unlike a QPushButton hosting its
	 * own layout, it reports a sizeHint that accounts for both the badge and
	 * the label, so the grid never clips the last row. */
	auto *tile = new QToolButton(this);
	tile->setObjectName(QStringLiteral("channelTile"));
	tile->setProperty("platform", StreamPlatformId(platform));
	tile->setCursor(Qt::PointingHandCursor);
	tile->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
	tile->setToolTip(TileHint(platform));
	tile->setText(StreamPlatformDisplayName(platform));
	tile->setIconSize(QSize(BADGE_SIZE, BADGE_SIZE));
	/* Drawn at 2x so the badge stays sharp on high-DPI screens; Qt scales it
	 * back down on ordinary ones. */
	tile->setIcon(QIcon(PlatformIconProvider::Badge(platform, BADGE_SIZE, 2.0)));
	tile->setMinimumSize(TILE_WIDTH, TILE_HEIGHT);
	tile->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

	connect(tile, &QToolButton::clicked, this, [this, platform]() {
		selectedPlatform = platform;
		accept();
	});

	grid->addWidget(tile, row, column);
}
