#pragma once

#include <docks/OBSDock.hpp>
#include <QString>
#include <QPointer>
#include <functional>
#include <obs.h>

class QTableWidget;
class QDoubleSpinBox;
class QSpinBox;
class QCheckBox;
class QComboBox;
class QLabel;
class QTimer;
class VolumeMeter;
class QVBoxLayout;
class QPushButton;
class StudioSceneAudio;

/* Native OBS source controls and six output mixes. Tracks are output mixes,
 * not independent effect buses. No parallel audio graph is maintained. */
class StudioAudioDock : public OBSDock {
	Q_OBJECT
public:
	explicit StudioAudioDock(QWidget *parent = nullptr);
	void Shutdown();
	void SelectSource(const QString &uuid);
	void SetSceneLinked(const QString &uuid, bool linked);

private:
	QTableWidget *routing;
	QWidget *controls;
	QLabel *sourceName;
	QLabel *sourceStatus;
	QLabel *feedback;
	QComboBox *monitorDevice;
	QVBoxLayout *meterLayout;
	QPointer<VolumeMeter> meter;
	QString meterUUID;
	QDoubleSpinBox *gain;
	QDoubleSpinBox *balance;
	QCheckBox *mute;
	QCheckBox *sceneLink;
	QPushButton *vodExclude;
	QLabel *vodHint;
	StudioSceneAudio *sceneAudio;
	QCheckBox *mono;
	QComboBox *monitor;
	QSpinBox *sync;
	QTimer *timer;
	QString selectedUUID;
	void Refresh();
	void RefreshMonitoringDevices();
	void Edit(const QString &label, const std::function<void(obs_source_t *)> &change);
	void AddVoiceChain();
};
