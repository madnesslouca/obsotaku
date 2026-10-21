#pragma once

#include <docks/OBSDock.hpp>
#include <QString>

class QListWidget;
class QLabel;
class QPushButton;
class QSlider;
class QDoubleSpinBox;
class QCheckBox;
class QTimer;
struct obs_source;

/* Files remain ordinary local recordings. Playback operates only on explicitly
 * managed ffmpeg sources, identified by persistent source-private metadata. */
class StudioClipsDock : public OBSDock {
	Q_OBJECT
public:
	explicit StudioClipsDock(QWidget *parent = nullptr);
	void Shutdown();
	Q_INVOKABLE QString CanvasForClip(const QString &path) const;

public slots:
	void RegisterClip(const QString &path);
	void RegisterCanvasClip(const QString &path, const QString &canvas, const QString &pairId);

signals:
	void insertRequested(const QString &path, bool followLatest);

private:
	QListWidget *clips;
	QListWidget *layers;
	QLabel *status;
	QWidget *playback;
	QPushButton *insert;
	QPushButton *play;
	QSlider *seek;
	QDoubleSpinBox *speed;
	QCheckBox *follow;
	QCheckBox *loop;
	QLabel *range;
	QTimer *timer;
	QString storagePath;
	QString selectedUUID;
	bool closing = false;
	void Load();
	bool AddFile(const QString &path, const QString &canvas = {}, const QString &pairId = {},
		     bool allowMissing = false);
	void Save();
	void Tick();
	void Mark(bool first);
	void ResetRange();
	void UpdateRange(obs_source *source);
};
