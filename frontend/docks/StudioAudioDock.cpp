#include "StudioAudioDock.hpp"

#include <OBSApp.hpp>
#include <components/VolumeMeter.hpp>
#include <utility/StudioSceneAudio.hpp>
#include <utility/StudioVodControls.hpp>
#include <utility/StudioMonitoringDevices.hpp>
#include <QAction>
#include <widgets/OBSBasic.hpp>
#include <obs.hpp>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <vector>

namespace {
class MonitoringDeviceCombo final : public QComboBox {
public:
	explicit MonitoringDeviceCombo(QWidget *parent) : QComboBox(parent) {}
	std::function<void()> refreshDevices;

protected:
	void showPopup() override
	{
		if (refreshDevices) {
			refreshDevices();
		}
		QComboBox::showPopup();
	}
};

std::vector<StudioMonitoringDevice> EnumerateMonitoringDevices()
{
	std::vector<StudioMonitoringDevice> devices;
	obs_enum_audio_monitoring_devices(
		[](void *opaque, const char *name, const char *id) {
			if (name && id) {
				static_cast<std::vector<StudioMonitoringDevice> *>(opaque)->push_back(
					{QString::fromUtf8(name), QString::fromUtf8(id)});
			}
			return true;
		},
		&devices);
	return devices;
}
} // namespace

static bool AudioSource(obs_source_t *source)
{
	return source && !obs_source_removed(source) && obs_source_get_type(source) == OBS_SOURCE_TYPE_INPUT &&
	       (obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO);
}

static std::string AudioState(obs_source_t *source)
{
	QJsonObject state{{"uuid", QString::fromUtf8(obs_source_get_uuid(source))},
			  {"scope", StudioSceneAudio::Get() ? StudioSceneAudio::Get()->Scope(source) : QString()},
			  {"volume", double(obs_source_get_volume(source))},
			  {"mute", obs_source_muted(source)},
			  {"mono", bool(obs_source_get_flags(source) & OBS_SOURCE_FLAG_FORCE_MONO)},
			  {"balance", double(obs_source_get_balance_value(source))},
			  {"sync", double(obs_source_get_sync_offset(source))},
			  {"monitor", int(obs_source_get_monitoring_type(source))},
			  {"mixers", int(obs_source_get_audio_mixers(source))}};
	return QJsonDocument(state).toJson(QJsonDocument::Compact).toStdString();
}

static void RestoreAudioState(const std::string &json)
{
	const auto data = QJsonDocument::fromJson(QByteArray::fromStdString(json)).object();
	OBSSourceAutoRelease source = obs_get_source_by_uuid(data.value("uuid").toString().toUtf8().constData());
	if (!AudioSource(source)) {
		return;
	}
	const QString scope = data.value("scope").toString();
	if (data.contains("volume")) {
		if (auto *model = StudioSceneAudio::Get()) {
			model->SetValue(source, scope, "volume", data.value("volume"));
		} else {
			obs_source_set_volume(source, float(data.value("volume").toDouble()));
		}
	}
	if (data.contains("mute")) {
		if (auto *model = StudioSceneAudio::Get()) {
			model->SetValue(source, scope, "muted", data.value("mute"));
		} else {
			obs_source_set_muted(source, data.value("mute").toBool());
		}
	}
	if (data.contains("mono")) {
		uint32_t flags = obs_source_get_flags(source);
		flags = data.value("mono").toBool() ? flags | OBS_SOURCE_FLAG_FORCE_MONO
						    : flags & ~OBS_SOURCE_FLAG_FORCE_MONO;
		obs_source_set_flags(source, flags);
	}
	if (data.contains("balance")) {
		obs_source_set_balance_value(source, float(data.value("balance").toDouble()));
	}
	if (data.contains("sync")) {
		obs_source_set_sync_offset(source, int64_t(data.value("sync").toDouble()));
	}
	if (data.contains("monitor")) {
		obs_source_set_monitoring_type(source, obs_monitoring_type(data.value("monitor").toInt()));
	}
	if (data.contains("mixers")) {
		const uint32_t mask = uint32_t(data.value("mixersMask").toInt());
		const uint32_t current = obs_source_get_audio_mixers(source);
		obs_source_set_audio_mixers(source,
					    (current & ~mask) | (uint32_t(data.value("mixers").toInt()) & mask));
	}
	OBSBasic::Get()->SaveProject();
}

