#include "StudioEffectControls.hpp"
#include <widgets/OBSBasic.hpp>
#include <QCheckBox>
#include <QComboBox>
#include <QDial>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>
#include <algorithm>

bool StudioEffectControls::Supports(const char *id)
{
	return strcmp(id, "compressor_filter") == 0 || strcmp(id, "studio_gain_filter") == 0 ||
	       strcmp(id, "studio_pan_filter") == 0 || strcmp(id, "studio_voice_filter") == 0;
}

StudioEffectControls::StudioEffectControls(obs_source_t *filter,
	std::function<void(const std::string &, const std::string &)> commit_, QWidget *parent)
	: QWidget(parent), weakFilter(OBSGetWeakRef(filter)), commit(std::move(commit_))
{
	setObjectName("studioCompactEffectControls");
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 4, 0, 0); layout->setSpacing(8);
	pending = new QTimer(this); pending->setSingleShot(true); pending->setInterval(350);
	connect(pending, &QTimer::timeout, this, &StudioEffectControls::Flush);
	const QByteArray id = obs_source_get_id(filter);
	if (id == "compressor_filter") {
		auto *row = new QHBoxLayout;
		auto *label = new QLabel(QTStr("Studio.EffectSidechain"), this); label->setFixedWidth(88);
		auto *sidechain = new QComboBox(this); sidechain->setObjectName("studioEffectSidechain");
		row->addWidget(label); row->addWidget(sidechain, 1); layout->addLayout(row);
		readers.emplace_back([sidechain, parent = OBSWeakSource(OBSGetWeakRef(obs_filter_get_parent(filter)))](obs_data_t *settings) {
			const QSignalBlocker block(sidechain);
			const QString selected = QString::fromUtf8(obs_data_get_string(settings, "sidechain_source"));
			sidechain->clear(); sidechain->addItem(QTStr("None"), QStringLiteral("none"));
			struct Items { QComboBox *combo; OBSSource parent; } items{sidechain, OBSGetStrongRef(parent)};
			obs_enum_sources([](void *opaque, obs_source_t *source) {
				auto &items = *static_cast<Items *>(opaque);
				if (source != items.parent.Get() && (obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO))
					items.combo->addItem(QString::fromUtf8(obs_source_get_name(source)), QString::fromUtf8(obs_source_get_name(source)));
				return true;
			}, &items);
			int index = sidechain->findData(selected);
			if (index < 0 && !selected.isEmpty() && selected != "none") {
				sidechain->addItem(selected + QStringLiteral(" — ") + QTStr("Studio.EffectUnavailable"), selected);
				index = sidechain->count() - 1;
			}
			sidechain->setCurrentIndex(std::max(0, index));
		});
		connect(sidechain, &QComboBox::activated, this, [this, sidechain]() {
			Flush();
			OBSSource filter = OBSGetStrongRef(weakFilter);
			if (!filter || !obs_filter_get_parent(filter)) return;
			OBSDataAutoRelease settings = obs_source_get_settings(filter);
			const std::string old = obs_data_get_json(settings);
			OBSDataAutoRelease patch = obs_data_create();
			obs_data_set_string(patch, "sidechain_source", sidechain->currentData().toString().toUtf8().constData());
			obs_source_update(filter, patch);
			commit(old, obs_data_get_json(settings)); observed.clear(); Refresh();
		});
		auto *line = new QFrame(this); line->setFrameShape(QFrame::HLine); layout->addWidget(line);
		Number("threshold", QTStr("Studio.EffectThreshold"), -60, 0, .1, 2, 1, QString(), false);
		Number("ratio", QTStr("Studio.EffectRatio"), 1, 32, .1, 2, 1, QStringLiteral(":1"));
		Number("attack_time", QTStr("Studio.EffectAttack"), .001, .5, .001, 3, .001, QStringLiteral(" s"), true);
		Number("release_time", QTStr("Studio.EffectRelease"), .001, 1, .001, 3, .001, QStringLiteral(" s"), true);
		Number("knee", QStringLiteral("Knee"), 0, 40, .1, 2, 1, QStringLiteral(" dB"));
		Number("output_gain", QTStr("Studio.EffectOutputGain"), -32, 32, .1, 2, 1, QStringLiteral(" dB"));
	} else if (id == "studio_gain_filter") {
		Dial("gain", QTStr("Studio.EffectGain"), false);
	} else if (id == "studio_pan_filter") {
		Dial("pan", QTStr("Studio.EffectPan"), true);
	} else if (id == "studio_voice_filter") {
		auto *description = new QLabel(QTStr("Studio.EffectVoiceHint"), this);
		description->setWordWrap(true); layout->addWidget(description);
		auto *row = new QHBoxLayout;
		row->addWidget(new QLabel(QTStr("Studio.EffectVoiceLevel"), this)); row->addStretch();
		auto *level = new QComboBox(this); level->setObjectName("studioEffectVoiceLevel");
		level->addItems({QTStr("Studio.EffectLow"), QTStr("Studio.EffectModerate"), QTStr("Studio.EffectHigh")});
		row->addWidget(level); layout->addLayout(row);
		auto *agc = new QCheckBox(QTStr("Studio.EffectAGC"), this); agc->setObjectName("studioEffectAGC"); layout->addWidget(agc);
		readers.emplace_back([level, agc](obs_data_t *settings) {
			const QSignalBlocker a(level), b(agc);
			level->setCurrentIndex(std::clamp(int(obs_data_get_int(settings, "level")), 0, 2));
			agc->setChecked(obs_data_get_bool(settings, "enable_agc"));
		});
		connect(level, &QComboBox::activated, this, [this](int level) { Update("level", level, true); });
		connect(agc, &QCheckBox::toggled, this, [this](bool on) { Flush(); Update("enable_agc", on, true); });
	}
	Refresh();
	auto *poll = new QTimer(this);
	connect(poll, &QTimer::timeout, this, &StudioEffectControls::Refresh); poll->start(150);
}

