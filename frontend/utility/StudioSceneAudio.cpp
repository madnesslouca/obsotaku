#include "StudioSceneAudio.hpp"

#include <obs.hpp>
#include <QJsonDocument>
#include <cmath>

namespace {
StudioSceneAudio *instance = nullptr;
constexpr const char *storageKey = "studio_scene_audio";
QString UUID(obs_source_t *source)
{
	return source ? QString::fromUtf8(obs_source_get_uuid(source)) : QString();
}
bool AudioInput(obs_source_t *source)
{
	return source && !obs_source_removed(source) && obs_source_get_type(source) == OBS_SOURCE_TYPE_INPUT &&
	       (obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO);
}
QJsonObject State(obs_source_t *source)
{
	return {{"volume", double(obs_source_get_volume(source))}, {"muted", obs_source_muted(source)}};
}
bool ValidState(const QJsonObject &state)
{
	const double volume = state.value("volume").toDouble(-1);
	return state.value("volume").isDouble() && std::isfinite(volume) && volume >= 0 && volume <= 20 &&
	       state.value("muted").isBool();
}
}

StudioSceneAudio::StudioSceneAudio(QObject *parent) : QObject(parent)
{
	instance = this;
	obs_frontend_add_event_callback(Event, this);
	obs_frontend_add_save_callback(Save, this);
}
StudioSceneAudio::~StudioSceneAudio()
{
	Shutdown();
}
StudioSceneAudio *StudioSceneAudio::Get()
{
	return instance;
}
void StudioSceneAudio::Shutdown()
{
	if (stopped)
		return;
	stopped = true;
	obs_frontend_remove_event_callback(Event, this);
	obs_frontend_remove_save_callback(Save, this);
	if (instance == this)
		instance = nullptr;
	sources = {};
	programUUID.clear();
}
QString StudioSceneAudio::ProgramName() const
{
	OBSSourceAutoRelease scene = obs_get_source_by_uuid(programUUID.toUtf8().constData());
	return scene ? QString::fromUtf8(obs_source_get_name(scene)) : QString();
}
bool StudioSceneAudio::IsLinked(obs_source_t *source) const
{
	return !sources.value(UUID(source)).toObject().value("scenes").toObject().contains(programUUID);
}
void StudioSceneAudio::RememberSource(obs_source_t *source)
{
	SyncProgram();
	if (AudioInput(source) && !sources.contains(UUID(source)))
		sources.insert(UUID(source), QJsonObject{{"global", State(source)}, {"scenes", QJsonObject{}}});
}
void StudioSceneAudio::Capture()
{
	if (programUUID.isEmpty())
		return;
	for (const auto &uuid : sources.keys()) {
		OBSSourceAutoRelease source = obs_get_source_by_uuid(uuid.toUtf8().constData());
		if (!AudioInput(source))
			continue;
		auto entry = sources.value(uuid).toObject();
		auto scenes = entry.value("scenes").toObject();
		if (scenes.contains(programUUID)) {
			scenes.insert(programUUID, State(source));
			entry.insert("scenes", scenes);
		} else {
			entry.insert("global", State(source));
		}
		sources.insert(uuid, entry);
	}
}
void StudioSceneAudio::Apply()
{
	if (programUUID.isEmpty())
		return;
	for (auto it = sources.begin(); it != sources.end(); ++it) {
		OBSSourceAutoRelease source = obs_get_source_by_uuid(it.key().toUtf8().constData());
		if (!AudioInput(source))
			continue;
		const auto entry = it.value().toObject();
		const auto scenes = entry.value("scenes").toObject();
		const auto state = scenes.value(programUUID).toObject(entry.value("global").toObject());
		if (!ValidState(state))
			continue;
		obs_source_set_volume(source, float(state.value("volume").toDouble()));
		obs_source_set_muted(source, state.value("muted").toBool());
	}
}
void StudioSceneAudio::SyncProgram()
{
	OBSSourceAutoRelease scene = obs_frontend_get_current_scene();
	if (!scene || obs_source_removed(scene))
		return;
	const QString next = UUID(scene);
	if (next == programUUID)
		return;
	Capture();
	programUUID = next;
	Apply();
	emit Changed();
}
void StudioSceneAudio::SetLinked(obs_source_t *source, bool linked)
{
	RememberSource(source);
	if (!AudioInput(source) || programUUID.isEmpty() || IsLinked(source) == linked)
		return;
	Capture();
	auto entry = sources.value(UUID(source)).toObject();
	auto scenes = entry.value("scenes").toObject();
	if (linked)
		scenes.remove(programUUID);
	else
		scenes.insert(programUUID, State(source));
	entry.insert("scenes", scenes);
	sources.insert(UUID(source), entry);
	Apply();
	emit Changed();
}
void StudioSceneAudio::Prune()
{
	for (const auto &uuid : sources.keys()) {
		OBSSourceAutoRelease source = obs_get_source_by_uuid(uuid.toUtf8().constData());
		if (!AudioInput(source)) {
			sources.remove(uuid);
			continue;
		}
		auto entry = sources.value(uuid).toObject();
		auto scenes = entry.value("scenes").toObject();
		for (const auto &sceneUUID : scenes.keys()) {
			OBSSourceAutoRelease scene = obs_get_source_by_uuid(sceneUUID.toUtf8().constData());
			if (!scene || obs_source_removed(scene) || !obs_source_is_scene(scene) || !ValidState(scenes.value(sceneUUID).toObject()))
				scenes.remove(sceneUUID);
		}
		entry.insert("scenes", scenes);
		sources.insert(uuid, entry);
	}
}
QByteArray StudioSceneAudio::Snapshot()
{
	Capture();
	// Keep removed UUIDs until reload: native Undo can restore a scene/source.
	return QJsonDocument(QJsonObject{{"version", 1}, {"sources", sources}}).toJson(QJsonDocument::Compact);
}
void StudioSceneAudio::Restore(const QByteArray &snapshot)
{
	const auto data = QJsonDocument::fromJson(snapshot).object();
	if (data.value("version").toInt() != 1)
		return;
	sources = data.value("sources").toObject();
	Apply();
	emit Changed();
}
void StudioSceneAudio::Event(enum obs_frontend_event event, void *opaque)
{
	auto *self = static_cast<StudioSceneAudio *>(opaque);
	if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP) {
		self->sources = {};
		self->programUUID.clear();
		emit self->Changed();
	} else if (event == OBS_FRONTEND_EVENT_SCENE_CHANGED || event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		// Forced changes notify before the frontend updates its scene selection.
		QMetaObject::invokeMethod(self, [self]() { if (!self->stopped) self->SyncProgram(); }, Qt::QueuedConnection);
	}
}
void StudioSceneAudio::Save(obs_data_t *data, bool saving, void *opaque)
{
	auto *self = static_cast<StudioSceneAudio *>(opaque);
	if (saving) {
		OBSDataAutoRelease model = obs_data_create_from_json(self->Snapshot().constData());
		obs_data_set_obj(data, storageKey, model);
	} else {
		OBSDataAutoRelease model = obs_data_get_obj(data, storageKey);
		self->sources = {};
		self->programUUID.clear();
		if (model) {
			const auto json = QJsonDocument::fromJson(obs_data_get_json(model)).object();
			if (json.value("version").toInt() == 1)
				self->sources = json.value("sources").toObject();
		}
		self->Prune();
		self->SyncProgram();
	}
}

QString StudioSceneAudio::Scope(obs_source_t *source) const
{
	return IsLinked(source) ? QString() : programUUID;
}
void StudioSceneAudio::SetValue(obs_source_t *source, const QString &scope, const QString &field, const QJsonValue &value)
{
	if (!AudioInput(source) || (field != "volume" && field != "muted"))
		return;
	if (!sources.contains(UUID(source))) {
		if (scope.isEmpty()) {
			if (field == "volume")
				obs_source_set_volume(source, float(value.toDouble()));
			else
				obs_source_set_muted(source, value.toBool());
		}
		return;
	}
	Capture();
	auto entry = sources.value(UUID(source)).toObject();
	auto scenes = entry.value("scenes").toObject();
	if (!scope.isEmpty() && !scenes.contains(scope))
		return;
	auto state = scope.isEmpty() ? entry.value("global").toObject() : scenes.value(scope).toObject();
	state.insert(field, value);
	if (!ValidState(state))
		return;
	if (scope.isEmpty())
		entry.insert("global", state);
	else {
		scenes.insert(scope, state);
		entry.insert("scenes", scenes);
	}
	sources.insert(UUID(source), entry);
	Apply();
	emit Changed();
}
