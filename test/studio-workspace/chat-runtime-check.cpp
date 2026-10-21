// Opt-in verification in the installed native OBS. No sources or messages are created.
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QAction>
#include <QCheckBox>
#include <QDockWidget>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMainWindow>
#include <QPushButton>
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("studio-chat-runtime-check", "en-US")
static QString reportPath;
static QTimer *timer = nullptr;
static void run()
{
	QJsonArray checks;
	auto check = [&](bool passed, const QString &name) {
		checks.append(QJsonObject{{"name", name}, {"passed", passed}});
	};
	auto *window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	auto *dock = window ? window->findChild<QDockWidget *>("unifiedChatDock") : nullptr;
	check(dock != nullptr, "unified chat dock exists in installed runtime");
	if (dock) {
		const bool visible = dock->isVisible();
		dock->show();
		for (const char *name :
		     {"unifiedChatSettings", "unifiedChatSearch", "unifiedChatView", "unifiedChatActivity",
		      "unifiedChatResume", "unifiedChatSendPlatform", "unifiedChatSendInput", "unifiedChatSendButton",
		      "unifiedChatDelivery", "unifiedChatNewMessage", "unifiedChatEmojiButton",
		      "unifiedChatActivityStatus"}) {
			check(dock->findChild<QWidget *>(name) != nullptr, QString::fromUtf8(name));
		}
		for (const char *name :
		     {"Compact", "Alternating", "Timestamps", "SplitActivity", "PaidEvents", "MembershipEvents",
		      "SystemEvents", "FollowEvents", "RaidEvents", "PointEvents", "ShowEmotes"}) {
			check(dock->findChild<QAction *>(name) != nullptr, QString::fromUtf8(name));
		}
		auto *scroll = dock->findChild<QCheckBox *>("unifiedChatAutoScroll");
		auto *resume = dock->findChild<QPushButton *>("unifiedChatResume");
		if (scroll && resume) {
			const bool original = scroll->isChecked();
			scroll->setChecked(false);
			check(!resume->isHidden(), "paused feed exposes return to live");
			resume->click();
			check(scroll->isChecked() && resume->isHidden(), "return to live resumes scrolling");
			scroll->setChecked(original);
		}
		auto *options = dock->findChild<QToolButton *>("unifiedChatSettings");
		check(options && options->text() == QString::fromUtf8("Opções"), "Portuguese labels installed");
		dock->grab().save(reportPath + ".png");
		if (!visible) {
			dock->hide();
		}
	}
	bool passed = true;
	for (const auto &item : checks) {
		passed &= item.toObject()["passed"].toBool();
	}
	QFile file(reportPath);
	if (file.open(QIODevice::WriteOnly)) {
		file.write(
			QJsonDocument(QJsonObject{{"passed", passed}, {"checks", checks}, {"public_messages_sent", 0}})
				.toJson());
	}
}
static void event(obs_frontend_event value, void *)
{
	if (value != OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		return;
	}
	timer = new QTimer();
	timer->setSingleShot(true);
	QObject::connect(timer, &QTimer::timeout, run);
	timer->start(1500);
}
bool obs_module_load()
{
	reportPath = qEnvironmentVariable("STUDIO_CHAT_RUNTIME_REPORT");
	if (reportPath.isEmpty()) {
		return false;
	}
	obs_frontend_add_event_callback(event, nullptr);
	return true;
}
void obs_module_unload()
{
	obs_frontend_remove_event_callback(event, nullptr);
	if (timer) {
		timer->stop();
		delete timer;
		timer = nullptr;
	}
}