StudioEffectControls::~StudioEffectControls() { Flush(); }

void StudioEffectControls::Update(const char *key, double value, bool integer)
{
	OBSSource filter = OBSGetStrongRef(weakFilter);
	if (!filter || !obs_filter_get_parent(filter)) return;
	OBSDataAutoRelease settings = obs_source_get_settings(filter);
	if (!editing) { before = obs_data_get_json(settings); editing = true; }
	OBSDataAutoRelease patch = obs_data_create();
	if (strcmp(key, "enable_agc") == 0) obs_data_set_bool(patch, key, value != 0);
	else if (integer) obs_data_set_int(patch, key, std::llround(value));
	else obs_data_set_double(patch, key, value);
	obs_source_update(filter, patch);
	observed = obs_data_get_json(settings); pending->start();
}

void StudioEffectControls::Flush()
{
	pending->stop();
	if (!editing) return;
	editing = false;
	OBSSource filter = OBSGetStrongRef(weakFilter);
	if (!filter || !obs_filter_get_parent(filter)) return;
	OBSDataAutoRelease settings = obs_source_get_settings(filter);
	const std::string after = obs_data_get_json(settings);
	if (before != after) commit(before, after);
}

void StudioEffectControls::Refresh()
{
	OBSSource filter = OBSGetStrongRef(weakFilter);
	if (!filter || !obs_filter_get_parent(filter)) return;
	OBSDataAutoRelease settings = obs_source_get_settings(filter);
	const std::string json = obs_data_get_json(settings);
	if (observed == json) return;
	observed = json;
	for (auto &reader : readers) reader(settings);
}

void StudioEffectControls::Reset()
{
	Flush();
	OBSSource filter = OBSGetStrongRef(weakFilter);
	if (!filter || !obs_filter_get_parent(filter)) return;
	OBSDataAutoRelease settings = obs_source_get_settings(filter);
	const std::string old = obs_data_get_json(settings);
	obs_data_clear(settings);
	obs_source_update(filter, nullptr);
	const std::string next = obs_data_get_json(settings);
	if (old != next) commit(old, next);
	observed.clear(); Refresh();
}

