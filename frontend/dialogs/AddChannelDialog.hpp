/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <utility/StreamPlatform.hpp>

#include <QDialog>

#include <optional>
#include <vector>

/* Platform picker for a new destination, shown as a grid of brand tiles.
 * Picking a tile only reports the choice: the caller runs the OAuth flow or
 * the manual key form, because those need the output handler. */
class AddChannelDialog : public QDialog {
	Q_OBJECT

public:
	explicit AddChannelDialog(QWidget *parent = nullptr);

	std::optional<StreamPlatform> SelectedPlatform() const { return selectedPlatform; }

private:
	void AddSection(const QString &title, const std::vector<StreamPlatform> &platforms, class QVBoxLayout *layout);
	void AddPlatformTile(StreamPlatform platform, class QGridLayout *grid, int row, int column);

	std::optional<StreamPlatform> selectedPlatform;
};
