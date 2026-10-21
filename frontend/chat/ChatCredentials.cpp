/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

// Shared credential loader. Tokens stay in the platform credential store.
#include "ChatConnection.hpp"
#include <dialogs/MultistreamAccountsDialog.hpp>
#include <oauth/ConnectedAccountManager.hpp>
#include <utility/MultistreamTaskPool.hpp>

void LoadChatAccessToken(StreamPlatform platform, const QString &accountId, QObject *context,
			 ChatTokenCallback callback)
{
	QPointer<QObject> guard(context);
	MultistreamTaskPool().start([guard, platform, accountId, callback = std::move(callback)] {
		std::string error;
		OAuthTokenSet tokens;
		const auto registration = MultistreamAccountsDialog::RegistrationForPlatform(platform);
		const bool loaded = ConnectedAccountManager::LoadUsableTokens(platform, accountId.toStdString(),
									      registration, {}, tokens, error);
		const ChatAccessToken result{loaded ? QString::fromStdString(tokens.accessToken) : QString(),
					     QString::fromStdString(registration.clientId),
					     QString::fromStdString(tokens.scope).split(' ', Qt::SkipEmptyParts)};
		if (guard) {
			QMetaObject::invokeMethod(
				guard,
				[guard, result, callback] {
					if (guard) {
						callback(result);
					}
				},
				Qt::QueuedConnection);
		}
	});
}
