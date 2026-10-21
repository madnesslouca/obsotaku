#pragma once

#include <QObject>
#include <QJsonObject>
#include <obs.hpp>
#include <array>
#include <memory>

class OBSBasic;
struct BasicOutputHandler;

/* Independent replay outputs: no encoder is shared with recording or a live
 * destination. All state transitions and UI notifications run on the Qt thread. */
class StudioReplayOutputs : public QObject {
	Q_OBJECT
public:
	explicit StudioReplayOutputs(BasicOutputHandler *handler, OBSBasic *main);
	~StudioReplayOutputs() override;
	bool Active() const;
	OBSOutputAutoRelease PreferredOutput() const;
	Q_INVOKABLE bool StartTargets(QString targets);
	Q_INVOKABLE QJsonObject SaveTargets(QString targets);
	Q_INVOKABLE void StopTargets(QString targets, bool force = false);
	Q_INVOKABLE QJsonObject Snapshot() const;

signals:
	void AggregateChanged(bool active);
	void AggregateStopping();
	void AggregateStoppingChanged(bool stopping);
	void Saved(QString canvas, QString path, QString pairId);
	void Error(QString canvas, QString message);

private:
	struct Buffer;
	BasicOutputHandler *handler;
	OBSBasic *main;
	std::array<std::unique_ptr<Buffer>, 2> buffers;
	bool notifiedActive = false;
	bool notifiedStopping = false;
	bool closing = false;
	bool Prepare(Buffer &buffer, QString &error);
	void NotifyAggregate();
	void CompleteStop(Buffer &buffer, int code, QString error);
	void RequestStop(Buffer &buffer, bool force);
	static void OnStop(void *data, calldata_t *params);
	static void OnSaveResult(void *data, calldata_t *params);
};
