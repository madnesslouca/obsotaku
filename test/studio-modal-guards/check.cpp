/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include <OBSApp.hpp>
#include <QApplication>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QInputDialog>
#include <QElapsedTimer>
#include <QPointer>
#include <QPushButton>
#include <QTemporaryDir>
#include <atomic>
#include <dialogs/MultistreamAccountsDialog.hpp>
#include <iostream>
#include <oauth/AuthListener.hpp>
#include <utility/MultistreamChannelStore.hpp>
#include <utility/MultistreamTaskPool.hpp>
#include <utility/PlatformIconProvider.hpp>
OBSApp stub;
OBSApp *App()
{
	return &stub;
}
std::atomic_int loads{0}, guiIO{0}, exchanges{0}, checks{0};
QJsonArray failures;
void check(bool pass, const QString &s)
{
	++checks;
	if (!pass) {
		failures.append(s);
	}
}
QPixmap PlatformIconProvider::Badge(StreamPlatform, int s, qreal)
{
	QPixmap p(s, s);
	p.fill(Qt::gray);
	return p;
}
QPixmap PlatformIconProvider::Glyph(StreamPlatform p, int s, qreal d)
{
	return Badge(p, s, d);
}
bool PlatformIconProvider::HasArtwork(StreamPlatform)
{
	return false;
}
std::optional<std::string> SecureTokenStore::Load(const std::string &p, const std::string &a, std::string &error)
{
	++loads;
	if (QThread::currentThread() == qApp->thread()) {
		++guiIO;
	}
	error.clear();
	if (p == "kick-client" && a == "qa-kick-client") {
		return "private-client-canary";
	}
	return {};
}
bool SecureTokenStore::Save(const std::string &, const std::string &, const std::string &, std::string &)
{
	++guiIO;
	return false;
}
bool SecureTokenStore::Remove(const std::string &, const std::string &, std::string &)
{
	++guiIO;
	return false;
}
std::string mode;
int saves = 0, commits = 0, starts = 0, authorized = 0;
bool completed = false;
std::string guardValue()
{
	std::string out = stub.userConfigLocation.string();
	for (auto section : {"Basic", "MultistreamAccount.kick", "MultistreamChannels"}) {
		for (auto key : {"ProfileDir", "ClientId", "Count", "Channel0.DisplayName"}) {
			auto *value = config_get_string(stub.config, section, key);
			out += std::string("|") + section + "/" + key + "=" + (value ? value : "");
		}
	}
	return out;
}
std::function<bool()> MultistreamChannelStore::CaptureMetadataGuard()
{
	auto expected = guardValue();
	return [expected] {
		return expected == guardValue();
	};
}
bool MultistreamChannelStore::IsBusy()
{
	return false;
}
MultistreamChannelStore::Request MultistreamChannelStore::SaveSecretAsync(std::string, std::string, std::string,
									  QObject *receiver,
									  std::function<bool(std::string &)> commit,
									  Completion done,
									  std::function<bool()> canCommit)
{
	++saves;
	auto request = std::make_shared<SecureTokenStore::RequestContext>();
	if (mode == "profile-late") {
		config_set_string(stub.config, "Basic", "ProfileDir", "external-profile");
	}
	if (mode == "client-late") {
		config_set_string(stub.config, "MultistreamAccount.kick", "ClientId", "external-client");
	}
	if (mode == "endpoint-late") {
		qputenv("OBS_MULTISTREAM_KICK_TOKEN_PROXY", "https://private-fixture.invalid/changed");
	}
	QTimer::singleShot(0, receiver,
			   [commit = std::move(commit), done = std::move(done),
			    canCommit = std::move(canCommit)]() mutable {
				   std::string error;
				   MultistreamChannelStore::Result result;
				   result.success = (!canCommit || canCommit()) && commit(error);
				   result.configCommitted = result.success;
				   result.deliveryCurrent = result.success;
				   result.error = error;
				   if (result.success) {
					   ++commits;
				   }
				   completed = true;
				   done(std::move(result));
			   });
	return request;
}
OAuthTokenSet::~OAuthTokenSet() = default;
bool PlatformOAuthClient::CreateAuthorizationSession(StreamPlatform p, const OAuthClientRegistration &r,
						     const std::string &redirect, OAuthAuthorizationSession &s,
						     std::string &error)
{
	check(r.clientSecret.empty(), "PKCE GUI session creation does not receive saved secret");
	s.platform = p;
	s.authorizationUrl = "https://private-fixture.invalid/auth";
	s.redirectUri = redirect;
	s.state = "private-fixture-state";
	error.clear();
	return true;
}
bool PlatformOAuthClient::ExchangeAuthorizationCode(const OAuthClientRegistration &r, const OAuthAuthorizationSession &,
						    const std::string &, OAuthTokenSet &t, std::string &e)
{
	++exchanges;
	check(QThread::currentThread() != qApp->thread(), "fake token exchange executes on worker");
	check(r.clientSecret == "private-client-canary", "saved Kick credential reaches authorized exchange");
	t.accessToken = "private-access-token";
	e.clear();
	return true;
}
bool PlatformOAuthClient::StartDeviceAuthorization(StreamPlatform, const OAuthClientRegistration &,
						   OAuthDeviceAuthorization &, std::string &)
{
	return false;
}
OAuthDevicePollStatus PlatformOAuthClient::PollDeviceAuthorization(StreamPlatform, const OAuthClientRegistration &,
								   const OAuthDeviceAuthorization &, OAuthTokenSet &,
								   std::string &)
{
	return OAuthDevicePollStatus::Error;
}
bool ConnectedAccountManager::ResolveChannel(const ConnectedStreamAccount &a, const OAuthClientRegistration &,
					     const std::string &, MultiStreamChannel &c, std::string &e)
{
	c.platform = a.platform;
	c.id = "kick:" + a.accountId;
	c.accountId = a.accountId;
	c.server = "rtmp://127.0.0.1/local";
	c.streamKey = "private-key";
	e.clear();
	return true;
}
bool ConnectedAccountManager::CompleteConnection(StreamPlatform p, const OAuthClientRegistration &,
						 const OAuthTokenSet &, ConnectedStreamAccount &a,
						 MultiStreamChannel &c, std::string &e, const std::atomic_bool *)
{
	a = {p, "new-account", "Private account", true};
	return ResolveChannel(a, {}, "", c, e);
}
void ConnectedAccountManager::PreserveUserSettings(const MultiStreamChannel &before, MultiStreamChannel &after)
{
	after.audioMixIndex = before.audioMixIndex;
	after.vodTrackEnabled = before.vodTrackEnabled;
	after.vodTrackIndex = before.vodTrackIndex;
}
int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	QTemporaryDir tmp;
	mode = argc > 1 ? argv[1] : "none";
	stub.userConfigLocation = tmp.path().toStdString();
	config_open(&stub.config, (tmp.path() + "/user.ini").toUtf8(), CONFIG_OPEN_ALWAYS);
	config_set_string(stub.config, "Basic", "ProfileDir", "original-profile");
	config_set_string(stub.config, "MultistreamAccount.kick", "ClientId", "original-client");
	qputenv("OBS_MULTISTREAM_KICK_CLIENT_ID", "qa-kick-client");
	qputenv("OBS_MULTISTREAM_KICK_TOKEN_PROXY", "");
	MultiStreamChannel pending;
	pending.platform = StreamPlatform::Kick;
	auto outerDraft = MultistreamChannelStore::CaptureMetadataGuard();
	MultistreamAccountsDialog dialog(nullptr, {pending}, StreamPlatform::Kick);
	dialog.show();
	auto *configure = dialog.findChild<QPushButton *>("btnConfigure_kick");
	check(configure, "actual Configure Kick gesture exists");
	quint64 authorizedOperation = 0, startedOperation = 0;
	QObject::connect(&dialog, &MultistreamAccountsDialog::registrationChangeStarted, [&](quint64 operation) {
		++starts;
		startedOperation = operation;
		authorizedOperation = outerDraft() ? operation : 0;
	});
	QObject::connect(&dialog, &MultistreamAccountsDialog::registrationCommitted, [&](quint64 operation) {
		++authorized;
		check(operation && operation == startedOperation,
		      "durable registration signal matches its operation token");
		if (authorizedOperation == operation) {
			outerDraft = MultistreamChannelStore::CaptureMetadataGuard();
			authorizedOperation = 0;
		}
	});
	QTimer timer;
	int prompts = 0;
	bool message = false, conflictReported = false, configureReturned = false;
	QPointer<QInputDialog> previous;
	QElapsedTimer clock;
	clock.start();
	QObject::connect(&timer, &QTimer::timeout, [&] {
		auto *input = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
		if (input && input != previous) {
			previous = input;
			++prompts;
			bool change = (prompts == 1 && QString::fromStdString(mode).endsWith("-id")) ||
				      (prompts == 2 && QString::fromStdString(mode).endsWith("-secret"));
			if (change) {
				if (QString::fromStdString(mode).startsWith("profile")) {
					config_set_string(stub.config, "Basic", "ProfileDir", "external-profile");
				}
				if (QString::fromStdString(mode).startsWith("client")) {
					config_set_string(stub.config, "MultistreamAccount.kick", "ClientId",
							  "external-client");
				}
				if (QString::fromStdString(mode).startsWith("env")) {
					qputenv("OBS_MULTISTREAM_KICK_CLIENT_ID", "externally-changed-client");
				}
				if (QString::fromStdString(mode).startsWith("port")) {
					qputenv("OBS_MULTISTREAM_KICK_CALLBACK_PORT", "49479");
				}
				if (QString::fromStdString(mode).startsWith("endpoint")) {
					qputenv("OBS_MULTISTREAM_KICK_TOKEN_PROXY",
						"https://private-fixture.invalid/changed");
				}
			}
			input->setTextValue(prompts == 1 ? "qa-new-client" : "private-local-secret");
			if ((mode == "cancel-id" && prompts == 1) || (mode == "cancel-secret" && prompts == 2)) {
				QTimer::singleShot(0, input, &QDialog::reject);
			} else {
				QTimer::singleShot(0, input, &QDialog::accept);
			}
		}
		if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
			message = true;
			conflictReported |= box->text().contains("Multistream.Credentials.StateChanged") ||
					    box->text().contains("Provider registration changed");
			QTimer::singleShot(0, box, &QDialog::accept);
		}
		const bool cancelled = QString::fromStdString(mode).startsWith("cancel-");
		const bool outerCase = QString::fromStdString(mode).startsWith("outer-");
		const bool conflict = mode != "none" && !cancelled && !outerCase;
		if (clock.elapsed() > 400 &&
		    ((completed && message) || (saves == 0 && (message || (cancelled && configureReturned))))) {
			check(cancelled  ? saves == 0
			      : conflict ? saves == 0 || QString::fromStdString(mode).endsWith("-late")
					 : saves == 1,
			      "conflicting prompt rejected before credential dispatch; unchanged prompt dispatches");
			check((conflict || cancelled) ? commits == 0 : commits == 1,
			      "conflicting scope or registration never commits; unchanged registration commits");
			check(prompts > 0, "real Qt input prompt exercised");
			check(guiIO == 0, "GUI never reads or mutates credential backend");
			check(starts == 1, "registration flow starts one operation token");
			check(authorized == ((conflict || cancelled) ? 0 : 1),
			      "only a durable current operation emits authorization");
			check(conflict ? conflictReported : !conflictReported,
			      "conflict is visible and cancellation is not reported as conflict");
			if (mode == "outer-client-after") {
				config_set_string(stub.config, "MultistreamAccount.kick", "ClientId",
						  "external-after-commit");
			}
			if (mode == "none" || outerCase) {
				check(outerDraft() == !outerCase,
				      "outer modal accepts its own authorized commit but rejects external registration edits");
			}
			ShutdownMultistreamTasks();
			timer.stop();
			std::cout
				<< QJsonDocument(
					   QJsonObject{
						   {"mode", QString::fromStdString(mode)},
						   {"passed", failures.empty()},
						   {"checks", int(checks)},
						   {"failures", failures},
						   {"saves", saves},
						   {"commits", commits},
						   {"prompts", prompts},
						   {"scope",
						    "Actual AccountsDialog Qt prompts; fake Store boundary and no credential/HTTP network."}})
					   .toJson()
					   .constData();
			app.exit(failures.empty() ? 0 : 1);
		}
	});
	timer.start(10);
	QTimer::singleShot(0, &dialog, [&] {
		if (mode == "outer-client-before") {
			config_set_string(stub.config, "MultistreamAccount.kick", "ClientId",
					  "external-before-operation");
		}
		if (configure) {
			configure->click();
		}
		configureReturned = true;
	});
	QTimer::singleShot(4000, &dialog, [&] {
		std::cout << "fixture timeout\n";
		app.exit(2);
	});
	auto rc = app.exec();
	config_close(stub.config);
	return rc;
}
#include "check.moc"
