#pragma once

#include <QObject>
#include <QJsonObject>
#include <QString>
#include <obs-frontend-api.h>

// Scene-local gain/mute over native OBS sources, not a parallel DSP graph.
// Program changes apply at the frontend SCENE_CHANGED event (transition end).
class StudioSceneAudio : public QObject {
	Q_OBJECT
public:
	explicit StudioSceneAudio(QObject *parent);
	~StudioSceneAudio();
	static StudioSceneAudio *Get();
	void Shutdown();
	bool IsLinked(obs_source_t *source) const;
	QString ProgramName() const;
	QString Scope(obs_source_t *source) const;
	void SetValue(obs_source_t *source, const QString &scope, const QString &field, const QJsonValue &value);
	void RememberSource(obs_source_t *source);
	void SetLinked(obs_source_t *source, bool linked);
	QByteArray Snapshot();
	void Restore(const QByteArray &snapshot);

signals:
	void Changed();

private:
	QJsonObject sources;
	QString programUUID;
	bool stopped = false;
	void Capture();
	void Apply();
	void SyncProgram();
	void Prune();
	static void Event(enum obs_frontend_event event, void *opaque);
	static void Save(obs_data_t *data, bool saving, void *opaque);
};
