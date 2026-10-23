// SPDX-License-Identifier: GPL-2.0-or-later
// Opt-in native regression; requires an independently isolated QA application.
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QInputDialog>
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>
#include <QMenu>
#include <QAction>
#include <QAbstractButton>
#include <QDateTime>
#include <QRegularExpression>
OBS_DECLARE_MODULE()
namespace {
QString root, mode;
QMainWindow *window = nullptr;
QTimer *timer = nullptr;
QElapsedTimer clock;
qint64 last = 0, maxGap = 0;
int ticks = 0, phase = 0;
bool progressSeen = false, done = false;
QJsonArray checks;
constexpr const char *canary = "qa-local-only-credential-canary";
void check(bool ok, const QString &name)
{
	checks.append(QJsonObject{{"passed", ok}, {"name", name}});
}
void finish()
{
	if (done) {
		return;
	}
	done = true;
	if (timer) {
		timer->stop();
	}
	check(ticks > 10, "GUI event heartbeat continues");
	check(maxGap < 1500, "no credential wait stalls GUI for 1.5 seconds");
	bool pass = true;
	for (auto v : checks) {
		pass &= v.toObject()["passed"].toBool();
	}
	QFile out(root + "/runtime.json");
	(void)out.open(QIODevice::WriteOnly);
	out.write(
		QJsonDocument(
			QJsonObject{
				{"passed", pass},
				{"checks", checks},
				{"mode", mode},
				{"finished_epoch_ms", QDateTime::currentMSecsSinceEpoch()},
				{"ticks", ticks},
				{"max_gap_ms", maxGap},
				{"progress_seen", progressSeen},
				{"scope",
				 "Native isolated frontend modal regression; no streaming/capture/physical source/normal profile."}})
			.toJson());
	out.close();
	window->grab().save(root + "/window.png");
	QTimer::singleShot(300, window, [] { window->close(); });
}
class Events final : public QObject {
	bool eventFilter(QObject *o, QEvent *e) override
	{
		if (e->type() == QEvent::Show && o->objectName() == "multistreamCredentialProgress") {
			progressSeen = true;
		}
		return false;
	}
};
void tick()
{
	if (done) {
		return;
	}
	const auto now = clock.elapsed();
	if (last) {
		maxGap = qMax(maxGap, now - last);
	}
	last = now;
	++ticks;
	if (now > 30000) {
		check(false, "native UI sequence deadline");
		finish();
		return;
	}
	auto *config = obs_frontend_get_user_config();
	if (!config) {
		return;
	}
	auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());