StudioAudioDock::StudioAudioDock(QWidget *parent) : OBSDock(parent)
{
	sceneAudio = new StudioSceneAudio(this);
	setObjectName(QStringLiteral("studioAudioDock"));
	setWindowTitle(QTStr("Studio.AudioRouting"));
	setMinimumWidth(310);
	auto *body = new QWidget(this);
	auto *layout = new QVBoxLayout(body);
	auto *hint = new QLabel(QTStr("Studio.AudioRoutingHint"), body);
	hint->setWordWrap(true);
	layout->addWidget(hint);
	auto *addSource = new QPushButton(QTStr("Studio.AudioAddSource"), body);
	addSource->setObjectName(QStringLiteral("studioAudioAddSource"));
	layout->addWidget(addSource);
	connect(addSource, &QPushButton::clicked, this, []() {
		if (auto *action = OBSBasic::Get()->findChild<QAction *>(QStringLiteral("actionAddSource"))) {
			action->trigger();
		}
	});
	routing = new QTableWidget(0, 7, body);
	routing->setObjectName(QStringLiteral("studioAudioRouting"));
	routing->setHorizontalHeaderLabels({QTStr("Studio.Source"), "1", "2", "3", "4", "5", "6"});
	routing->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
	for (int col = 1; col <= 6; ++col) {
		routing->horizontalHeader()->setSectionResizeMode(col, QHeaderView::ResizeToContents);
	}
	routing->verticalHeader()->hide();
	routing->setSelectionBehavior(QAbstractItemView::SelectRows);
	routing->setSelectionMode(QAbstractItemView::SingleSelection);
	routing->setEditTriggers(QAbstractItemView::NoEditTriggers);
	routing->setMinimumHeight(130);
	routing->setMaximumHeight(260);
	layout->addWidget(routing);
	sourceStatus = new QLabel(body);
	sourceStatus->setObjectName(QStringLiteral("studioAudioStatus"));
	sourceStatus->setWordWrap(true);
	layout->addWidget(sourceStatus);
	meterLayout = new QVBoxLayout;
	layout->addLayout(meterLayout);
	controls = new QWidget(body);
	auto *form = new QFormLayout(controls);
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	form->setRowWrapPolicy(QFormLayout::WrapLongRows);
	sourceName = new QLabel(controls);
	sourceName->setTextFormat(Qt::PlainText);
	sourceName->setWordWrap(true);
	form->addRow(sourceName);
	sceneLink = new QCheckBox(QTStr("Studio.AudioSceneLinked"), controls);
	sceneLink->setObjectName(QStringLiteral("studioAudioSceneLink"));
	sceneLink->setToolTip(QTStr("Studio.AudioSceneLinkHint"));
	form->addRow(sceneLink);
	vodExclude = new QPushButton(QTStr("Studio.VodExclude"), controls);
	vodExclude->setObjectName(QStringLiteral("studioAudioVodExclude"));
	vodExclude->setCheckable(true);
	form->addRow(vodExclude);
	vodHint = new QLabel(controls);
	vodHint->setObjectName(QStringLiteral("studioAudioVodHint"));
	vodHint->setWordWrap(true);
	form->addRow(vodHint);
	connect(vodExclude, &QPushButton::clicked, this, [this]() {
		OBSSourceAutoRelease source = obs_get_source_by_uuid(selectedUUID.toUtf8().constData());
		if (!AudioSource(source)) {
			return;
		}
		const auto plan = CurrentStudioVodRouting();
		SetStudioSourceVodExcluded(source, (obs_source_get_audio_mixers(source) & plan.vodMask) != 0);
		Refresh();
	});
	connect(sceneLink, &QCheckBox::clicked, this, [this](bool linked) { SetSceneLinked(selectedUUID, linked); });
	connect(sceneAudio, &StudioSceneAudio::Changed, this, &StudioAudioDock::Refresh);
	auto *device = new QPushButton(QTStr("Studio.AudioSourceProperties"), controls);
	device->setObjectName(QStringLiteral("studioAudioSourceProperties"));
	form->addRow(device);
	connect(device, &QPushButton::clicked, this, [this]() {
		OBSSourceAutoRelease source = obs_get_source_by_uuid(selectedUUID.toUtf8().constData());
		if (AudioSource(source)) {
			OBSBasic::Get()->CreatePropertiesWindow(source);
		}
	});
	gain = new QDoubleSpinBox(controls);
	gain->setObjectName(QStringLiteral("studioAudioGain"));
	gain->setRange(-96, 26);
	gain->setDecimals(1);
	gain->setSuffix(QStringLiteral(" dB"));
	form->addRow(QTStr("Basic.AdvAudio.Volume"), gain);
	mute = new QCheckBox(QTStr("Mute"), controls);
	mute->setObjectName(QStringLiteral("studioAudioMute"));
	form->addRow(mute);
	monitor = new QComboBox(controls);
	monitor->setObjectName(QStringLiteral("studioAudioMonitor"));
	monitor->addItem(QTStr("Basic.AdvAudio.Monitoring.None"), int(OBS_MONITORING_TYPE_NONE));
	monitor->addItem(QTStr("Basic.AdvAudio.Monitoring.MonitorOnly"), int(OBS_MONITORING_TYPE_MONITOR_ONLY));
	monitor->addItem(QTStr("Basic.AdvAudio.Monitoring.Both"), int(OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT));
	monitor->setToolTip(QTStr("Studio.AudioCueHint"));
	form->addRow(QTStr("Studio.AudioCue"), monitor);
	mono = new QCheckBox(QTStr("Basic.AdvAudio.Mono"), controls);
	mono->setObjectName(QStringLiteral("studioAudioMono"));
	form->addRow(mono);
	balance = new QDoubleSpinBox(controls);
	balance->setObjectName(QStringLiteral("studioAudioBalance"));
	balance->setRange(0, 1);
	balance->setDecimals(2);
	balance->setSingleStep(.05);
	form->addRow(QTStr("Basic.AdvAudio.Balance"), balance);
	sync = new QSpinBox(controls);
	sync->setObjectName(QStringLiteral("studioAudioSync"));
	sync->setRange(-950, 20000);
	sync->setSuffix(QStringLiteral(" ms"));
	form->addRow(QTStr("Basic.AdvAudio.SyncOffset"), sync);
	auto *effects = new QPushButton(QTStr("Studio.AudioEffects"), controls);
	effects->setObjectName(QStringLiteral("studioAudioEffects"));
	form->addRow(effects);
	auto *voice = new QPushButton(QTStr("Studio.AudioVoicePreset"), controls);
	voice->setObjectName(QStringLiteral("studioAudioVoicePreset"));
	voice->setToolTip(QTStr("Studio.AudioVoicePresetHint"));
	form->addRow(voice);
	layout->addWidget(controls);
	auto *advanced = new QPushButton(QTStr("Basic.MainMenu.Edit.AdvAudio"), body);
	advanced->setObjectName(QStringLiteral("studioAudioAdvanced"));
	layout->addWidget(advanced);
	auto *deviceLabel = new QLabel(QTStr("Basic.Settings.Advanced.Audio.MonitoringDevice"), body);
	layout->addWidget(deviceLabel);
	auto *deviceCombo = new MonitoringDeviceCombo(body);
	deviceCombo->refreshDevices = [this]() { RefreshMonitoringDevices(); };
	monitorDevice = deviceCombo;
	monitorDevice->setObjectName(QStringLiteral("studioAudioMonitorDevice"));
	monitorDevice->setToolTip(QTStr("Studio.AudioMonitorDeviceHint"));
	layout->addWidget(monitorDevice);
	auto *refreshDevices = new QPushButton(QTStr("Studio.AudioRefreshDevices"), body);
	refreshDevices->setObjectName(QStringLiteral("studioAudioRefreshDevices"));
	layout->addWidget(refreshDevices);
	feedback = new QLabel(body);
	feedback->setObjectName(QStringLiteral("studioAudioFeedback"));
	feedback->setWordWrap(true);
	feedback->setTextFormat(Qt::PlainText);
	layout->addWidget(feedback);
	connect(refreshDevices, &QPushButton::clicked, this, &StudioAudioDock::RefreshMonitoringDevices);
	connect(monitorDevice, &QComboBox::activated, this, [this]() {
		const QString id = monitorDevice->currentData().toString();
		// The popup may have been open while an endpoint disappeared. The core
		// setter accepts any nonempty ID, so resolve against a fresh inventory
		// before changing the shared CUE destination or the profile.
		if (!obs_audio_monitoring_available()) {
			feedback->setText(QTStr("Studio.AudioMonitorDeviceError"));
			RefreshMonitoringDevices();
			return;
		}
		const auto device = ResolveStudioMonitoringDevice(
			EnumerateMonitoringDevices(), id,
			QTStr("Basic.Settings.Advanced.Audio.MonitoringDevice.Default"));
		if (!device || !obs_set_audio_monitoring_device(device->name.toUtf8().constData(),
							      device->id.toUtf8().constData())) {
			feedback->setText(QTStr("Studio.AudioMonitorDeviceError"));
			RefreshMonitoringDevices();
			return;
		}
		auto *config = OBSBasic::Get()->Config();
		config_set_string(config, "Audio", "MonitoringDeviceName", device->name.toUtf8().constData());
		config_set_string(config, "Audio", "MonitoringDeviceId", device->id.toUtf8().constData());
		const bool saved = config_save_safe(config, "tmp", nullptr) == CONFIG_SUCCESS;
		feedback->setText(QTStr(saved ? "Studio.AudioMonitorDeviceSaved"
					    : "Studio.AudioMonitorDeviceNotSaved"));
		RefreshMonitoringDevices();
	});
	auto *scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setWidget(body);
	setWidget(scroll);
	connect(routing, &QTableWidget::currentCellChanged, this, [this](int row) {
		selectedUUID = row >= 0 && routing->item(row, 0) ? routing->item(row, 0)->data(Qt::UserRole).toString()
								 : QString();
		Refresh();
	});
	connect(routing, &QTableWidget::cellChanged, this, [this](int row, int col) {
		if (col < 1 || !routing->item(row, 0)) {
			return;
		}
		const QString uuid = routing->item(row, 0)->data(Qt::UserRole).toString();
		const bool checked = routing->item(row, col)->checkState() == Qt::Checked;
		const QString previous = selectedUUID;
		selectedUUID = uuid;
		Edit(QTStr("Studio.AudioRouting"), [col, checked](obs_source_t *source) {
			uint32_t mask = obs_source_get_audio_mixers(source), bit = 1u << (col - 1);
			obs_source_set_audio_mixers(source, checked ? mask | bit : mask & ~bit);
		});
		selectedUUID = previous;
	});
	connect(gain, &QDoubleSpinBox::editingFinished, this, [this]() {
		Edit(QTStr("Basic.AdvAudio.Volume"), [this](obs_source_t *source) {
			obs_source_set_volume(source, gain->value() <= -96 ? 0.f : float(obs_db_to_mul(gain->value())));
		});
	});
	connect(mute, &QCheckBox::clicked, this, [this](bool checked) {
		Edit(QTStr("Mute"), [checked](obs_source_t *source) { obs_source_set_muted(source, checked); });
	});
	connect(mono, &QCheckBox::clicked, this, [this](bool checked) {
		Edit(QTStr("Basic.AdvAudio.Mono"), [checked](obs_source_t *source) {
			uint32_t flags = obs_source_get_flags(source);
			obs_source_set_flags(source, checked ? flags | OBS_SOURCE_FLAG_FORCE_MONO
							     : flags & ~OBS_SOURCE_FLAG_FORCE_MONO);
		});
	});
	connect(balance, &QDoubleSpinBox::editingFinished, this, [this]() {
		Edit(QTStr("Basic.AdvAudio.Balance"),
		     [this](obs_source_t *source) { obs_source_set_balance_value(source, float(balance->value())); });
	});
	connect(sync, &QSpinBox::editingFinished, this, [this]() {
		Edit(QTStr("Basic.AdvAudio.SyncOffset"), [this](obs_source_t *source) {
			obs_source_set_sync_offset(source, int64_t(sync->value()) * 1000000);
		});
	});
	connect(monitor, &QComboBox::activated, this, [this]() {
		Edit(QTStr("Studio.AudioCue"), [this](obs_source_t *source) {
			obs_source_set_monitoring_type(source, obs_monitoring_type(monitor->currentData().toInt()));
		});
	});
	connect(effects, &QPushButton::clicked, this, [this]() {
		OBSSourceAutoRelease source = obs_get_source_by_uuid(selectedUUID.toUtf8().constData());
		if (AudioSource(source)) {
			OBSBasic::Get()->CreateFiltersWindow(source);
		}
	});
	connect(voice, &QPushButton::clicked, this, &StudioAudioDock::AddVoiceChain);
	connect(advanced, &QPushButton::clicked, OBSBasic::Get(), &OBSBasic::on_actionAdvAudioProperties_triggered);
	timer = new QTimer(this);
	timer->setInterval(250);
	connect(timer, &QTimer::timeout, this, &StudioAudioDock::Refresh);
	timer->start();
	controls->setEnabled(false);
}

