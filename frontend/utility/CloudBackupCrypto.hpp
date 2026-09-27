/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <QByteArray>
#include <QString>

namespace CloudBackupCrypto {

bool Supported();
bool Encrypt(const QByteArray &plain, const QString &passphrase, QByteArray &encrypted, QString &error);
bool Decrypt(const QByteArray &encrypted, const QString &passphrase, QByteArray &plain, QString &error);

} // namespace CloudBackupCrypto
