/* Optional integration probe, loaded only into the isolated test runtime.
 * It performs no work unless OBS_STUDIO_UI_TEST_REPORT is explicitly set. */
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QApplication>
#include <QAction>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QToolButton>
#include <cmath>
#include <QDialogButtonBox>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListView>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QToolBar>
#include <functional>
#include <stdexcept>

OBS_DECLARE_MODULE()

static QString reportPath;
static QMainWindow *window;
static QJsonArray checks;
static bool ownsRecording = false;
static bool finished = false;
static bool ownsReplay = false;
static QString clipPath;

static void check(bool condition, const char *description)
{
	checks.append(QJsonObject{{"check", description}, {"passed", condition}});
	if (!condition)
		throw std::runtime_error(description);
}

static void finish(const QString &error = {})
{
	if (finished)
		return;
	finished = true;
    if (ownsReplay && obs_frontend_replay_buffer_active())
        obs_frontend_replay_buffer_stop();
	if (ownsRecording && obs_frontend_recording_active())
		obs_frontend_recording_stop();
	QFile file(reportPath);
	if (file.open(QIODevice::WriteOnly))
		file.write(QJsonDocument(QJsonObject{{"passed", error.isEmpty()}, {"error", error}, {"checks", checks}, {"clip_path", clipPath}}).toJson());
}

static void later(int ms, std::function<void()> callback)
{
	QTimer::singleShot(ms, qApp, [callback]() {
		try {
			callback();
		} catch (const std::exception &error) {
			finish(QString::fromUtf8(error.what()));
		}
	});
}

template<typename T> static T *child(const char *name)
{
	auto *result = window->findChild<T *>(QString::fromUtf8(name));
	check(result != nullptr, name);
	return result;
}

static void closeDialog(const char *className)
{
	for (QWidget *widget : QApplication::topLevelWidgets()) {
		if (widget->isVisible() && QByteArray(widget->metaObject()->className()) == className) {
			checks.append(QJsonObject{{"check", className}, {"passed", true}});
			QTimer::singleShot(0, widget, [widget]() {
				for (auto *prompt : widget->findChildren<QMessageBox *>())
					if (prompt->isVisible())
						prompt->done(QMessageBox::Discard);
			});
			/* Cancel through the native dialog so source properties are restored
			 * without a Save/Discard prompt if a device refreshed its defaults. */
			if (auto *buttons = widget->findChild<QDialogButtonBox *>();
			    buttons && buttons->button(QDialogButtonBox::Cancel))
				buttons->button(QDialogButtonBox::Cancel)->click();
			else
				widget->close();
			return;
		}
	}
	check(false, className);
}

static void checkClipping()
{
    auto *buffer = child<QAction>("studioClipBufferAction");
    buffer->trigger();
    ownsReplay = true;
    later(2500, []() {
        check(obs_frontend_replay_buffer_active(), "Clip menu enables native replay buffer");
        child<QToolButton>("studioClipButton")->click();
        later(2500, []() {
            char *path = obs_frontend_get_last_replay();
            clipPath = QString::fromUtf8(path ? path : "");
            bfree(path);
            check(!clipPath.isEmpty() && QFile(clipPath).size() > 0, "Clip button saves a local video file");
            child<QAction>("studioClipBufferAction")->trigger();
            later(1500, []() {
                check(!obs_frontend_replay_buffer_active(), "Clip menu stops replay buffer");
                ownsReplay = false;
                window->grab().save(reportPath + ".png");
                finish();
            });
        });
    });
}

