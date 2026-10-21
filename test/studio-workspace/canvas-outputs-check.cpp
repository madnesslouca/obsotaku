/* Opt-in real native recording/replay UI probe. Uses local generated media only. */
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.hpp>
#include <utility/MultiStreamManager.hpp>
#include <utility/MultistreamChannelStore.hpp>
#include <util/config-file.h>
#include <util/bmem.h>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGroupBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>
OBS_DECLARE_MODULE()
bool MultistreamChannelStore::SetEnabled(const std::string &, bool, std::string &error)
{
	error = "The canvas probe cannot persist public destinations";
	return false;
}
namespace {
QString root;
QMainWindow *window;
QJsonArray checks, files;
OBSSourceAutoRelease original, media;
OBSSceneAutoRelease fixture;
OBSScene portraitScene;
obs_sceneitem_t *marker = nullptr;
bool done = false, studioMode = false;
int phase = 0;
struct Saved {
	const char *section, *key;
	bool exists;
	QString value;
};
std::vector<Saved> saved;
void check(bool pass, const char *label)
{
	checks.append(QJsonObject{{"check", label}, {"passed", pass}, {"phase", phase}});
	if (!pass) {
		throw std::runtime_error(label);
	}
}
void saveKey(const char *section, const char *key)
{
	auto *c = obs_frontend_get_profile_config();
	saved.push_back({section, key, config_has_user_value(c, section, key),
			 QString::fromUtf8(config_get_string(c, section, key))});
}
void restoreConfig()
{
	auto *c = obs_frontend_get_profile_config();
	for (const auto &v : saved) {
		if (v.exists) {
			config_set_string(c, v.section, v.key, v.value.toUtf8().constData());
		} else {
			config_remove_value(c, v.section, v.key);
		}
	}
	config_save_safe(c, "tmp", nullptr);
}
void finish(QString error = {});
void later(int delay, std::function<void()> fn)
{
	QTimer::singleShot(delay, qApp, [fn]() {
		if (done) {
			return;
		}
		try {
			fn();
		} catch (const std::exception &e) {
			finish(QString::fromUtf8(e.what()));
		}
	});
}
template<class T> T *child(QObject *p, const char *name)
{
	auto *w = p->findChild<T *>(name);
	check(w != nullptr, name);
	return w;
}
void edit(std::function<void(QDialog *)> fn, bool apply = true)
{
	later(300, [fn, apply]() {
		auto *d = qobject_cast<QDialog *>(qApp->activeModalWidget());
		check(d && d->objectName() == "OBSBasicSettings", "native Settings dialog opens");
		child<QListWidget>(d, "listWidget")->setCurrentRow(0);
		fn(d);
		if (apply) {
			child<QDialogButtonBox>(d, "buttonBox")->button(QDialogButtonBox::Apply)->click();
		}
		child<QDialogButtonBox>(d, "buttonBox")->button(QDialogButtonBox::Cancel)->click();
	});
	child<QAction>(window, "action_Settings")->trigger();
}
void choice(QDialog *d, const char *name, const char *value)
{
	auto *combo = child<QComboBox>(d, name);
	const int index = combo->findData(value);
	check(index >= 0, "canvas selection is available");
	combo->setCurrentIndex(index);
}
OBSCanvas vertical()
{
	OBSCanvas canvas;
	obs_frontend_canvas_list list{};
	obs_frontend_get_canvases(&list);
	for (size_t i = 0; i < list.canvases.num; ++i) {
		if (QString::fromUtf8(obs_canvas_get_name(list.canvases.array[i])) == "Vertical 9:16") {
			canvas = list.canvases.array[i];
		}
	}
	obs_frontend_canvas_list_free(&list);
	return canvas;
}
void report(const QString &error)
{
	QFile f(root + "/runtime.json");
	if (f.open(QIODevice::WriteOnly)) {
		f.write(QJsonDocument(
				QJsonObject{
					{"passed", error.isEmpty()},
					{"error", error},
					{"checks", checks},
					{"files", files},
					{"scope",
					 "Native Settings Apply/Cancel; Simple/Advanced handlers; simultaneous main+portrait with audio; native clip save and library; pause and stop; Windows native run pending"}})
				.toJson());
	}
}
void finish(QString error)
{
	if (done) {
		return;
	}
	done = true;
	if (auto *d = qobject_cast<QDialog *>(qApp->activeModalWidget())) {
		d->reject();
	}
	if (obs_frontend_recording_active()) {
		obs_frontend_recording_stop();
	}
	if (obs_frontend_replay_buffer_active()) {
		obs_frontend_replay_buffer_stop();
	}
	QTimer::singleShot(1800, qApp, [error]() {
		if (marker) {
			obs_sceneitem_remove(marker);
			marker = nullptr;
		}
		portraitScene = nullptr;
		if (original) {
			obs_frontend_set_current_scene(original);
		}
		if (fixture) {
			obs_source_remove(obs_scene_get_source(fixture));
		}
		fixture = nullptr;
		if (media) {
			obs_source_remove(media);
		}
		media = nullptr;
		restoreConfig();
		if (studioMode) {
			obs_frontend_set_preview_program_mode(true);
		}
		obs_frontend_save();
		report(error);
	});
}
void waitIdle(std::function<void()> next, int attempt = 0)
{
	later(150, [next, attempt]() {
		OBSOutputAutoRelease extra = obs_get_output_by_name("studio_portrait_recording");
		if (obs_frontend_recording_active() || obs_frontend_replay_buffer_active() ||
		    (extra && obs_output_active(extra))) {
			check(attempt < 80, "all canvas outputs stop within deadline");
			waitIdle(next, attempt + 1);
		} else {
			next();
		}
	});
}
void phaseRun();
void stress(int iteration = 0);
void saveClip()
{
	obs_frontend_replay_buffer_save();
	later(1600, []() {
		char *path = obs_frontend_get_last_replay();
		const QString clip = QString::fromUtf8(path);
		bfree(path);
		check(!clip.isEmpty() && QFileInfo(clip).size() > 10000, "native replay Save writes a clip");
		OBSOutputAutoRelease replay = obs_frontend_get_replay_buffer_output();
		auto *encoder = obs_output_get_video_encoder(replay);
		files.append(QJsonObject{{"path", clip},
					 {"kind", "clip"},
					 {"phase", phase},
					 {"width", int(obs_encoder_get_width(encoder))},
					 {"height", int(obs_encoder_get_height(encoder))}});
		const QString configRoot = qEnvironmentVariable("XDG_CONFIG_HOME");
		QFile library(configRoot + "/obs-studio/plugin_config/studio-clips.json");
		check(library.open(QIODevice::ReadOnly), "native clip library can be read");
		check(library.readAll().contains(clip.toUtf8()),
		      "saved clip is registered in the existing native library");
		char *recordingPath = obs_frontend_get_last_recording();
		const QString recording = QString::fromUtf8(recordingPath);
		bfree(recordingPath);
		OBSOutputAutoRelease output = obs_frontend_get_recording_output();
		files.append(
			QJsonObject{{"path", recording},
				    {"kind", "recording"},
				    {"phase", phase},
				    {"width", int(obs_encoder_get_width(obs_output_get_video_encoder(output)))},
				    {"height", int(obs_encoder_get_height(obs_output_get_video_encoder(output)))}});
		OBSOutputAutoRelease extra = obs_get_output_by_name("studio_portrait_recording");
		if (extra && obs_output_active(extra)) {
			OBSDataAutoRelease settings = obs_output_get_settings(extra);
			files.append(QJsonObject{{"path", QString::fromUtf8(obs_data_get_string(settings, "path"))},
						 {"kind", "companion"},
						 {"phase", phase},
						 {"width", 1080},
						 {"height", 1920}});
		}
		obs_frontend_recording_stop();
		obs_frontend_replay_buffer_stop();
		waitIdle([]() {
			check(!obs_video_active(), "all encoders idle after native Stop");
			if (++phase < 4) {
				phaseRun();
			} else {
				stress();
			}
		});
	});
}
void stress(int iteration)
{
	if (iteration == 10) { finish(); return; }
	phase = 4;
	if (iteration == 0) {
		QDir().mkpath(root + "/stress");
		edit([](QDialog *d) {
			child<QLineEdit>(d, "simpleOutputPath")->setText(root + "/stress");
			choice(d, "studioCanvasRecordingCanvas", "portrait");
			choice(d, "studioCanvasReplayCanvas", "portrait");
		});
	}
	obs_frontend_recording_start();
	obs_frontend_replay_buffer_start();
	later(1000, [iteration]() {
		check(obs_frontend_recording_active() && obs_frontend_replay_buffer_active(), "stress: two independent encoders start on the same portrait mix");
		obs_frontend_recording_stop();
		obs_frontend_replay_buffer_stop();
		waitIdle([iteration]() {
			check(!obs_video_active(), "stress: concurrent portrait stops fully deactivate both encoders");
			later(100, [iteration]() { stress(iteration + 1); });
		});
	});
}

void phaseRun()
{
	auto *c = obs_frontend_get_profile_config();
	const QString directory = root + QString("/phase-%1").arg(phase);
	QDir().mkpath(directory);
	if (phase == 2) {
		config_set_string(c, "AdvOut", "RecFilePath", directory.toUtf8().constData());
		config_set_string(c, "AdvOut", "Encoder",
				  obs_get_encoder_codec("obs_nvenc_h264_tex") ? "obs_nvenc_h264_tex" : "obs_x264");
		config_set_string(c, "AdvOut", "RecEncoder", "none");
		config_set_string(c, "AdvOut", "RecFormat2", "mkv");
		config_set_bool(c, "AdvOut", "RecRB", true);
		config_set_int(c, "AdvOut", "RecRBTime", 5);
		config_set_int(c, "AdvOut", "RecTracks", 1);
	}
	edit([directory](QDialog *d) {
		child<QComboBox>(d, "outputMode")->setCurrentIndex(phase == 2 ? 1 : 0);
		child<QLineEdit>(d, "simpleOutputPath")->setText(directory);
		child<QSpinBox>(d, "studioCanvasMainBitrate")->setValue(7200);
		child<QSpinBox>(d, "studioCanvasPortraitBitrate")->setValue(4800);
		child<QSpinBox>(d, "studioCanvasPortraitRecordingBitrate")->setValue(3600 + phase * 100);
		choice(d, "studioCanvasRecordingCanvas",
		       phase == 0 || phase == 2 ? "both"
		       : phase == 1             ? "portrait"
						: "main");
		choice(d, "studioCanvasReplayCanvas", phase == 0 || phase == 3 ? "main" : "portrait");
		if (phase == 0) {
			child<QGroupBox>(d, "studioCanvasSettings")->grab().save(root + "/settings.png");
		}
	});
	if (done) {
		return;
	}
	check(config_get_int(c, "StudioCanvas", "PortraitRecordingBitrate") == 3600 + phase * 100,
	      "portrait bitrate persists through native Apply");
	check(config_get_int(c, "SimpleOutput", "VBitrate") == 7200, "main stream bitrate persists independently");
	edit(
		[](QDialog *d) {
			check(child<QSpinBox>(d, "studioCanvasPortraitBitrate")->value() == 4800,
			      "portrait stream bitrate survives reopening Settings");
			check(child<QSpinBox>(d, "studioCanvasPortraitRecordingBitrate")->value() == 3600 + phase * 100,
			      "recording bitrate survives reopening Settings");
		},
		false);
	obs_frontend_recording_start();
	obs_frontend_replay_buffer_start();
	later(1800, []() {
		check(obs_frontend_recording_active() && obs_frontend_replay_buffer_active(),
		      "native Record and Replay buttons start both output paths");
		OBSOutputAutoRelease record = obs_frontend_get_recording_output();
		OBSOutputAutoRelease replay = obs_frontend_get_replay_buffer_output();
		auto *recEncoder = obs_output_get_video_encoder(record);
		auto *clipEncoder = obs_output_get_video_encoder(replay);
		check(obs_encoder_get_width(recEncoder) == (phase == 1 ? 1080u : 1920u),
		      "recording uses the chosen canvas");
		check(obs_encoder_get_width(clipEncoder) == (phase == 1 || phase == 2 ? 1080u : 1920u),
		      "clip independently uses the chosen canvas");
		check(obs_output_get_audio_encoder(record, 0) && obs_output_get_audio_encoder(replay, 0),
		      "recording and clips both carry the native audio selection");
		if (phase == 0 || phase == 2) {
			OBSOutputAutoRelease extra = obs_get_output_by_name("studio_portrait_recording");
			check(extra && obs_output_active(extra), "second portrait file records simultaneously");
			OBSDataAutoRelease v = obs_encoder_get_settings(obs_output_get_video_encoder(extra));
			check(obs_data_get_int(v, "bitrate") == 3600 + phase * 100,
			      "portrait encoder uses its separate bitrate");
			check(obs_encoder_get_mixer_index(obs_output_get_audio_encoder(extra, 0)) ==
				      obs_encoder_get_mixer_index(obs_output_get_audio_encoder(record, 0)),
			      "both files preserve the same selected audio mix with independently pausable encoders");
			OBSDataAutoRelease a = obs_output_get_settings(record), b = obs_output_get_settings(extra);
			check(QString::fromUtf8(obs_data_get_string(a, "path")) !=
				      QString::fromUtf8(obs_data_get_string(b, "path")),
			      "cloning settings never overwrites the main recording file path");
		}
		edit(
			[](QDialog *d) {
				check(!child<QWidget>(d, "studioCanvasOutputFields")->isEnabled(),
				      "canvas choices and bitrates are locked during recording");
			},
			false);
		if (phase == 0) {
			obs_frontend_recording_pause(true);
			later(400, []() {
				OBSOutputAutoRelease extra = obs_get_output_by_name("studio_portrait_recording");
				check(obs_frontend_recording_paused() && obs_output_paused(extra),
				      "native Pause pauses both recording files");
				obs_frontend_recording_pause(false);
				later(300, []() {
					OBSOutputAutoRelease extra =
						obs_get_output_by_name("studio_portrait_recording");
					check(!obs_frontend_recording_paused() && !obs_output_paused(extra),
					      "native Resume resumes both recording files");
				});
			});
		}
	});
	later(6500, saveClip);
}
void run()
{
	window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	check(window && !obs_video_active(), "OBS starts idle before native dual-recording probe");
	for (const auto &v :
	     {std::pair{"Output", "Mode"}, std::pair{"SimpleOutput", "FilePath"}, std::pair{"SimpleOutput", "VBitrate"},
	      std::pair{"StudioCanvas", "PortraitBitrate"}, std::pair{"StudioCanvas", "PortraitRecordingBitrate"},
	      std::pair{"StudioCanvas", "RecordingCanvas"}, std::pair{"StudioCanvas", "ReplayCanvas"},
	      std::pair{"AdvOut", "RecFilePath"}, std::pair{"AdvOut", "Encoder"}, std::pair{"AdvOut", "RecEncoder"},
	      std::pair{"AdvOut", "RecFormat2"}, std::pair{"AdvOut", "RecRB"}, std::pair{"AdvOut", "RecRBTime"},
	      std::pair{"AdvOut", "RecTracks"}}) {
		saveKey(v.first, v.second);
	}
	original = obs_frontend_get_current_scene();
	studioMode = obs_frontend_preview_program_mode_active();
	if (studioMode) {
		obs_frontend_set_preview_program_mode(false);
	}
	fixture = obs_scene_create("Canvas outputs QA — local media");
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_bool(settings, "is_local_file", true);
	obs_data_set_bool(settings, "looping", true);
	obs_data_set_string(settings, "local_file", (root + "/fixture.mp4").toUtf8().constData());
	media = obs_source_create("ffmpeg_source", "Canvas outputs QA — pattern and tone", settings, nullptr);
	check(bool(media) && bool(fixture), "local moving image and synthetic audio scene created");
	obs_source_set_audio_mixers(media, 1);
	obs_scene_add(fixture, media);
	obs_frontend_set_current_scene(obs_scene_get_source(fixture));
	later(900, []() {
		OBSCanvas canvas = vertical();
		check(bool(canvas), "persistent vertical canvas exists");
		OBSSourceAutoRelease source = obs_canvas_get_channel(canvas, 0);
		portraitScene = obs_scene_from_source(source);
		check(bool(portraitScene), "linked portrait scene follows the QA main scene");
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_int(settings, "width", 160);
		obs_data_set_int(settings, "height", 160);
		obs_data_set_int(settings, "color", 0xff33bb55);
		OBSSourceAutoRelease green =
			obs_source_create_private("color_source_v3", "vertical-only QA marker", settings);
		marker = obs_scene_add(portraitScene, green);
		vec2 position{40.f, 40.f};
		obs_sceneitem_set_pos(marker, &position);
		edit(
			[](QDialog *d) {
				child<QSpinBox>(d, "studioCanvasPortraitRecordingBitrate")->setValue(9999);
				choice(d, "studioCanvasRecordingCanvas", "portrait");
			},
			false);
		check(config_get_int(obs_frontend_get_profile_config(), "StudioCanvas", "PortraitRecordingBitrate") !=
			      9999,
		      "Cancel does not persist canvas bitrate edits");
		{
			OBSDataAutoRelease settings = obs_data_create();
			obs_data_set_int(settings, "bitrate", 7200);
			obs_data_set_string(settings, "rate_control", "CBR");
			OBSEncoderAutoRelease main =
				obs_video_encoder_create("obs_x264", "canvas-bitrate-probe", settings, nullptr);
			obs_encoder_set_video(main, obs_get_video());
			MultiStreamManager manager;
			MultiStreamChannel channel;
			channel.id = "canvas-bitrate-local";
			channel.displayName = "Local bitrate probe";
			channel.server = "rtmp://127.0.0.1:1/live";
			channel.streamKey = "local-test";
			channel.videoLayout = MultiStreamVideoLayout::Portrait;
			std::string error;
			check(manager.Configure({channel}, error),
			      "independent stream bitrate probe configures without publishing");
			manager.SetPrimaryChannelId(channel.id);
			auto *portrait = manager.PreparePrimaryVideoEncoder(main, error, 4200);
			check(portrait != nullptr, "primary portrait stream prepares with explicit bitrate");
			OBSDataAutoRelease a = obs_encoder_get_settings(main), b = obs_encoder_get_settings(portrait);
			check(obs_data_get_int(a, "bitrate") == 7200 && obs_data_get_int(b, "bitrate") == 4200,
			      "portrait stream bitrate never mutates the main encoder settings");
		}
		phaseRun();
	});
}
void event(obs_frontend_event value, void *)
{
	if (value == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		later(1700, run);
	}
}
} // namespace
bool obs_module_load()
{
	root = qEnvironmentVariable("OBS_STUDIO_CANVAS_OUTPUTS_TEST_ROOT");
	if (root.isEmpty()) {
		return false;
	}
	obs_frontend_add_event_callback(event, nullptr);
	later(150000, []() { finish("Native canvas output test timed out"); });
	return true;
}
void obs_module_unload()
{
	obs_frontend_remove_event_callback(event, nullptr);
}