void StudioAudioDock::Edit(const QString &label, const std::function<void(obs_source_t *)> &change)
{
	OBSSourceAutoRelease source = obs_get_source_by_uuid(selectedUUID.toUtf8().constData());
	if (!AudioSource(source)) {
		return;
	}
	const std::string before = AudioState(source);
	change(source);
	const std::string after = AudioState(source);
	if (before != after) {
		const auto previous = QJsonDocument::fromJson(QByteArray::fromStdString(before)).object();
		const auto next = QJsonDocument::fromJson(QByteArray::fromStdString(after)).object();
		QJsonObject undo{{"uuid", previous.value("uuid")}, {"scope", previous.value("scope")}};
		QJsonObject redo{{"uuid", next.value("uuid")}, {"scope", next.value("scope")}};
		for (const char *field : {"volume", "mute", "mono", "balance", "sync", "monitor", "mixers"}) {
			if (previous.value(field) == next.value(field)) {
				continue;
			}
			undo.insert(field, previous.value(field));
			redo.insert(field, next.value(field));
			if (!strcmp(field, "mixers")) {
				const int mask = previous.value(field).toInt() ^ next.value(field).toInt();
				undo.insert("mixersMask", mask);
				redo.insert("mixersMask", mask);
			}
		}
		OBSBasic::Get()->undo_s.add_action(label + QStringLiteral(" — ") +
							   QString::fromUtf8(obs_source_get_name(source)),
						   RestoreAudioState, RestoreAudioState,
						   QJsonDocument(undo).toJson(QJsonDocument::Compact).toStdString(),
						   QJsonDocument(redo).toJson(QJsonDocument::Compact).toStdString());
		OBSBasic::Get()->SaveProject();
	}
}