static void runCheck()
{
	window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	check(window != nullptr, "main window");
	check(!obs_frontend_recording_active() && !obs_frontend_streaming_active(), "no active outputs before test");
	auto *action = child<QAction>("studioWorkspaceAction");
	check(action->isChecked(), "Studio enabled at startup");
	check(child<QDockWidget>("sourcesDock")->isVisible(), "layers visible");
	check(child<QDockWidget>("scenesDock")->isVisible(), "scenes visible");
	check(child<QDockWidget>("mixerDock")->isVisible(), "mixer visible");
	check(window->dockWidgetArea(child<QDockWidget>("sourcesDock")) == Qt::LeftDockWidgetArea, "layers on left");
	check(window->dockWidgetArea(child<QDockWidget>("mixerDock")) == Qt::BottomDockWidgetArea, "mixer at bottom");
	check(window->dockWidgetArea(child<QDockWidget>("studioInspectorDock")) == Qt::RightDockWidgetArea, "inspector on right");
	auto *sources = child<QListView>("sources");
	check(sources->model()->rowCount() >= 2, "demo layers loaded");
	sources->selectionModel()->select(sources->model()->index(1, 0), QItemSelectionModel::ClearAndSelect);
	if (qEnvironmentVariableIsSet("OBS_STUDIO_UI_SNAPSHOT_ONLY")) {
		later(800, []() {
			window->showNormal();
			window->resizeDocks({child<QDockWidget>("studioInspectorDock")}, {320}, Qt::Horizontal);
			window->resize(1650, 950);
			later(500, []() {
				window->grab().save(reportPath + ".png");
				check(child<QWidget>("studioInlineProperties")->isVisible(), "embedded properties visible at compact width");
				check(window->width() <= 1650, "compact workspace fits 1650 pixel window");
				window->grab().save(reportPath + ".png");
				finish();
			});
		});
		return;
	}
	later(600, []() {
        check(child<QWidget>("studioInlineProperties")->isVisible(), "source properties embedded in inspector");
        obs_source_t *defaultCamera = obs_get_source_by_name("Studio — webcam C922");
        obs_data_t *defaultSettings = obs_source_get_settings(defaultCamera);
        const bool exposurePreserved = obs_data_get_int(defaultSettings, "Auto Exposure") == 3 &&
            !obs_data_has_user_value(defaultSettings, "Auto Exposure");
        obs_data_release(defaultSettings);
        obs_source_release(defaultCamera);
        check(exposurePreserved, "selecting camera preserves automatic exposure without adding user overrides");
        auto *x = child<QDoubleSpinBox>("studioPositionX");
        const double before = x->value();
        x->setValue(before + 12);
        QMetaObject::invokeMethod(x, "editingFinished", Qt::DirectConnection);
        obs_source_t *sceneSource = obs_frontend_get_current_scene();
        obs_sceneitem_t *cameraItem = obs_scene_find_source(obs_scene_from_source(sceneSource), "Studio — webcam C922");
        vec2 position;
        obs_sceneitem_get_pos(cameraItem, &position);
        check(std::abs(position.x - (before + 12)) < .01, "inline position edits libobs scene item");
        child<QAction>("actionMainUndo")->trigger();
        obs_sceneitem_get_pos(cameraItem, &position);
        check(std::abs(position.x - before) < .01, "inline position supports undo");
        obs_source_release(sceneSource);
        check(child<QPushButton>("studioSourceProperties")->isEnabled(), "inspector follows single selection");
		child<QPushButton>("studioSourceProperties")->click();
		later(700, []() {
			obs_source_t *camera = obs_get_source_by_name("Studio — webcam C922");
			check(camera != nullptr, "native camera available");
			obs_data_t *settings = obs_source_get_settings(camera);
			const bool retained = obs_data_get_int(settings, "framerate") == INT64_C(4294967356);
			obs_data_release(settings);
			obs_source_release(camera);
			check(retained, "opening source properties preserves 64-bit 60 fps selection");
			closeDialog("OBSBasicProperties");
			child<QPushButton>("studioSourceFilters")->click();
			later(400, []() {
				closeDialog("OBSBasicFilters");
				child<QPushButton>("studioSourceTransform")->click();
				later(400, []() {
					closeDialog("OBSBasicTransform");
					auto *sources = child<QListView>("sources");
					sources->selectionModel()->select(sources->model()->index(0, 0), QItemSelectionModel::Select);
					later(400, []() {
						check(!child<QPushButton>("studioSourceProperties")->isEnabled(), "inspector handles multiple selection");
						auto *sources = child<QListView>("sources");
						sources->selectionModel()->select(sources->model()->index(1, 0), QItemSelectionModel::ClearAndSelect);
						auto *action = child<QAction>("studioWorkspaceAction");
						action->setChecked(false);
						check(!child<QToolBar>("studioToolbar")->isVisible(), "toolbar hidden on restore");
						check(child<QDockWidget>("sourcesDock")->isVisible(), "previous layers restored");
						check(child<QDockWidget>("mixerDock")->isVisible(), "previous mixer restored");
						action->setChecked(true);
						check(child<QDockWidget>("studioInspectorDock")->isVisible(), "Studio restored after toggle");
						child<QPushButton>("studioRecordButton")->click();
						ownsRecording = true;
						later(1500, []() {
							check(obs_frontend_recording_active(), "toolbar started recording");
							later(4500, []() {
								child<QPushButton>("studioRecordButton")->click();
								later(1500, []() {
									check(!obs_frontend_recording_active(), "toolbar stopped recording");
									ownsRecording = false;
									checkClipping();
								});
							});
						});
					});
				});
			});
		});
	});
}

static void frontendEvent(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING)
		later(1500, runCheck);
}

bool obs_module_load(void)
{
	reportPath = qEnvironmentVariable("OBS_STUDIO_UI_TEST_REPORT");
	if (reportPath.isEmpty())
		return false;
	obs_frontend_add_event_callback(frontendEvent, nullptr);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(frontendEvent, nullptr);
}
