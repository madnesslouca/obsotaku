/* Studio canvas controls share OBS' native settings widgets and Apply/Cancel.
 * Layouts belong to the scene collection; switching canvas mode never deletes them. */
#include "OBSBasicSettings.hpp"
#include <OBSApp.hpp>
#include <utility/DualCanvasController.hpp>
#include <utility/StudioCanvasWorkspace.hpp>
#include <widgets/OBSBasic.hpp>
#include <qt-wrappers.hpp>
#include <QButtonGroup>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QSignalBlocker>
#include <QRegularExpression>
#include <QTimer>
#include <QSpinBox>
#include <algorithm>

namespace {
bool CanvasBusy()
{
	return obs_video_active() || obs_frontend_streaming_active() || obs_frontend_recording_active() ||
	       obs_frontend_replay_buffer_active() || obs_frontend_virtualcam_active();
}
bool StudioResolutionValue(const QString &value)
{
	const auto match = QRegularExpression("^([0-9]{2,5})x([0-9]{2,5})$").match(value.trimmed());
	if (!match.hasMatch()) {
		return false;
	}
	const uint32_t width = match.captured(1).toUInt(), height = match.captured(2).toUInt();
	return width >= 16 && height >= 16 && width <= 16384 && height <= 16384 && !(width & 1) && !(height & 1);
}
void CopyCombo(QComboBox *from, QComboBox *to, const QString &custom = {})
{
	QStringList values;
	for (int i = 0; i < from->count(); ++i) {
		values.append(from->itemText(i));
	}
	const QString current = custom.isEmpty() ? from->currentText() : custom;
	if (!values.contains(current)) {
		values.append(current);
	}
	bool identical = to->count() == values.size();
	for (int i = 0; identical && i < to->count(); ++i) {
		identical = to->itemText(i) == values[i];
	}
	if (identical && to->currentText() == current) {
		return;
	}
	const QSignalBlocker blocked(to);
	if (!identical) {
		to->clear();
		to->addItems(values);
	}
	to->setCurrentText(current);
}
} // namespace