void StudioAudioDock::Refresh()
{
	if (!isVisible()) {
		return;
	}
	std::vector<OBSSource> sources;
	obs_enum_sources(
		[](void *opaque, obs_source_t *source) {
			if (AudioSource(source)) {
				static_cast<std::vector<OBSSource> *>(opaque)->emplace_back(source);
			}
			return true;
		},
		&sources);
	std::sort(sources.begin(), sources.end(), [](const OBSSource &a, const OBSSource &b) {
		return QString::fromUtf8(obs_source_get_name(a))
			       .localeAwareCompare(QString::fromUtf8(obs_source_get_name(b))) < 0;
	});
	const QSignalBlocker tableBlock(routing);
	bool changed = routing->rowCount() != int(sources.size());
	for (int row = 0; !changed && row < routing->rowCount(); ++row) {
		changed = routing->item(row, 0)->data(Qt::UserRole).toString() !=
			  QString::fromUtf8(obs_source_get_uuid(sources[row]));
	}
	if (changed) {
		routing->clearContents();
		routing->setRowCount(int(sources.size()));
		for (int row = 0; row < int(sources.size()); ++row) {
			auto *item = new QTableWidgetItem;
			const QString uuid = QString::fromUtf8(obs_source_get_uuid(sources[row]));
			item->setData(Qt::UserRole, uuid);
			routing->setItem(row, 0, item);
			for (int col = 1; col <= 6; ++col) {
				auto *track = new QTableWidgetItem;
				track->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
				routing->setItem(row, col, track);
			}
			if (uuid == selectedUUID) {
				routing->setCurrentCell(row, 0);
			}
		}
	}
	// A removed source must not leave the panel bound to an unreachable UUID.
	auto selected = std::find_if(sources.begin(), sources.end(), [this](const OBSSource &source) {
		return selectedUUID == QString::fromUtf8(obs_source_get_uuid(source));
	});
	if (selected == sources.end()) {
		selected = std::find_if(sources.begin(), sources.end(),
					[](const OBSSource &source) { return obs_source_active(source); });
		if (selected == sources.end() && !sources.empty()) {
			selected = sources.begin();
		}
		selectedUUID = selected == sources.end() ? QString()
							 : QString::fromUtf8(obs_source_get_uuid(*selected));
	}
	if (selected != sources.end()) {
		routing->setCurrentCell(int(selected - sources.begin()), 0);
	}
	for (int row = 0; row < int(sources.size()); ++row) {
		auto &source = sources[row];
		routing->item(row, 0)->setText(QString::fromUtf8(obs_source_get_name(source)));
		routing->item(row, 0)->setToolTip(
			QTStr(obs_source_active(source) ? "Studio.AudioActive" : "Studio.AudioInactive"));
		const uint32_t mask = obs_source_get_audio_mixers(source);
		for (int col = 1; col <= 6; ++col) {
			routing->item(row, col)->setCheckState(mask & (1u << (col - 1)) ? Qt::Checked : Qt::Unchecked);
		}
	}
	OBSSourceAutoRelease source = obs_get_source_by_uuid(selectedUUID.toUtf8().constData());
	controls->setEnabled(AudioSource(source));
	const QString sceneName = sceneAudio->ProgramName();
	const bool sceneChanged = controls->property("studioProgram").toString() != sceneName;
	controls->setProperty("studioProgram", sceneName);
	const bool selectionChanged = meterUUID != selectedUUID;
	if (selectionChanged) {
		delete meter;
		meter = nullptr;
		meterUUID = selectedUUID;
		feedback->clear();
		if (AudioSource(source)) {
			meter = new VolumeMeter(this, source);
			meter->setObjectName(QStringLiteral("studioAudioLevelMeter"));
			meterLayout->addWidget(meter);
		}
	}
	if (meter) {
		meter->setMuted(obs_source_muted(source));
	}
	if (!AudioSource(source)) {
		sourceName->setText(QTStr("Studio.AudioSelect"));
		sourceStatus->setText(QTStr("Studio.AudioSelect"));
		return;
	}
	sourceName->setText(QString::fromUtf8(obs_source_get_name(source)));
	const auto vod = CurrentStudioVodRouting();
	const uint32_t included = obs_source_get_audio_mixers(source) & vod.vodMask;
	const QSignalBlocker vodBlock(vodExclude);
	vodExclude->setChecked(included == 0);
	vodExclude->setEnabled(!vod.invalid && !vod.conflict);
	vodExclude->setText(QTStr(included == 0             ? "Studio.VodExcluded"
				  : included != vod.vodMask ? "Studio.VodPartial"
							    : "Studio.VodExclude"));
	vodExclude->setProperty("studioVodMask", qulonglong(vod.vodMask));
	vodExclude->setProperty("studioLiveMask", qulonglong(vod.liveMask));
	vodExclude->setProperty("studioVodEffective", vod.effective);
	vodExclude->setProperty("studioVodActiveSession", vod.activeSession);
	vodHint->setText(StudioVodRoutingHint(vod));
	vodExclude->setToolTip(vodHint->text());
	sourceStatus->setText(QTStr(obs_source_active(source) ? "Studio.AudioActive" : "Studio.AudioInactive") +
			      QStringLiteral("\n") + QTStr("Studio.AudioProgramScene").arg(sceneName));
	const QSignalBlocker linkBlock(sceneLink);
	sceneLink->setChecked(sceneAudio->IsLinked(source));
	sceneLink->setToolTip(QTStr("Studio.AudioSceneLinkHint") + QStringLiteral("\n") +
			      QTStr("Studio.AudioProgramScene").arg(sceneName));
	const QSignalBlocker gainBlock(gain), muteBlock(mute), monoBlock(mono), balanceBlock(balance), syncBlock(sync),
		monitorBlock(monitor);
	if (selectionChanged || sceneChanged || !gain->hasFocus()) {
		gain->setValue(std::clamp(double(obs_mul_to_db(obs_source_get_volume(source))), -96.0, 26.0));
	}
	mute->setChecked(obs_source_muted(source));
	mono->setChecked(obs_source_get_flags(source) & OBS_SOURCE_FLAG_FORCE_MONO);
	if (selectionChanged || sceneChanged || !balance->hasFocus()) {
		balance->setValue(obs_source_get_balance_value(source));
	}
	if (selectionChanged || sceneChanged || !sync->hasFocus()) {
		sync->setValue(int(obs_source_get_sync_offset(source) / 1000000));
	}
	monitor->setEnabled(obs_audio_monitoring_available());
	if (selectionChanged || sceneChanged || !monitor->hasFocus()) {
		monitor->setCurrentIndex(monitor->findData(int(obs_source_get_monitoring_type(source))));
	}
}

