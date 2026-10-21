/* Opt-in product audit. Exercises local UI only; never starts an output,
 * sends chat, connects an account or changes a source. */
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.hpp>
#include <util/config-file.h>
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidget>
#include <QMainWindow>
#include <QPushButton>
#include <QScrollArea>
#include <QTest>
#include <QTimer>
#include <QToolBar>
#include <functional>
#include <stdexcept>
OBS_DECLARE_MODULE()
namespace {
QString path;
QMainWindow *window;
QByteArray layout;
QJsonArray checks, observations, pages, objects;
bool originalMode, hadMode, captured = false, finished = false;
template<class T> T *get(QObject *owner, const char *name)
{
	auto *object = owner->findChild<T *>(name);
	if (!object) {
		throw std::runtime_error(name);
	}
	return object;
}
void check(bool result, const char *label)
{
	checks.append(QJsonObject{{"check", label}, {"passed", result}});
	if (!result) {
		throw std::runtime_error(label);
	}
}
void click(QPushButton *button)
{
	for (QWidget *p = button->parentWidget(); p; p = p->parentWidget()) {
		if (auto *scroll = qobject_cast<QScrollArea *>(p)) {
			scroll->ensureWidgetVisible(button);
		}
	}
	QTest::mouseClick(button, Qt::LeftButton);
	QTest::qWait(180);
}
QJsonObject state(const char *label)
{
	auto *u = obs_frontend_get_user_config();
	const bool mode = !config_has_user_value(u, "BasicWindow", "StudioMultiCanvas") ||
			  config_get_bool(u, "BasicWindow", "StudioMultiCanvas");
	return {{"step", label},
		{"configured_multi", mode},
		{"portrait_dock_visible", get<QDockWidget>(window, "verticalPreviewDock")->isVisible()}};
}
void edit(std::function<void(QDialog *)> fn)
{
	QString failure;
	QTimer::singleShot(250, qApp, [&]() {
		auto *d = qobject_cast<QDialog *>(qApp->activeModalWidget());
		try {
			check(d && d->objectName() == "OBSBasicSettings", "native settings opens");
			fn(d);
		} catch (const std::exception &e) {
			failure = QString::fromUtf8(e.what());
		}
		if (d) {
			if (auto *box = d->findChild<QDialogButtonBox *>("buttonBox")) {
				box->button(QDialogButtonBox::Cancel)->click();
			} else {
				d->QDialog::reject();
			}
		}
	});
	get<QAction>(window, "action_Settings")->trigger();
	if (!failure.isEmpty()) {
		throw std::runtime_error(failure.toStdString());
	}
}
void finish(QString error = {})
{
	if (finished) {
		return;
	}
	finished = true;
	if (window && captured) {
		auto *u = obs_frontend_get_user_config();
		if (hadMode) {
			config_set_bool(u, "BasicWindow", "StudioMultiCanvas", originalMode);
		} else {
			config_remove_value(u, "BasicWindow", "StudioMultiCanvas");
		}
		config_save_safe(u, "tmp", nullptr);
		obs_frontend_reset_video();
		window->restoreState(layout);
		obs_frontend_save();
	}
	QFile file(path);
	if (file.open(QIODevice::WriteOnly)) {
		file.write(
			QJsonDocument(
				QJsonObject{
					{"audit_completed", error.isEmpty()},
					{"error", error},
					{"checks", checks},
					{"canvas_observations", observations},
					{"settings_pages", pages},
					{"named_ui_objects", objects},
					{"scope",
					 "Native Linux UI navigation, real mouse events for canvas cards, Apply and Cancel; no output or account actions. This reports UX gaps independently of passing mechanics. Native Windows execution pending."}})
				.toJson());
	}
}
void run()
{
	try {
		window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
		check(window && !obs_video_active(), "native audit starts with every encoder idle");
		layout = window->saveState();
		auto *u = obs_frontend_get_user_config();
		hadMode = config_has_user_value(u, "BasicWindow", "StudioMultiCanvas");
		originalMode = !hadMode || config_get_bool(u, "BasicWindow", "StudioMultiCanvas");
		captured = true;
		for (auto *object : window->findChildren<QWidget *>()) {
			if (!object->objectName().startsWith("studio") &&
			    !object->objectName().startsWith("unifiedChat")) {
				continue;
			}
			objects.append(QJsonObject{{"name", object->objectName()},
						   {"class", object->metaObject()->className()},
						   {"visible", object->isVisible()},
						   {"enabled", object->isEnabled()}});
		}
		edit([](QDialog *d) {
			auto *list = get<QListWidget>(d, "listWidget");
			for (int row = 0; row < list->count(); ++row) {
				list->setCurrentRow(row);
				QTest::qWait(30);
				pages.append(
					QJsonObject{{"row", row},
						    {"title", list->item(row)->text()},
						    {"enabled", bool(list->item(row)->flags() & Qt::ItemIsEnabled)}});
			}
			check(list->count() >= 7, "settings category navigation enumerated");
			list->setCurrentRow(0);
			observations.append(state("before_click"));
			click(get<QPushButton>(d, originalMode ? "studioCanvasSingle" : "studioCanvasMulti"));
			observations.append(state("after_opposite_card_click_before_apply"));
			check(state("")["configured_multi"].toBool() != originalMode,
			      "canvas card click changes runtime immediately before Apply");
		});
		check(state("")["configured_multi"].toBool() != originalMode, "Cancel preserves the immediate canvas action");
		edit([](QDialog *d) {
			get<QListWidget>(d, "listWidget")->setCurrentRow(0);
			click(get<QPushButton>(d, "studioCanvasSingle"));
			click(get<QDialogButtonBox>(d, "buttonBox")->button(QDialogButtonBox::Apply));
			observations.append(state("main_after_apply"));
			check(!state("")["configured_multi"].toBool() && !state("")["portrait_dock_visible"].toBool(),
			      "Main Apply changes mode and hides portrait");
			click(get<QPushButton>(d, "studioCanvasMulti"));
			observations.append(state("multi_card_clicked_before_apply"));
			check(state("")["configured_multi"].toBool() && state("")["portrait_dock_visible"].toBool(),
			      "Multi click reveals portrait immediately before Apply");
			click(get<QDialogButtonBox>(d, "buttonBox")->button(QDialogButtonBox::Apply));
			observations.append(state("multi_after_apply"));
			check(state("")["configured_multi"].toBool() && state("")["portrait_dock_visible"].toBool(),
			      "Multi Apply changes mode and reveals portrait");
			if (!originalMode) {
				click(get<QPushButton>(d, "studioCanvasSingle"));
				click(get<QDialogButtonBox>(d, "buttonBox")->button(QDialogButtonBox::Apply));
			}
		});
		get<QAction>(window, "studioOutputsAction")->trigger();
		QTest::qWait(100);
		check(get<QDockWidget>(window, "outputsDock")->isVisible(),
		      "multistream navigation reveals outputs dock");
		if (originalMode) {
			check(get<QDockWidget>(window, "verticalPreviewDock")->isVisible(),
			      "multistream navigation reveals vertical preview in Multi mode");
		}
		click(get<QPushButton>(window, "studioAudioRoutingButton"));
		check(get<QWidget>(window, "studioAudioGain")->isVisible(),
		      "audio navigation reveals actual gain control");
		auto *sidebar = get<QToolBar>(window, "studioSidebar");
		for (auto *action : sidebar->actions()) {
			if (action->text() == QString::fromUtf8("▤")) {
				action->trigger();
			}
		}
		QTest::qWait(100);
		check(get<QDockWidget>(window, "unifiedChatDock")->isVisible(),
		      "chat navigation reveals native chat dock");
		check(get<QWidget>(window, "unifiedChatSendButton") != nullptr,
		      "chat composer controls exist without sending a message");
		get<QAction>(window, "studioClipLibraryAction")->trigger();
		QTest::qWait(100);
		check(get<QWidget>(window, "studioClipLibrary")->isVisible(),
		      "clip library navigation works without starting replay");
		QTimer::singleShot(200, qApp, []() {
			if (auto *d = qobject_cast<QDialog *>(qApp->activeModalWidget())) {
				d->reject();
			}
		});
		get<QAction>(window, "studioWidgetAddAction")->trigger();
		check(!obs_video_active(), "UI audit leaves encoders idle");
		finish();
	} catch (const std::exception &e) {
		finish(QString::fromUtf8(e.what()));
	}
}
void event(obs_frontend_event e, void *)
{
	if (e == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		QTimer::singleShot(1500, qApp, run);
	}
}
} // namespace
bool obs_module_load()
{
	path = qEnvironmentVariable("OBS_STUDIO_BRAIN_AUDIT_REPORT");
	if (path.isEmpty()) {
		return false;
	}
	obs_frontend_add_event_callback(event, nullptr);
	return true;
}
void obs_module_unload()
{
	obs_frontend_remove_event_callback(event, nullptr);
}
