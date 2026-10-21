/* Standalone Linux OpenGL exercise. No frontend, user profile or user source is
 * loaded; readback measures the actual optional plugin on synthetic pixels. */
#include <obs.hpp>
#include <obs-nix-platform.h>
#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <wayland-client.h>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <stdexcept>

static constexpr int W = 256, H = 256;
static QJsonArray checks;
static QString root;
static void check(bool condition, const char *name)
{
	checks.append(QJsonObject{{"check", name}, {"passed", condition}});
	if (!condition) {
		throw std::runtime_error(name);
	}
}
static void wait(int duration = 130)
{
	QElapsedTimer timer;
	timer.start();
	while (timer.elapsed() < duration) {
		QCoreApplication::processEvents();
		QThread::msleep(10);
	}
}
static void write(const QString &file, const QJsonObject &object)
{
	QSaveFile output(root + QLatin1Char('/') + file);
	const auto bytes = QJsonDocument(object).toJson();
	if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit()) {
		throw std::runtime_error("cannot write private test report");
	}
}
static QImage frame(obs_source_t *source)
{
	QImage image(W, H, QImage::Format_RGBA8888);
	image.fill(Qt::transparent);
	bool mapped = false;
	obs_enter_graphics();
	auto *render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	auto *stage = gs_stagesurface_create(W, H, GS_RGBA);
	if (render && stage && gs_texrender_begin(render, W, H)) {
		vec4 clear{};
		gs_clear(GS_CLEAR_COLOR, &clear, 0.f, 0);
		gs_ortho(0.f, float(W), 0.f, float(H), -100.f, 100.f);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		obs_source_video_render(source);
		gs_blend_state_pop();
		gs_texrender_end(render);
		gs_stage_texture(stage, gs_texrender_get_texture(render));
		uint8_t *pixels = nullptr;
		uint32_t stride = 0;
		if (gs_stagesurface_map(stage, &pixels, &stride)) {
			for (int y = 0; y < H; ++y) {
				std::copy_n(pixels + y * stride, W * 4, image.scanLine(y));
			}
			gs_stagesurface_unmap(stage);
			mapped = true;
		}
	}
	gs_stagesurface_destroy(stage);
	gs_texrender_destroy(render);
	obs_leave_graphics();
	check(mapped, "GPU texture readback maps successfully");
	return image;
}
static QJsonObject statistics(const QImage &image)
{
	double sum = 0, square = 0, edges = 0;
	int opaque = 0;
	for (int y = 0; y < H; ++y) {
		for (int x = 0; x < W; ++x) {
			const int v = image.pixelColor(x, y).red();
			sum += v;
			square += v * v;
			opaque += image.pixelColor(x, y).alpha() >= 250;
			if (x) {
				edges += std::abs(v - image.pixelColor(x - 1, y).red());
			}
			if (y) {
				edges += std::abs(v - image.pixelColor(x, y - 1).red());
			}
		}
	}
	const double mean = sum / (W * H);
	return {{"mean", mean},
		{"deviation", std::sqrt(std::max(0., square / (W * H) - mean * mean))},
		{"edge_energy", edges / (2 * W * H)},
		{"opaque_pixels", opaque}};
}
static double difference(const QImage &a, const QImage &b)
{
	double result = 0;
	for (int y = 0; y < H; ++y) {
		for (int x = 0; x < W; ++x) {
			result += std::abs(a.pixelColor(x, y).red() - b.pixelColor(x, y).red());
		}
	}
	return result / (W * H);
}
static void loadModule(const QString &binary, const QString &data)
{
	obs_module_t *module = nullptr;
	check(obs_open_module(&module, binary.toUtf8().constData(), data.toUtf8().constData()) == MODULE_SUCCESS,
	      "isolated native module opens");
	check(obs_init_module(module), "isolated native module initializes");
}
int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	if (argc != 5) {
		std::fprintf(stderr, "usage: probe OBS_PREFIX PLUGIN_STAGE EVIDENCE_ROOT wayland|x11\n");
		return 2;
	}
	const QString prefix = QString::fromLocal8Bit(argv[1]), stage = QString::fromLocal8Bit(argv[2]);
	root = QString::fromLocal8Bit(argv[3]);
	QDir().mkpath(root);
	const bool wayland = QString::fromLocal8Bit(argv[4]) == QStringLiteral("wayland");
	wl_display *display = nullptr;
	bool started = false;
	QString error;
	QJsonObject stats, platform;
	try {
		if (wayland) {
			display = wl_display_connect(nullptr);
			check(display, "own Wayland display connection opens");
			obs_set_nix_platform(OBS_NIX_PLATFORM_WAYLAND);
			obs_set_nix_platform_display(display);
		} else {
			obs_set_nix_platform(OBS_NIX_PLATFORM_X11_EGL);
		}
		check(obs_startup("en-US", nullptr, nullptr), "isolated libobs startup succeeds");
		started = true;
		const QByteArray data = (prefix + "/share/obs/libobs").toUtf8();
		obs_add_data_path(data.constData());
		const QByteArray graphics = (prefix + "/lib64/libobs-opengl.so.30").toUtf8();
		obs_video_info video{};
		video.graphics_module = graphics.constData();
		video.fps_num = 60;
		video.fps_den = 1;
		video.base_width = video.output_width = W;
		video.base_height = video.output_height = H;
		video.output_format = VIDEO_FORMAT_RGBA;
		video.colorspace = VIDEO_CS_709;
		video.range = VIDEO_RANGE_FULL;
		video.scale_type = OBS_SCALE_BILINEAR;
		check(obs_reset_video(&video) == OBS_VIDEO_SUCCESS, "native OpenGL video backend initializes");
		obs_enter_graphics();
		platform = {{"device", gs_get_device_name()},
			    {"renderer", gs_get_renderer()},
			    {"platform", wayland ? "Wayland/EGL" : "X11/EGL (XWayland host)"}};
		obs_leave_graphics();
		loadModule(prefix + "/lib64/obs-plugins/image-source.so",
			   prefix + "/share/obs/obs-plugins/image-source");
		loadModule(stage + "/lib64/obs-plugins/obs-composite-blur.so",
			   stage + "/share/obs/obs-plugins/obs-composite-blur");
		QImage pattern(W, H, QImage::Format_RGBA8888);
		for (int y = 0; y < H; ++y) {
			for (int x = 0; x < W; ++x) {
				pattern.setPixelColor(x, y, ((x / 16 + y / 16) % 2) ? Qt::white : Qt::black);
			}
		}
		const QString patternPath = root + "/pattern.png";
		check(pattern.save(patternPath), "synthetic checker pattern is saved");
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_string(settings, "file", patternPath.toUtf8().constData());
		OBSSourceAutoRelease source = obs_source_create(obs_get_latest_input_type_id("image_source"),
								"Own synthetic GPU fixture", settings, nullptr);
		check(bool(source), "isolated image source is created");
		obs_set_output_source(0, source);
		wait(400);
		check(obs_source_get_width(source) == W && obs_source_get_height(source) == H,
		      "synthetic input has expected native size");
		QImage original = frame(source);
		stats["baseline"] = statistics(original);
		check(stats["baseline"].toObject().value("deviation").toDouble() > 120 &&
			      stats["baseline"].toObject().value("opaque_pixels").toInt() == W * H,
		      "baseline GPU pixels preserve opaque high-contrast checker");
		check(original.save(root + "/baseline.png"), "baseline GPU readback is exported");
		OBSDataAutoRelease effect = obs_data_create();
		obs_data_set_int(effect, "blur_algorithm", 1);
		obs_data_set_int(effect, "blur_type", 1);
		obs_data_set_double(effect, "radius", 10.);
		OBSSourceAutoRelease filter =
			obs_source_create("obs_composite_blur", "Own optional blur", effect, nullptr);
		check(bool(filter), "Composite Blur filter creates against real graphics");
		obs_source_filter_add(source, filter);
		QImage gaussian;
		for (int algorithm : {1, 2, 3, 4}) {
			obs_data_set_int(effect, "blur_algorithm", algorithm);
			obs_data_set_int(effect, "passes", 3);
			obs_data_set_double(effect, "kawase_passes", 4.);
			obs_data_set_int(effect, "pixelate_type", 0);
			obs_source_update(filter, effect);
			wait(220);
			QImage filtered = frame(source);
			const auto sample = statistics(filtered);
			stats[QStringLiteral("algorithm_%1").arg(algorithm)] = sample;
			check(sample.value("opaque_pixels").toInt() >= W * H * .98,
			      "filtered GPU output remains opaque");
			check(std::abs(sample.value("mean").toDouble() -
				       stats["baseline"].toObject().value("mean").toDouble()) < 15. &&
				      sample.value("deviation").toDouble() > 1.,
			      "filtered pixels retain brightness and nonconstant detail");
			if (algorithm != 4) {
				check(sample.value("edge_energy").toDouble() <
					      stats["baseline"].toObject().value("edge_energy").toDouble() * .75,
				      "actual blur pixels reduce checker edge energy");
			}
			check(difference(original, filtered) > 5.,
			      "filter changes actual pixels rather than only settings");
			check(filtered.save(root + QStringLiteral("/algorithm-%1.png").arg(algorithm)),
			      "filtered GPU readback is exported");
			if (algorithm == 1) {
				gaussian = filtered;
			}
		}
		obs_source_set_enabled(filter, false);
		wait();
		const auto bypass = frame(source);
		check(difference(original, bypass) <= 1., "bypass restores original GPU pixels");
		obs_data_set_int(effect, "blur_algorithm", 1);
		obs_source_update(filter, effect);
		obs_source_set_enabled(filter, true);
		wait(220);
		const auto beforeSave = frame(source);
		check(difference(gaussian, beforeSave) <= 1., "re-enabled Gaussian produces the same pixels");
		OBSDataAutoRelease saved = obs_save_source(source);
		const QString json = QString::fromUtf8(obs_data_get_json(saved));
		write("source-state.json", QJsonDocument::fromJson(json.toUtf8()).object());
		const QByteArray priorUUID = obs_source_get_uuid(source);
		obs_set_output_source(0, nullptr);
		obs_source_remove(source);
		source = nullptr;
		filter = nullptr;
		wait();
		obs_wait_for_destroy_queue();
		OBSSourceAutoRelease gone = obs_get_source_by_uuid(priorUUID.constData());
		check(!gone, "old source identity is destroyed before native reload");
		OBSDataAutoRelease reloaded = obs_data_create_from_json(json.toUtf8().constData());
		source = obs_load_source(reloaded);
		check(bool(source), "own source and plugin state reload through native serialization");
		obs_set_output_source(0, source);
		wait(400);
		OBSSourceAutoRelease restoredFilter = obs_source_get_filter_by_name(source, "Own optional blur");
		check(bool(restoredFilter), "plugin filter exists after native reload");
		OBSDataAutoRelease restoredSettings = obs_source_get_settings(restoredFilter);
		check(obs_data_get_int(restoredSettings, "blur_algorithm") == 1 &&
			      obs_data_get_double(restoredSettings, "radius") == 10.,
		      "algorithm and radius survive native reload");
		QImage afterSave = frame(source);
		check(difference(beforeSave, afterSave) <= 1., "reloaded plugin reproduces prior GPU pixels");
		check(afterSave.save(root + "/reloaded.png"), "reloaded GPU result is exported");
		obs_set_output_source(0, nullptr);
		obs_source_remove(source);
		source = nullptr;
	} catch (const std::exception &exception) {
		error = QString::fromUtf8(exception.what());
	}
	if (started) {
		obs_set_output_source(0, nullptr);
		obs_shutdown();
	}
	obs_set_nix_platform_display(nullptr);
	if (display) {
		wl_display_disconnect(display);
	}
	write("result.json",
	      {{"passed", error.isEmpty()},
	       {"error", error},
	       {"checks", checks},
	       {"statistics", stats},
	       {"graphics", platform},
	       {"scope",
		"Standalone Linux native GPU textures with synthetic image source; no user profile, physical camera, audio, Windows, output concurrency or timing certification"}});
	std::printf("%s %lld checks %s\n", error.isEmpty() ? "PASS" : "FAIL", (long long)checks.size(),
		    qPrintable(error));
	return error.isEmpty() ? 0 : 1;
}
