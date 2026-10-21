#pragma once

#include <obs.hpp>
#include <QObject>
#include <functional>
#include <memory>

/* Routes native recording/replay outputs without changing a shared encoder.
 * Audio encoders and track order come from the native output configuration. */
class StudioCanvasOutputs : public QObject {
public:
	explicit StudioCanvasOutputs(QObject *parent);
	~StudioCanvasOutputs() override;
	bool Start(obs_output_t *output, bool replay, QString &error);
	void Finish(bool replay, int code, const QString &error, std::function<void(int, QString)> complete);
	bool Active() const;
	void StopCompanion(bool force);

private:
	struct Session;
	std::unique_ptr<Session> recording, replay;
	void TryFinish(Session &session);
};
