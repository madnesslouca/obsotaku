/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <utility/MultiStreamManager.hpp>

#include <QDialog>

class QLineEdit;
class QLabel;

/* Server and stream key form for the platforms that do not expose the ingest
 * credentials through an API (Facebook Live, TikTok, X, Trovo, custom RTMP).
 * The key is handed back in the channel and stored by the caller; this dialog
 * never writes it to the configuration. */
class ManualChannelDialog : public QDialog {
	Q_OBJECT

public:
	/* Pass an existing channel to edit it, or a default-constructed one with
	 * the platform set to create a new destination. */
	ManualChannelDialog(QWidget *parent, StreamPlatform platform, MultiStreamChannel channel);

	MultiStreamChannel Channel() const { return channel; }

protected:
	void accept() override;

private:
	MultiStreamChannel channel;
	QLineEdit *nameEdit = nullptr;
	QLineEdit *serverEdit = nullptr;
	QLineEdit *keyEdit = nullptr;
	QLabel *errorLabel = nullptr;
};
