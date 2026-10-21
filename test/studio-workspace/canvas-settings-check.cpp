/* Opt-in native UI + actual portrait recording test; no public outputs. */
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.hpp>
#include <util/config-file.h>
#include <media-io/video-io.h>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QGroupBox>
#include <QLabel>
#include <QStackedWidget>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidget>
#include <QMainWindow>
#include <QPushButton>
#include <QTimer>
#include <functional>
#include <memory>
#include <stdexcept>
OBS_DECLARE_MODULE()
namespace {
QString root, originalCommon;
QJsonArray checks;
QMainWindow *window = nullptr;
bool done = false, originalMode = true, hadMode = false;
uint64_t originalType = 0, originalInteger = 0, originalNum = 0, originalDen = 0;
OBSEncoderAutoRelease video, audio;
OBSOutputAutoRelease recording;
OBSScene originalPortrait;
OBSSourceAutoRelease color;
obs_sceneitem_t *testItem = nullptr;
QString portraitUuid;
void check(bool pass, const char *name)
{
	checks.append(QJsonObject{{"check", name}, {"passed", pass}});
	if (!pass) {
		throw std::runtime_error(name);
	}
}
OBSCanvas vertical()
{
	OBSCanvas result;
	obs_frontend_canvas_list list{};
	obs_frontend_get_canvases(&list);
	for (size_t i = 0; i < list.canvases.num; ++i) {
		if (QString::fromUtf8(obs_canvas_get_name(list.canvases.array[i])) == "Vertical 9:16") {
			result = list.canvases.array[i];
		}
	}
	obs_frontend_canvas_list_free(&list);
	return result;
}
void restoreConfig()
{
	auto *profile = obs_frontend_get_profile_config();
	config_set_string(profile, "Video", "FPSCommon", originalCommon.toUtf8().constData());
	for (const auto &v : {std::pair{"FPSType", originalType}, std::pair{"FPSInt", originalInteger},
			      std::pair{"FPSNum", originalNum}, std::pair{"FPSDen", originalDen}}) {
		config_set_uint(profile, "Video", v.first, v.second);
	}
	auto *user = obs_frontend_get_user_config();
	if (hadMode) {
		config_set_bool(user, "BasicWindow", "StudioMultiCanvas", originalMode);
	} else {
		config_remove_value(user, "BasicWindow", "StudioMultiCanvas");
	}
	config_save_safe(profile, "tmp", nullptr);
	config_save_safe(user, "tmp", nullptr);
}
void finish(const QString &error = {})
{
	if (done) {
		return;
	}
	done = true;
	if (recording && obs_output_active(recording)) {
		obs_output_force_stop(recording);
	}
	if (auto *dialog = qobject_cast<QDialog *>(qApp->activeModalWidget())) {
		dialog->reject();
	}
	QTimer::singleShot(1300, qApp, [error]() {
		recording = nullptr;
		video = nullptr;
		audio = nullptr;
		if (testItem) {
			obs_sceneitem_remove(testItem);
			testItem = nullptr;
		}
		color = nullptr;
		originalPortrait = nullptr;
		restoreConfig();
		obs_frontend_reset_video();
		QTimer::singleShot(700, qApp, [error]() {
			obs_frontend_save();
			QFile report(root + "/runtime.json");
			if (report.open(QIODevice::WriteOnly)) {
				report.write(
					QJsonDocument(
						QJsonObject{
							{"passed", error.isEmpty()},
							{"error", error},
							{"checks", checks},
							{"original_portrait_scene_uuid", portraitUuid},
							{"scope",
							 "native Settings Apply/Cancel, retained vertical scene/layers, shared 30/60 FPS, real 1080x1920 H.264/AAC files, busy guards; Windows run pending"}})
						.toJson());
			}
		});
	});
}
void later(int ms, std::function<void()> callback)
{
	QTimer::singleShot(ms, qApp, [callback]() {
		if (done) {
			return;
		}
		try {
			callback();
		} catch (const std::exception &error) {
			finish(QString::fromUtf8(error.what()));
		}
	});
}
template<typename T> T *child(QObject *parent, const char *name)
{
	auto *item = parent->findChild<T *>(name);
	check(item != nullptr, name);
	return item;
}
void edit(std::function<void(QDialog *)> callback)
{
	auto completion = std::make_shared<bool>(false);
	later(350, [callback, completion]() {
		auto *dialog = qobject_cast<QDialog *>(qApp->activeModalWidget());
		check(dialog && dialog->objectName() == "OBSBasicSettings", "native settings dialog opens");
		child<QListWidget>(dialog, "listWidget")->setCurrentRow(0);
		callback(dialog);
		child<QDialogButtonBox>(dialog, "buttonBox")->button(QDialogButtonBox::Cancel)->click();
		*completion = true;
	});
	child<QAction>(window, "action_Settings")->trigger();
	check(*completion || done, "settings dialog completes without blocking");
}
void apply(QDialog *dialog)
{
	child<QDialogButtonBox>(dialog, "buttonBox")->button(QDialogButtonBox::Apply)->click();
}
void activate(QComboBox *combo, const QString &text)
{
	const int index = combo->findText(text);
	check(index >= 0, "requested setting exists");
	combo->setCurrentIndex(index);
	check(QMetaObject::invokeMethod(combo, "activated", Qt::DirectConnection, Q_ARG(int, index)),
	      "settings proxy activation handled");
}
void checkFPS(int fps)
{
	obs_video_info main{}, portrait{};
	check(obs_get_video_info(&main) && obs_canvas_get_video_info(vertical(), &portrait),
	      "both canvas video mixes ready");
	check(main.fps_num == unsigned(fps) * main.fps_den && portrait.fps_num == main.fps_num &&
		      portrait.fps_den == main.fps_den,
	      "portrait FPS matches the actual main clock");
	const auto *actual = video_output_get_info(obs_canvas_get_video(vertical()));
	check(actual && actual->fps_num == main.fps_num && actual->fps_den == main.fps_den,
	      "actual video output FPS matches the canvas metadata and main clock");
	check(portrait.base_width == 1080 && portrait.base_height == 1920 && portrait.output_width == 1080 &&
		      portrait.output_height == 1920,
	      "portrait stays 1080x1920");
}
void waitStopped(std::function<void()> next, int attempts = 0)
{
	if (obs_output_active(recording) && attempts < 50) {
		later(100, [next, attempts]() { waitStopped(next, attempts + 1); });
		return;
	}
	check(!obs_output_active(recording), "portrait recording stops");
	recording = nullptr;
	video = nullptr;
	audio = nullptr;
	next();
}
void record(int fps, std::function<void()> next)
{
	checkFPS(fps);
	OBSDataAutoRelease v = obs_data_create();
	obs_data_set_string(v, "rate_control", "CBR");
	obs_data_set_int(v, "bitrate", 2500);
	obs_data_set_int(v, "keyint_sec", 1);
	const bool nvenc = obs_get_encoder_codec("obs_nvenc_h264_tex") != nullptr;
	obs_data_set_string(v, "preset", nvenc ? "p4" : "ultrafast");
	video = obs_video_encoder_create(nvenc ? "obs_nvenc_h264_tex" : "obs_x264", "canvas-test-video", v, nullptr);
	audio = obs_audio_encoder_create("ffmpeg_aac", "canvas-test-audio", nullptr, 0, nullptr);
	check(bool(video) && bool(audio), "real portrait encoders created");
	obs_encoder_set_video(video, obs_canvas_get_video(vertical()));
	obs_encoder_set_audio(audio, obs_get_audio());
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "path", (root + QString("/portrait-%1.mkv").arg(fps)).toUtf8().constData());
	recording = obs_output_create("ffmpeg_muxer", "canvas-settings-local-recording", settings, nullptr);
	check(bool(recording), "local recording output created");
	obs_output_set_video_encoder(recording, video);
	obs_output_set_audio_encoder(recording, audio, 0);
	check(obs_output_start(recording), "real portrait recording starts");
	later(1200, []() {
		edit([](QDialog *dialog) {
			check(!child<QWidget>(dialog, "studioCanvasModes")->isEnabled(),
			      "canvas mode is locked while an encoder is active");
			check(!child<QWidget>(dialog, "studioCanvasVideoFields")->isEnabled(),
			      "resolution and FPS are locked while recording");
		});
	});
	later(5600, [next]() {
		check(obs_output_active(recording) && obs_output_get_total_bytes(recording) > 10000,
		      "portrait recording delivers real media");
		obs_output_stop(recording);
		waitStopped(next);
	});
}
void run()
{
	window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	check(window && !obs_video_active(), "native OBS idle before canvas probe");
	auto *profile = obs_frontend_get_profile_config();
	auto *user = obs_frontend_get_user_config();
	originalCommon = QString::fromUtf8(config_get_string(profile, "Video", "FPSCommon"));
	originalType = config_get_uint(profile, "Video", "FPSType");
	originalInteger = config_get_uint(profile, "Video", "FPSInt");
	originalNum = config_get_uint(profile, "Video", "FPSNum");
	originalDen = config_get_uint(profile, "Video", "FPSDen");
	hadMode = config_has_user_value(user, "BasicWindow", "StudioMultiCanvas");
	originalMode = !hadMode || config_get_bool(user, "BasicWindow", "StudioMultiCanvas");
	if (qEnvironmentVariableIsSet("OBS_STUDIO_CANVAS_PREVIEW_ONLY")) {
		edit([](QDialog *dialog) {
			child<QGroupBox>(dialog, "studioCanvasSettings")->grab().save(root + "/settings.png");
		});
		finish();
		return;
	}
	OBSCanvas canvas = vertical();
	check(bool(canvas), "existing linked portrait canvas found");
	OBSSourceAutoRelease portrait = obs_canvas_get_channel(canvas, 0);
	originalPortrait = portrait ? obs_scene_from_source(portrait) : nullptr;
	check(bool(originalPortrait), "existing portrait scene found");
	portraitUuid = QString::fromUtf8(obs_source_get_uuid(portrait));
	OBSDataAutoRelease c = obs_data_create();
	obs_data_set_int(c, "width", 160);
	obs_data_set_int(c, "height", 160);
	obs_data_set_int(c, "color", 0xff33bb55);
	color = obs_source_create_private("color_source_v3", "canvas-test-green-square", c);
	testItem = obs_scene_add(originalPortrait, color);
	check(testItem != nullptr, "independent vertical layer added");
	vec2 position{40.f, 40.f};
	obs_sceneitem_set_pos(testItem, &position);
	edit([](QDialog *dialog) {
		check(child<QPushButton>(dialog, "studioCanvasMulti")->isChecked(),
		      "Multi Canvas mirrors existing configuration");
		const QString originalBase = child<QComboBox>(dialog, "baseResolution")->currentText();
		activate(child<QComboBox>(dialog, "studioCanvasBaseResolution"), "1280x720");
		check(child<QComboBox>(dialog, "baseResolution")->currentText() == "1280x720",
		      "canvas resolution edits the native OBS video settings");
		child<QPushButton>(dialog, "studioCanvasSingle")->click();
		check(!config_get_bool(obs_frontend_get_user_config(), "BasicWindow", "StudioMultiCanvas"),
		      "canvas mode changes immediately without Apply");
		check(!originalBase.isEmpty(), "original base resolution retained for cancellation");
	});
	if (done) {
		return;
	}
	edit([](QDialog *dialog) {
		check(child<QPushButton>(dialog, "studioCanvasSingle")->isChecked(),
		      "Cancel preserves the immediate canvas action");
		child<QPushButton>(dialog, "studioCanvasSingle")->click();
		apply(dialog);
		OBSSourceAutoRelease inactive = obs_canvas_get_channel(vertical(), 0);
		check(!inactive, "Main Canvas disables vertical program output");
		check(child<QStackedWidget>(window, "verticalPreviewStack")->currentIndex() == 1,
		      "disabled vertical preview does not silently mirror the main program");
		check(child<QLabel>(window, "verticalPreviewHint")->text().contains("Multi Canvas"),
		      "disabled preview explains how to enable Multi Canvas");
		child<QPushButton>(dialog, "studioCanvasMulti")->click();
		apply(dialog);
		OBSSourceAutoRelease current = obs_canvas_get_channel(vertical(), 0);
		check(current && portraitUuid == QString::fromUtf8(obs_source_get_uuid(current)),
		      "re-enabling retains the original portrait scene");
		vec2 pos{};
		obs_sceneitem_get_pos(testItem, &pos);
		check(pos.x == 40.f && pos.y == 40.f, "independent vertical layer transform survives mode changes");
		activate(child<QComboBox>(dialog, "studioCanvasFPS"), "30");
		apply(dialog);
		checkFPS(30);
		child<QGroupBox>(dialog, "studioCanvasSettings")->grab().save(root + "/settings.png");
	});
	if (done) {
		return;
	}
	record(30, []() {
		edit([](QDialog *dialog) {
			activate(child<QComboBox>(dialog, "studioCanvasFPS"), "60");
			apply(dialog);
			checkFPS(60);
		});
		if (!done) {
			record(60, []() { finish(); });
		}
	});
}
void event(obs_frontend_event e, void *)
{
	if (e == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		later(1600, run);
	}
}
} // namespace
bool obs_module_load()
{
	root = qEnvironmentVariable("OBS_STUDIO_CANVAS_TEST_ROOT");
	if (root.isEmpty() || !QDir().mkpath(root)) {
		return false;
	}
	obs_frontend_add_event_callback(event, nullptr);
	return true;
}
void obs_module_unload()
{
	obs_frontend_remove_event_callback(event, nullptr);
	if (recording && obs_output_active(recording)) {
		obs_output_force_stop(recording);
	}
	recording = nullptr;
	video = nullptr;
	audio = nullptr;
	if (testItem) {
		obs_sceneitem_remove(testItem);
	}
	testItem = nullptr;
	color = nullptr;
	originalPortrait = nullptr;
}
