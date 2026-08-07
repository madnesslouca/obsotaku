/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "OAuthPkce.hpp"

#include <QByteArray>
#include <QCryptographicHash>
#include <QRandomGenerator>

#include <vector>

static std::string Base64Url(const QByteArray &data)
{
	return data.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals).toStdString();
}

static QByteArray RandomBytes(int count)
{
	/* fillRange works on 32-bit words, so round up and trim the result. */
	std::vector<quint32> words(static_cast<size_t>((count + 3) / 4));
	QRandomGenerator::system()->fillRange(words.data(), static_cast<qsizetype>(words.size()));
	return QByteArray(reinterpret_cast<const char *>(words.data()), count);
}

OAuthPkceData OAuthPkce::Generate()
{
	OAuthPkceData result;
	result.verifier = Base64Url(RandomBytes(64));
	result.challenge = ChallengeForVerifier(result.verifier);
	result.state = Base64Url(RandomBytes(32));
	return result;
}

std::string OAuthPkce::ChallengeForVerifier(const std::string &verifier)
{
	const QByteArray digest =
		QCryptographicHash::hash(QByteArray::fromStdString(verifier), QCryptographicHash::Sha256);
	return Base64Url(digest);
}