void OBSBasicSettings::InitStudioCanvasSettings()
{
	auto *group = new QGroupBox(QTStr("Studio.Canvas.Title"), ui->generalPage);
	group->setObjectName("studioCanvasSettings");
	auto *layout = new QVBoxLayout(group);
	auto *modes = new QWidget(group);
	modes->setObjectName("studioCanvasModes");
	modes->setMinimumHeight(110);
	modes->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	auto *cards = new QHBoxLayout(modes);
	cards->setContentsMargins(0, 0, 0, 0);
	auto *single = new QPushButton(QTStr("Studio.Canvas.Single"), modes);
	auto *multi = new QPushButton(QTStr("Studio.Canvas.Multi"), modes);
	single->setObjectName("studioCanvasSingle");
	multi->setObjectName("studioCanvasMulti");
	auto *buttons = new QButtonGroup(group);
	for (auto *button : {single, multi}) {
		button->setCheckable(true);
		button->setMinimumHeight(76);
		button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
		buttons->addButton(button);
		cards->addWidget(button);
	}
	group->setStyleSheet(
		"QPushButton#studioCanvasSingle, QPushButton#studioCanvasMulti {text-align: left; padding: 12px; border: 1px solid palette(mid); border-radius: 6px;}"
		"QPushButton#studioCanvasSingle:checked, QPushButton#studioCanvasMulti:checked {border: 2px solid palette(highlight); background: palette(alternate-base);}");
	layout->addWidget(modes);
	auto *description = new QLabel(QTStr("Studio.Canvas.Description"), group);
	description->setObjectName("studioCanvasDescription");
	description->setWordWrap(true);
	layout->addWidget(description);
	auto *immediate = new QLabel(QTStr("Studio.Canvas.Immediate"), group);
	immediate->setWordWrap(true);
	layout->addWidget(immediate);
	auto *modeError = new QLabel(QTStr("Studio.Canvas.ModeFailed"), group);
	modeError->setObjectName("studioCanvasModeError");
	modeError->setWordWrap(true);
	modeError->hide();
	layout->addWidget(modeError);
	auto *fields = new QWidget(group);
	fields->setObjectName("studioCanvasVideoFields");
	auto *form = new QFormLayout(fields);
	form->setContentsMargins(0, 0, 0, 0);
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	for (const auto &entry : {std::pair{ui->baseResolution, "studioCanvasBaseResolution"},
				  std::pair{ui->outputResolution, "studioCanvasOutputResolution"},
				  std::pair{ui->fpsCommon, "studioCanvasFPS"}}) {
		auto *mirror = new QComboBox(fields);
		mirror->setObjectName(entry.second);
		mirror->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
		mirror->setMinimumContentsLength(10);
		if (entry.first == ui->outputResolution) {
			mirror->setToolTip(QTStr("Studio.Canvas.OutputHint"));
		}
		const char *label = entry.first == ui->baseResolution     ? "Studio.Canvas.BaseResolution"
				    : entry.first == ui->outputResolution ? "Studio.Canvas.StreamResolution"
									  : "Studio.Canvas.FPS";
		form->addRow(QTStr(label), mirror);
		connect(mirror, &QComboBox::activated, this, [this, original = entry.first, mirror](int) {
			if (original == ui->outputResolution) {
				if (!loading && !CanvasBusy()) {
					mirror->setProperty("changed", true);
					GeneralChanged();
				}
				return;
			}
			if (CanvasBusy()) {
				UpdateStudioCanvasSettings();
				return;
			}
			const QString selected = mirror->currentText();
			if (original == ui->fpsCommon) {
				if (original->findText(selected) < 0) {
					return;
				}
				ui->fpsType->setCurrentIndex(0);
			}
			original->setCurrentText(selected);
		});
		connect(entry.first, &QComboBox::currentTextChanged, this, [this]() { UpdateStudioCanvasSettings(); });
	}
	auto *mainVideo = new QComboBox(fields);
	mainVideo->setObjectName("studioCanvasMainVideoResolution");
	mainVideo->setToolTip(QTStr("Studio.Preset.MainVideoHint"));
	form->addRow(QTStr("Studio.Preset.MainVideoResolution"), mainVideo);
	connect(mainVideo, &QComboBox::activated, this, [this, mainVideo](int) {
		if (!CanvasBusy()) {
			ui->outputResolution->setCurrentText(mainVideo->currentText());
		}
	});
	layout->addWidget(fields);
	auto *outputs = new QWidget(group);
	outputs->setObjectName("studioCanvasOutputFields");
	auto *outputForm = new QFormLayout(outputs);
	outputForm->setContentsMargins(0, 0, 0, 0);
	outputForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	auto *mainBitrate = new QSpinBox(outputs);
	mainBitrate->setObjectName("studioCanvasMainBitrate");
	mainBitrate->setRange(ui->simpleOutputVBitrate->minimum(), ui->simpleOutputVBitrate->maximum());
	mainBitrate->setSuffix(" Kbps");
	outputForm->addRow(QTStr("Studio.Canvas.MainBitrate"), mainBitrate);
	connect(mainBitrate, &QSpinBox::valueChanged, this, [this](int value) {
		if (!loading && !CanvasBusy()) {
			ui->simpleOutputVBitrate->setValue(value);
		}
	});
	for (const auto &entry : {std::pair{"PortraitBitrate", "Studio.Canvas.PortraitBitrate"},
				  std::pair{"PortraitRecordingBitrate", "Studio.Canvas.RecordingBitrate"}}) {
		auto *spin = new QSpinBox(outputs);
		spin->setObjectName(QString("studioCanvas") + entry.first);
		spin->setRange(100, 100000);
		spin->setSingleStep(100);
		spin->setSuffix(" Kbps");
		outputForm->addRow(QTStr(entry.second), spin);
		connect(spin, &QSpinBox::valueChanged, this, [this, spin]() {
			if (!loading) {
				spin->setProperty("changed", true);
				GeneralChanged();
			}
		});
	}
	for (const auto &entry : {std::pair{"RecordingCanvas", "Studio.Canvas.RecordingCanvas"},
				  std::pair{"ReplayCanvas", "Studio.Canvas.ReplayCanvas"}}) {
		auto *combo = new QComboBox(outputs);
		combo->setObjectName(QString("studioCanvas") + entry.first);
		combo->addItem(QTStr("Studio.Canvas.MainChoice"), "main");
		combo->addItem(QTStr("Studio.Canvas.PortraitChoice"), "portrait");
		combo->addItem(QTStr("Studio.Canvas.BothChoice"), "both");
		outputForm->addRow(QTStr(entry.second), combo);
		connect(combo, &QComboBox::currentIndexChanged, this, [this, combo]() {
			if (!loading) {
				combo->setProperty("changed", true);
				GeneralChanged();
			}
		});
	}
	for (const auto &prefix : {QString("Main"), QString("Portrait")}) {
		auto *source = new QComboBox(outputs);
		source->setObjectName("studioCanvas" + prefix + "ReplayPresetSource");
		source->addItem(QTStr("Studio.Preset.FollowStream"), "stream");
		source->addItem(QTStr("Studio.Preset.Custom"), "custom");
		outputForm->addRow(QTStr(prefix == "Main" ? "Studio.Preset.MainClip" : "Studio.Preset.PortraitClip"),
				   source);
		auto *custom = new QWidget(outputs);
		custom->setObjectName("studioCanvas" + prefix + "ReplayCustom");
		auto *customForm = new QFormLayout(custom);
		customForm->setContentsMargins(0, 0, 0, 0);
		auto *bitrate = new QSpinBox(custom);
		bitrate->setObjectName("studioCanvas" + prefix + "ReplayBitrate");
		bitrate->setRange(100, 100000);
		bitrate->setSuffix(" Kbps");
		customForm->addRow(QTStr("Studio.Preset.VideoBitrate"), bitrate);
		auto *resolution = new QComboBox(custom);
		resolution->setObjectName("studioCanvas" + prefix + "ReplayResolution");
		resolution->addItem(QTStr("Studio.Preset.StreamDimensions"), "");
		for (const auto &size : prefix == "Main"
						? QStringList{"3840x2160", "2560x1440", "1920x1080", "1280x720"}
						: QStringList{"1080x1920", "720x1280", "540x960"}) {
			resolution->addItem(size, size);
		}
		customForm->addRow(QTStr("Studio.Preset.Resolution"), resolution);
		auto *audio = new QSpinBox(custom);
		audio->setObjectName("studioCanvas" + prefix + "ReplayAudioBitrate");
		audio->setRange(32, 512);
		audio->setSuffix(" Kbps");
		customForm->addRow(QTStr("Studio.Preset.AudioBitrate"), audio);
		auto *mix = new QComboBox(custom);
		mix->setObjectName("studioCanvas" + prefix + "ReplayAudioMix");
		for (int i = 0; i < MAX_AUDIO_MIXES; ++i) {
			mix->addItem(QTStr("Studio.Preset.AudioMixChoice").arg(i + 1), i);
		}
		customForm->addRow(QTStr("Studio.Preset.AudioMix"), mix);
		connect(mix, &QComboBox::currentIndexChanged, this, [this, mix]() {
			if (!loading) {
				mix->setProperty("changed", true);
				GeneralChanged();
			}
		});
		outputForm->addRow(custom);
		connect(source, &QComboBox::currentIndexChanged, this, [this, source, custom]() {
			custom->setEnabled(source->currentData() == "custom");
			if (!loading) {
				source->setProperty("changed", true);
				GeneralChanged();
			}
		});
		for (auto *spin : {bitrate, audio}) {
			connect(spin, &QSpinBox::valueChanged, this, [this, spin]() {
				if (!loading) {
					spin->setProperty("changed", true);
					GeneralChanged();
				}
			});
		}
		connect(resolution, &QComboBox::currentIndexChanged, this, [this, resolution]() {
			if (!loading) {
				resolution->setProperty("changed", true);
				GeneralChanged();
			}
		});
	}
	auto *presetHint = new QLabel(QTStr("Studio.Preset.Hint"), outputs);
	presetHint->setWordWrap(true);
	outputForm->addRow(presetHint);
	auto *outputHint = new QLabel(QTStr("Studio.Canvas.RecordingHint"), outputs);
	outputHint->setWordWrap(true);
	outputForm->addRow(outputHint);
	layout->addWidget(outputs);

	auto *busy = new QLabel(QTStr("Studio.Canvas.Busy"), group);
	busy->setObjectName("studioCanvasBusy");
	busy->setWordWrap(true);
	layout->addWidget(busy);
	auto *actions = new QHBoxLayout;
	for (const auto &entry :
	     {std::pair{"Studio.Canvas.VideoSettings", VIDEO}, std::pair{"Studio.Canvas.OutputSettings", OUTPUT}}) {
		auto *button = new QPushButton(QTStr(entry.first), group);
		connect(button, &QPushButton::clicked, this,
			[this, page = entry.second]() { ui->listWidget->setCurrentRow(page); });
		actions->addWidget(button);
	}
	layout->addLayout(actions);
	ui->verticalLayout_19->insertWidget(0, group);
	for (auto *card : {single, multi}) {
		connect(card, &QPushButton::clicked, this, [this, single, multi, card, modeError]() {
			if (loading) {
				return;
			}
			// Even clicking the already selected Multi card reveals its preview.
			const bool applied = main->SetStudioCanvasMode(card == multi);
			modeError->setVisible(!applied);
			if (!applied) {
				auto *config = App()->GetUserConfig();
				const bool enabled =
					!config_has_user_value(config, "BasicWindow", "StudioMultiCanvas") ||
					config_get_bool(config, "BasicWindow", "StudioMultiCanvas");
				const QSignalBlocker blockSingle(single), blockMulti(multi);
				single->setChecked(!enabled);
				multi->setChecked(enabled);
			}
			UpdateStudioCanvasSettings();
		});
	}
	connect(ui->fpsType, &QComboBox::currentIndexChanged, this, [this]() { UpdateStudioCanvasSettings(); });
	auto *guard = new QTimer(group);
	connect(guard, &QTimer::timeout, this, [this]() { UpdateStudioCanvasSettings(); });
	guard->start(500);
}