	if (mode.startsWith("add-") || mode.startsWith("remove-")) {
		static bool warning = false;
		static qint64 attemptedAt = 0;
		auto change = [&] {
			if (mode.endsWith("profile")) {
				config_set_string(config, "Basic", "ProfileDir", "Externally_Changed_Profile");
			} else {
				config_set_string(config, "MultistreamChannels", "Channel0.DisplayName",
						  "External modal metadata");
				config_set_bool(config, "MultistreamChannels", "Channel0.Enabled", false);
			}
			config_save_safe(config, "tmp", nullptr);
		};
		if (phase == 0 && now > 1500) {
			auto *dock = window->findChild<QDockWidget *>("outputsDock");
			if (dock) {
				dock->show();
				dock->raise();
			}
			if (mode.startsWith("add-")) {
				auto *button = window->findChild<QPushButton *>("outputsAddButton");
				if (!button) {
					return;
				}
				phase = 1;
				QTimer::singleShot(0, button, &QPushButton::click);
			} else {
				auto *menu = window->findChild<QToolButton *>("channelMenu");
				if (!menu || !menu->menu()) {
					return;
				}
				phase = 1;
				auto actions = menu->menu()->actions();
				if (actions.isEmpty()) {
					check(false, "actual destination menu exposes an action");
					finish();
					return;
				}
				QTimer::singleShot(0, actions.back(), &QAction::trigger);
			}
			return;
		}
		if (phase == 1 && mode.startsWith("add-") && dialog && dialog->objectName() == "addChannelDialog") {
			for (auto *tile : dialog->findChildren<QToolButton *>("channelTile")) {
				if (tile->property("platform").toString() == "custom-rtmp") {
					if (mode.startsWith("add-picker-")) {
						change();
						attemptedAt = now;
					}
					phase = 2;
					tile->click();
					return;
				}
			}
		}
		if (phase == 2 && dialog && dialog->objectName() == "manualChannelDialog") {
			auto fields = dialog->findChildren<QLineEdit *>();
			if (fields.size() != 3) {
				check(false, "actual manual form has three fields");
				finish();
				return;
			}
			if (mode.startsWith("add-form-")) {
				change();
				attemptedAt = now;
			}
			fields[0]->setText("Stale modal destination");
			fields[1]->setText("rtmp://127.0.0.1:19999/private");
			fields[2]->setText(QString::fromUtf8(canary));
			phase = 3;
			auto *save = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save);
			QTimer::singleShot(0, save, &QPushButton::click);
			return;
		}
		if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
			if (phase == 1 && mode.startsWith("remove-")) {
				change();
				attemptedAt = now;
				phase = 3;
				auto *yes = box->button(QMessageBox::Yes);
				check(yes, "actual Remove confirmation exposes Yes");
				if (yes) {
					QTimer::singleShot(0, yes, &QAbstractButton::click);
				}
				return;
			}
			warning = box->text().contains("changed", Qt::CaseInsensitive);
			box->grab().save(root + "/modal-state-warning.png");
			QTimer::singleShot(0, box, &QDialog::accept);
		}
		if (attemptedAt && now - attemptedAt > 1800) {
			check(warning, "modal state change is explicitly rejected");
			check(!progressSeen, "stale Add/Remove does not dispatch credential transaction");
			check(config_get_int(config, "MultistreamChannels", "Count") == 1,
			      "stale Add/Remove preserves original destination count");
			check(QString::fromUtf8(config_get_string(config, "MultistreamChannels", "Channel0.Id")) ==
				      "qa-manual-id",
			      "original destination identity survives stale modal");
			check(window->findChildren<QLabel *>("outputsRowName").size() == 1,
			      "original runtime card survives stale modal");
			if (mode.endsWith("metadata")) {
				check(QString::fromUtf8(config_get_string(config, "MultistreamChannels",
									  "Channel0.DisplayName")) ==
					      "External modal metadata",
				      "external destination metadata survives modal");
				check(!config_get_bool(config, "MultistreamChannels", "Channel0.Enabled"),
				      "external enabled intent survives modal");
			} else {
				check(QString::fromUtf8(config_get_string(config, "Basic", "ProfileDir")) ==
					      "Externally_Changed_Profile",
				      "external profile scope is not changed back");
			}
			finish();
		}
	}
}
void start()
{
	root = qEnvironmentVariable("OTAKUDOBS_PRIVATE_CREDENTIAL_UI_ROOT");
	mode = qEnvironmentVariable("OTAKUDOBS_PRIVATE_CREDENTIAL_UI_MODE");
	const QString nonce = qEnvironmentVariable("OTAKUDOBS_PRIVATE_CREDENTIAL_UI_NONCE");
	auto *config = obs_frontend_get_user_config();
	if (root.isEmpty() || qEnvironmentVariable("XDG_CONFIG_HOME") != root + "/config" || !config ||
	    !QRegularExpression("^[0-9a-fA-F]{32}$").match(nonce).hasMatch() ||
	    QString::fromUtf8(config_get_string(config, "PrivateCredentialUI", "Nonce")) != nonce) {
		return;
	}
	if (!QRegularExpression("^(add-(picker|form)|remove)-(profile|metadata)$").match(mode).hasMatch()) {
		return;
	}
	if (config_get_int(config, "MultistreamChannels", "Count") != 1 ||
	    QString::fromUtf8(config_get_string(config, "MultistreamChannels", "Channel0.Id")) != "qa-manual-id") {
		return;
	}
	window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	auto *events = new Events;
	events->setParent(qApp);
	qApp->installEventFilter(events);
	timer = new QTimer(window);
	timer->setInterval(25);
	QObject::connect(timer, &QTimer::timeout, tick);
	clock.start();
	timer->start();
}
void event(obs_frontend_event e, void *)
{
	if (e == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		QTimer::singleShot(500, qApp, start);
	}
}
} // namespace
bool obs_module_load()
{
	if (qEnvironmentVariableIsEmpty("OTAKUDOBS_PRIVATE_CREDENTIAL_UI_ROOT")) {
		return false;
	}
	obs_frontend_add_event_callback(event, nullptr);
	return true;
}
void obs_module_unload()
{
	obs_frontend_remove_event_callback(event, nullptr);
}