void StudioAudioDock::AddVoiceChain()
{
	OBSSourceAutoRelease source = obs_get_source_by_uuid(selectedUUID.toUtf8().constData());
	if (!AudioSource(source)) {
		return;
	}
	struct Filter {
		const char *id;
		const char *name;
	};
	const Filter filters[] = {{"noise_suppress_filter_v2", "Studio — RNNoise"},
				  {"compressor_filter", "Studio — Compressor"},
				  {"limiter_filter", "Studio — Limiter"}};
	for (auto &filter : filters) {
		if (!obs_source_get_display_name(filter.id)) {
			feedback->setText(QTStr("Studio.AudioEffectUnavailable").arg(QString::fromUtf8(filter.id)));
			return;
		}
	}
	OBSDataArrayAutoRelease saved = obs_data_array_create();
	for (auto &spec : filters) {
		OBSSourceAutoRelease existing = obs_source_get_filter_by_name(source, spec.name);
		if (existing) {
			continue;
		}
		OBSDataAutoRelease settings = obs_data_create();
		if (!strcmp(spec.id, "noise_suppress_filter_v2")) {
			obs_data_set_string(settings, "method", "rnnoise");
		} else if (!strcmp(spec.id, "compressor_filter")) {
			obs_data_set_double(settings, "threshold", -18);
			obs_data_set_double(settings, "ratio", 3);
			obs_data_set_double(settings, "attack_time", 6);
			obs_data_set_double(settings, "release_time", 100);
		} else {
			obs_data_set_double(settings, "threshold", -1);
			obs_data_set_double(settings, "release_time", 60);
		}
		OBSSourceAutoRelease filter = obs_source_create(spec.id, spec.name, settings, nullptr);
		if (!filter) {
			continue;
		}
		obs_source_filter_add(source, filter);
		OBSDataAutoRelease data = obs_save_source(filter);
		obs_data_array_push_back(saved, data);
	}
	if (!obs_data_array_count(saved)) {
		feedback->setText(QTStr("Studio.AudioVoicePresent"));
		return;
	}
	feedback->setText(QTStr("Studio.AudioVoiceAdded").arg(obs_data_array_count(saved)));
	OBSDataAutoRelease wrapper = obs_data_create();
	obs_data_set_array(wrapper, "filters", saved);
	const std::string uuid = obs_source_get_uuid(source);
	auto undo = [uuid](const std::string &json) {
		OBSSourceAutoRelease source = obs_get_source_by_uuid(uuid.c_str());
		if (!source) {
			return;
		}
		OBSDataAutoRelease data = obs_data_create_from_json(json.c_str());
		OBSDataArrayAutoRelease filters = obs_data_get_array(data, "filters");
		for (size_t i = 0; i < obs_data_array_count(filters); ++i) {
			OBSDataAutoRelease filterData = obs_data_array_item(filters, i);
			OBSSourceAutoRelease filter =
				obs_source_get_filter_by_name(source, obs_data_get_string(filterData, "name"));
			if (filter) {
				obs_source_filter_remove(source, filter);
			}
		}
	};
	auto redo = [uuid](const std::string &json) {
		OBSSourceAutoRelease source = obs_get_source_by_uuid(uuid.c_str());
		if (!source) {
			return;
		}
		OBSDataAutoRelease data = obs_data_create_from_json(json.c_str());
		OBSDataArrayAutoRelease filters = obs_data_get_array(data, "filters");
		for (size_t i = 0; i < obs_data_array_count(filters); ++i) {
			OBSDataAutoRelease filterData = obs_data_array_item(filters, i);
			OBSSourceAutoRelease existing =
				obs_source_get_filter_by_name(source, obs_data_get_string(filterData, "name"));
			if (!existing) {
				// A filters dialog or plugin may still hold the detached instance.
				// Recreate with a fresh UUID instead of registering it twice.
				obs_data_erase(filterData, "uuid");
				OBSSourceAutoRelease filter = obs_load_source(filterData);
				if (filter) {
					obs_source_filter_add(source, filter);
				}
			}
		}
	};
	const std::string json = obs_data_get_json(wrapper);
	OBSBasic::Get()->undo_s.add_action(QTStr("Studio.AudioVoicePreset"), undo, redo, json, json);
	OBSBasic::Get()->SaveProject();
}