void OBSBasicSettings::LoadStudioCanvasSettings()
{
	auto *single = findChild<QPushButton *>("studioCanvasSingle");
	auto *multi = findChild<QPushButton *>("studioCanvasMulti");
	if (!single || !multi) {
		return;
	}
	const QSignalBlocker blockSingle(single), blockMulti(multi);
	config_t *config = App()->GetUserConfig();
	const bool enabled = !config_has_user_value(config, "BasicWindow", "StudioMultiCanvas") ||
			     config_get_bool(config, "BasicWindow", "StudioMultiCanvas");
	single->setChecked(!enabled);
	multi->setChecked(enabled);
	multi->setProperty("changed", false);
	auto *profile = main->Config();
	auto *streamResolution = findChild<QComboBox *>("studioCanvasOutputResolution");
	streamResolution->setProperty("changed", false);
	streamResolution->setProperty(
		"studioValue", QString::fromUtf8(config_get_string(profile, "StudioCanvas", "MainStreamResolution")));
	for (const auto &prefix : {QString("Main"), QString("Portrait")}) {
		for (const auto &suffix : {QString("ReplayPresetSource"), QString("ReplayResolution")}) {
			auto *combo = findChild<QComboBox *>("studioCanvas" + prefix + suffix);
			const QSignalBlocker block(combo);
			const auto value = QString::fromUtf8(
				config_get_string(profile, "StudioCanvas", (prefix + suffix).toUtf8().constData()));
			int index = combo->findData(value);
			if (index < 0 && suffix == "ReplayResolution" && StudioResolutionValue(value)) {
				// Profiles/API clients may store a valid size outside the offered
				// presets. Display that actual value and keep it selectable; never
				// misrepresent it as the empty "follow stream dimensions" choice.
				combo->addItem(value, value);
				index = combo->count() - 1;
			}
			combo->setCurrentIndex(index < 0 ? 0 : index);
			combo->setProperty("changed", false);
		}
		auto *mix = findChild<QComboBox *>("studioCanvas" + prefix + "ReplayAudioMix");
		const QSignalBlocker mixBlock(mix);
		mix->setCurrentIndex(std::clamp(
			int(config_get_int(profile, "StudioCanvas", (prefix + "ReplayAudioMix").toUtf8().constData())),
			0, MAX_AUDIO_MIXES - 1));
		mix->setProperty("changed", false);
		for (const auto &suffix : {QString("ReplayBitrate"), QString("ReplayAudioBitrate")}) {
			auto *spin = findChild<QSpinBox *>("studioCanvas" + prefix + suffix);
			const QSignalBlocker block(spin);
			int value =
				int(config_get_int(profile, "StudioCanvas", (prefix + suffix).toUtf8().constData()));
			spin->setValue(value > 0 ? value : suffix == "ReplayBitrate" ? 6000 : 160);
			spin->setProperty("changed", false);
		}
		findChild<QWidget *>("studioCanvas" + prefix + "ReplayCustom")
			->setEnabled(
				findChild<QComboBox *>("studioCanvas" + prefix + "ReplayPresetSource")->currentData() ==
				"custom");
	}
	for (const char *key : {"PortraitBitrate", "PortraitRecordingBitrate"}) {
		auto *spin = findChild<QSpinBox *>(QString("studioCanvas") + key);
		const QSignalBlocker block(spin);
		const int value = int(config_get_int(profile, "StudioCanvas", key));
		spin->setValue(value > 0 ? value : 6000);
		spin->setProperty("changed", false);
	}
	for (const char *key : {"RecordingCanvas", "ReplayCanvas"}) {
		auto *combo = findChild<QComboBox *>(QString("studioCanvas") + key);
		const QSignalBlocker block(combo);
		const int index = combo->findData(QString::fromUtf8(config_get_string(profile, "StudioCanvas", key)));
		combo->setCurrentIndex(index < 0 ? 0 : index);
		combo->setProperty("changed", false);
	}
}