void StudioEffectControls::Number(const char *key, const QString &label, double low, double high, double step,
	int decimals, double scale, const QString &suffix, bool integer)
{
	auto *row = new QHBoxLayout; row->setSpacing(8);
	auto *caption = new QLabel(label, this); caption->setFixedWidth(88);
	auto *slider = new QSlider(Qt::Horizontal, this);
	slider->setObjectName(QString::fromUtf8(key) + QStringLiteral("Slider"));
	slider->setRange(0, int(std::round((high - low) / step)));
	auto *spin = new QDoubleSpinBox(this); spin->setObjectName(QString::fromUtf8(key));
	spin->setRange(low, high); spin->setDecimals(decimals); spin->setSingleStep(step); spin->setSuffix(suffix);
	spin->setKeyboardTracking(false); spin->setFixedWidth(86);
	spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
	row->addWidget(caption); row->addWidget(slider, 1); row->addWidget(spin);
	static_cast<QVBoxLayout *>(layout())->addLayout(row);
	readers.emplace_back([spin, slider, key, scale, low, step](obs_data_t *settings) {
		const QSignalBlocker a(spin), b(slider);
		const double value = obs_data_get_double(settings, key) * scale;
		spin->setValue(value); slider->setValue(int(std::round((value - low) / step)));
	});
	connect(slider, &QSlider::valueChanged, spin, [spin, low, step](int value) { spin->setValue(low + value * step); });
	connect(spin, &QDoubleSpinBox::valueChanged, this, [this, spin, slider, key, scale, integer, low, step](double value) {
		const QSignalBlocker block(slider); slider->setValue(int(std::round((value - low) / step)));
		Update(key, value / scale, integer);
	});
}

void StudioEffectControls::Dial(const char *key, const QString &label, bool pan)
{
	auto *box = static_cast<QVBoxLayout *>(layout());
	auto *title = new QLabel(label, this); title->setAlignment(Qt::AlignCenter); box->addWidget(title);
	auto *row = new QHBoxLayout;
	if (pan) row->addWidget(new QLabel(QStringLiteral("L"), this));
	auto *dial = new QDial(this); dial->setObjectName(QString::fromUtf8(key) + QStringLiteral("Dial"));
	dial->setFixedSize(100, 100); dial->setRange(pan ? -100 : 0, pan ? 100 : 400); dial->setWrapping(false);
	auto palette = dial->palette();
	palette.setColor(QPalette::Button, QColor("#2d2d32")); palette.setColor(QPalette::Light, QColor("#62626b"));
	palette.setColor(QPalette::Dark, QColor("#141416")); dial->setPalette(palette);
	row->addStretch(); row->addWidget(dial); row->addStretch();
	if (pan) row->addWidget(new QLabel(QStringLiteral("R"), this));
	box->addLayout(row);
	auto *value = new QDoubleSpinBox(this); value->setObjectName(QString::fromUtf8(key));
	value->setRange(dial->minimum(), dial->maximum()); value->setDecimals(0); value->setSuffix(QStringLiteral(" %"));
	value->setKeyboardTracking(false); value->setFixedWidth(94); box->addWidget(value, 0, Qt::AlignHCenter);
	if (pan && audio_output_get_channels(obs_get_audio()) < 2) {
		dial->setEnabled(false); value->setEnabled(false);
		box->addWidget(new QLabel(QTStr("Studio.EffectStereoRequired"), this));
	}
	readers.emplace_back([dial, value, key](obs_data_t *settings) {
		const QSignalBlocker a(dial), b(value);
		const double percent = obs_data_get_double(settings, key) * 100;
		dial->setValue(int(std::round(percent))); value->setValue(percent);
	});
	connect(dial, &QDial::valueChanged, value, [value](int next) { value->setValue(double(next)); });
	connect(value, &QDoubleSpinBox::valueChanged, this, [this, dial, key](double percent) {
		const QSignalBlocker block(dial); dial->setValue(int(std::round(percent))); Update(key, percent / 100.0);
	});
}