void StudioAudioDock::Shutdown()
{
	sceneAudio->Shutdown();
	timer->stop();
	delete meter;
	meter = nullptr;
	meterUUID.clear();
}

void StudioAudioDock::SelectSource(const QString &uuid)
{
	RefreshMonitoringDevices();
	if (!uuid.isEmpty()) {
		selectedUUID = uuid;
	}
	Refresh();
	const QSignalBlocker blocker(routing);
	for (int row = 0; row < routing->rowCount(); ++row) {
		if (routing->item(row, 0)->data(Qt::UserRole).toString() == selectedUUID) {
			routing->setCurrentCell(row, 0);
		}
	}
	QTimer::singleShot(0, this, [this]() {
		if (auto *scroll = qobject_cast<QScrollArea *>(widget())) {
			scroll->ensureWidgetVisible(gain);
		}
	});
}

void StudioAudioDock::RefreshMonitoringDevices()
{
	const QSignalBlocker blocker(monitorDevice);
	const bool available = obs_audio_monitoring_available();
	monitorDevice->setEnabled(available);
	monitorDevice->clear();
	if (!available) {
		return;
	}
	const char *currentName = nullptr, *currentId = nullptr;
	obs_get_audio_monitoring_device(&currentName, &currentId);
	// Enumeration may replace backend strings, so retain owned copies first.
	const QString id = QString::fromUtf8(currentId ? currentId : "default");
	const QString name = QString::fromUtf8(currentName ? currentName : "");
	PopulateStudioMonitoringDevices(monitorDevice, EnumerateMonitoringDevices(), id, name,
					QTStr("Basic.Settings.Advanced.Audio.MonitoringDevice.Default"),
					QTStr("Studio.AudioMonitorDeviceMissing"));
}

void StudioAudioDock::SetSceneLinked(const QString &uuid, bool linked)
{
	OBSSourceAutoRelease source = obs_get_source_by_uuid(uuid.toUtf8().constData());
	if (!AudioSource(source)) {
		return;
	}
	sceneAudio->RememberSource(source);
	if (sceneAudio->IsLinked(source) == linked) {
		return;
	}
	const std::string before = sceneAudio->Snapshot().toStdString();
	sceneAudio->SetLinked(source, linked);
	const std::string after = sceneAudio->Snapshot().toStdString();
	auto restore = [](const std::string &json) {
		if (auto *model = StudioSceneAudio::Get()) {
			model->Restore(QByteArray::fromStdString(json));
			OBSBasic::Get()->SaveProject();
		}
	};
	OBSBasic::Get()->undo_s.add_action(QTStr("Studio.AudioSceneLinked"), restore, restore, before, after);
	OBSBasic::Get()->SaveProject();
}