void OBSBasicSettings::UpdateStudioCanvasSettings()
{
	auto *group = findChild<QGroupBox *>("studioCanvasSettings");
	if (!group) {
		return;
	}
	const bool busy = CanvasBusy();
	group->findChild<QWidget *>("studioCanvasModes")->setEnabled(!busy);
	group->findChild<QWidget *>("studioCanvasVideoFields")->setEnabled(!busy && ui->videoPage->isEnabled());
	group->findChild<QLabel *>("studioCanvasBusy")->setVisible(busy);
	group->findChild<QWidget *>("studioCanvasOutputFields")->setEnabled(!busy);
	auto *mainBitrate = group->findChild<QSpinBox *>("studioCanvasMainBitrate");
	const QSignalBlocker bitrateBlock(mainBitrate);
	const bool simpleMode = ui->outputMode->currentIndex() == 0;
	mainBitrate->setSpecialValueText(simpleMode ? QString{} : QTStr("Studio.Canvas.AdvancedBitrate"));
	mainBitrate->setValue(simpleMode ? ui->simpleOutputVBitrate->value() : mainBitrate->minimum());
	mainBitrate->setEnabled(simpleMode && ui->simpleOutputVBitrate->isEnabled());
	mainBitrate->setToolTip(QTStr("Studio.Canvas.MainBitrateHint"));
	CopyCombo(ui->baseResolution, group->findChild<QComboBox *>("studioCanvasBaseResolution"));
	CopyCombo(ui->outputResolution, group->findChild<QComboBox *>("studioCanvasMainVideoResolution"));
	auto *streamResolution = group->findChild<QComboBox *>("studioCanvasOutputResolution");
	const auto streamValue = streamResolution->property("changed").toBool()
					 ? streamResolution->currentText()
					 : streamResolution->property("studioValue").toString();
	CopyCombo(ui->outputResolution, streamResolution, streamValue);
	for (const auto &prefix : {QString("Main"), QString("Portrait")}) {
		findChild<QWidget *>("studioCanvas" + prefix + "ReplayCustom")
			->setEnabled(
				!busy &&
				findChild<QComboBox *>("studioCanvas" + prefix + "ReplayPresetSource")->currentData() ==
					"custom");
	}
	auto *fps = group->findChild<QComboBox *>("studioCanvasFPS");
	QString custom;
	if (ui->fpsType->currentIndex() != 0) {
		const QSignalBlocker blocked(fps);
		const QString rate =
			ui->fpsType->currentIndex() == 1
				? QString::number(ui->fpsInteger->value())
				: QString("%1/%2").arg(ui->fpsNumerator->value()).arg(ui->fpsDenominator->value());
		custom = QTStr("Studio.Canvas.CustomFPS").arg(rate);
		// The custom value is displayed without rewriting its numerator/denominator.
	}
	CopyCombo(ui->fpsCommon, fps, custom);
}

void OBSBasicSettings::SaveStudioCanvasSettings()
{
	if (!CanvasBusy()) {
		auto *resolution = findChild<QComboBox *>("studioCanvasOutputResolution");
		if (resolution && resolution->property("changed").toBool()) {
			config_set_string(main->Config(), "StudioCanvas", "MainStreamResolution",
					  resolution->currentText().toUtf8().constData());
			resolution->setProperty("studioValue", resolution->currentText());
		}
		for (const auto &prefix : {QString("Main"), QString("Portrait")}) {
			auto *mix = findChild<QComboBox *>("studioCanvas" + prefix + "ReplayAudioMix");
			if (mix && mix->property("changed").toBool()) {
				config_set_int(main->Config(), "StudioCanvas",
					       (prefix + "ReplayAudioMix").toUtf8().constData(),
					       mix->currentData().toInt());
			}
			for (const auto &suffix : {QString("ReplayPresetSource"), QString("ReplayResolution")}) {
				auto *combo = findChild<QComboBox *>("studioCanvas" + prefix + suffix);
				if (combo && combo->property("changed").toBool()) {
					config_set_string(main->Config(), "StudioCanvas",
							  (prefix + suffix).toUtf8().constData(),
							  combo->currentData().toString().toUtf8().constData());
				}
			}
			for (const auto &suffix : {QString("ReplayBitrate"), QString("ReplayAudioBitrate")}) {
				auto *spin = findChild<QSpinBox *>("studioCanvas" + prefix + suffix);
				if (spin && spin->property("changed").toBool()) {
					config_set_int(main->Config(), "StudioCanvas",
						       (prefix + suffix).toUtf8().constData(), spin->value());
				}
			}
		}
		for (const char *key : {"PortraitBitrate", "PortraitRecordingBitrate"}) {
			auto *spin = findChild<QSpinBox *>(QString("studioCanvas") + key);
			if (spin && spin->property("changed").toBool()) {
				config_set_int(main->Config(), "StudioCanvas", key, spin->value());
			}
		}
		for (const char *key : {"RecordingCanvas", "ReplayCanvas"}) {
			auto *combo = findChild<QComboBox *>(QString("studioCanvas") + key);
			if (combo && combo->property("changed").toBool()) {
				config_set_string(main->Config(), "StudioCanvas", key,
						  combo->currentData().toString().toUtf8().constData());
			}
		}
	}
}
