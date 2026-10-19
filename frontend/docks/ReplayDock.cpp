/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "ReplayDock.hpp"

#include <OBSApp.hpp>
#include <qt-wrappers.hpp>

#include <util/config-file.h>
#include <util/dstr.h>
#include <util/util.hpp>

#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

#include <algorithm>

#include "moc_ReplayDock.cpp"

namespace {
constexpr const char *CONFIG_SECTION = "OtakuReplay";
} // namespace

ReplayDock::ReplayDock(QWidget *parent) : OBSDock(parent)
{
	setObjectName(QStringLiteral("replayDock"));
	setWindowTitle(QTStr("Replay.Title"));

	auto *content = new QWidget(this);
	content->setObjectName(QStringLiteral("replayContent"));
	auto *layout = new QVBoxLayout(content);
	layout->setContentsMargins(8, 8, 8, 8);
	layout->setSpacing(8);

	auto *card = new QFrame(content);
	card->setObjectName(QStringLiteral("replayCard"));
	auto *cardLayout = new QVBoxLayout(card);
	cardLayout->setContentsMargins(12, 10, 12, 10);
	cardLayout->setSpacing(6);
	auto *title = new QLabel(QTStr("Replay.HeaderTitle"), card);
	title->setObjectName(QStringLiteral("replayTitle"));
	auto *hint = new QLabel(QTStr("Replay.HeaderBody"), card);
	hint->setObjectName(QStringLiteral("replayHint"));
	hint->setWordWrap(true);
	cardLayout->addWidget(title);
	cardLayout->addWidget(hint);

	auto *grid = new QGridLayout();
	grid->setSpacing(6);
	for (size_t i = 0; i < LENGTHS.size(); i++) {
		const int seconds = LENGTHS[i];
		auto *button = new QPushButton(QTStr("Replay.Seconds").arg(seconds), card);
		button->setObjectName(QStringLiteral("replayClipButton"));
		button->setToolTip(QTStr("Replay.Clip.Tooltip").arg(seconds));
		connect(button, &QPushButton::clicked, this, [this, seconds]() { RequestClip(seconds); });
		grid->addWidget(button, static_cast<int>(i / 2), static_cast<int>(i % 2));
	}
	cardLayout->addLayout(grid);

	dismissButton = new QPushButton(QTStr("Replay.Dismiss"), card);
	dismissButton->setObjectName(QStringLiteral("replayDismissButton"));
	connect(dismissButton, &QPushButton::clicked, this, &ReplayDock::Dismiss);
	cardLayout->addWidget(dismissButton);

	startButton = new QPushButton(QTStr("Replay.StartBuffer"), card);
	startButton->setObjectName(QStringLiteral("replayStartButton"));
	connect(startButton, &QPushButton::clicked, this, []() { obs_frontend_replay_buffer_start(); });
	cardLayout->addWidget(startButton);

	status = new QLabel(card);
	status->setObjectName(QStringLiteral("replayHint"));
	status->setWordWrap(true);
	cardLayout->addWidget(status);
	layout->addWidget(card);
	layout->addStretch();
	setWidget(content);

	for (size_t i = 0; i < LENGTHS.size(); i++) {
		const std::string name = "OtakuReplay.Clip" + std::to_string(LENGTHS[i]);
		const std::string description = QTStr("Replay.Hotkey.Clip").arg(LENGTHS[i]).toStdString();
		clipHotkeys[i] = obs_hotkey_register_frontend(name.c_str(), description.c_str(), OnHotkey, this);
	}
	dismissHotkey = obs_hotkey_register_frontend("OtakuReplay.Dismiss", QT_TO_UTF8(QTStr("Replay.Hotkey.Dismiss")),
						     OnHotkey, this);
	LoadHotkeys();

	obs_frontend_add_event_callback(OnFrontendEvent, this);
	UpdateStatus();
}

ReplayDock::~ReplayDock()
{
	Shutdown();
}

void ReplayDock::Shutdown()
{
	if (shutDown)
		return;
	shutDown = true;
	obs_frontend_remove_event_callback(OnFrontendEvent, this);
	Dismiss();
	SaveHotkeys();
	for (obs_hotkey_id &id : clipHotkeys) {
		obs_hotkey_unregister(id);
		id = OBS_INVALID_HOTKEY_ID;
	}
	obs_hotkey_unregister(dismissHotkey);
	dismissHotkey = OBS_INVALID_HOTKEY_ID;
}

void ReplayDock::LoadHotkeys()
{
	auto load = [](obs_hotkey_id id, const char *key) {
		const char *json = config_get_string(App()->GetUserConfig(), CONFIG_SECTION, key);
		if (!json || !*json)
			return;
		OBSDataAutoRelease data = obs_data_create_from_json(json);
		OBSDataArrayAutoRelease array = obs_data_get_array(data, "bindings");
		if (array)
			obs_hotkey_load(id, array);
	};
	for (size_t i = 0; i < LENGTHS.size(); i++)
		load(clipHotkeys[i], ("Clip" + std::to_string(LENGTHS[i])).c_str());
	load(dismissHotkey, "Dismiss");
}

void ReplayDock::SaveHotkeys()
{
	auto save = [](obs_hotkey_id id, const char *key) {
		if (id == OBS_INVALID_HOTKEY_ID)
			return;
		OBSDataArrayAutoRelease array = obs_hotkey_save(id);
		OBSDataAutoRelease data = obs_data_create();
		obs_data_set_array(data, "bindings", array);
		config_set_string(App()->GetUserConfig(), CONFIG_SECTION, key, obs_data_get_json(data));
	};
	for (size_t i = 0; i < LENGTHS.size(); i++)
		save(clipHotkeys[i], ("Clip" + std::to_string(LENGTHS[i])).c_str());
	save(dismissHotkey, "Dismiss");
}

void ReplayDock::OnHotkey(void *data, obs_hotkey_id id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	auto *dock = static_cast<ReplayDock *>(data);
	QPointer<ReplayDock> guard(dock);
	int seconds = 0;
	for (size_t i = 0; i < LENGTHS.size(); i++)
		if (dock->clipHotkeys[i] == id)
			seconds = LENGTHS[i];
	/* Hotkeys fire on the hotkey thread; work on the UI thread. */
	QMetaObject::invokeMethod(
		dock,
		[guard, seconds]() {
			if (!guard)
				return;
			if (seconds > 0)
				guard->RequestClip(seconds);
			else
				guard->Dismiss();
		},
		Qt::QueuedConnection);
}

void ReplayDock::OnFrontendEvent(enum obs_frontend_event event, void *data)
{
	auto *dock = static_cast<ReplayDock *>(data);
	switch (event) {
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_SAVED:
		if (dock->pendingSeconds > 0)
			dock->PlaySavedReplay();
		break;
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTED:
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STOPPED:
		dock->UpdateStatus();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
	case OBS_FRONTEND_EVENT_EXIT:
		dock->Dismiss();
		break;
	default:
		break;
	}
}

int ReplayDock::BufferSeconds()
{
	config_t *profile = obs_frontend_get_profile_config();
	if (!profile)
		return 0;
	const char *mode = config_get_string(profile, "Output", "Mode");
	const bool advanced = mode && astrcmpi(mode, "Advanced") == 0;
	return static_cast<int>(config_get_int(profile, advanced ? "AdvOut" : "SimpleOutput", "RecRBTime"));
}

void ReplayDock::UpdateStatus()
{
	const bool active = obs_frontend_replay_buffer_active();
	startButton->setVisible(!active);
	dismissButton->setEnabled(media != nullptr);
	if (!active) {
		status->setText(QTStr("Replay.Status.NoBuffer"));
		status->setProperty("severity", "warning");
	} else if (media) {
		status->setText(QTStr("Replay.Status.Playing").arg(playingSeconds));
		status->setProperty("severity", "live");
	} else {
		const int buffer = BufferSeconds();
		status->setText(buffer < LENGTHS.back() ? QTStr("Replay.Status.ShortBuffer").arg(buffer)
							: QTStr("Replay.Status.Ready").arg(buffer));
		status->setProperty("severity", buffer < LENGTHS.back() ? "warning" : "ok");
	}
	status->style()->unpolish(status);
	status->style()->polish(status);
}

void ReplayDock::RequestClip(int seconds)
{
	if (shutDown)
		return;
	if (!obs_frontend_replay_buffer_active()) {
		UpdateStatus();
		return;
	}
	pendingSeconds = seconds;
	status->setText(QTStr("Replay.Status.Saving"));
	obs_frontend_replay_buffer_save();
}

void ReplayDock::PlaySavedReplay()
{
	const int seconds = pendingSeconds;
	pendingSeconds = 0;
	BPtr<char> path = obs_frontend_get_last_replay();
	if (!path || !*path) {
		UpdateStatus();
		return;
	}
	Dismiss();

	obs_video_info ovi{};
	if (!obs_get_video_info(&ovi))
		return;

	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_bool(settings, "is_local_file", true);
	obs_data_set_string(settings, "local_file", path);
	obs_data_set_bool(settings, "looping", false);
	obs_data_set_bool(settings, "restart_on_activate", false);
	obs_data_set_bool(settings, "close_when_inactive", true);
	media = obs_source_create_private("ffmpeg_source", "otaku_replay_clip", settings);
	if (!media) {
		UpdateStatus();
		return;
	}
	playingSeconds = seconds;
	signal_handler_t *handler = obs_source_get_signal_handler(media);
	signal_handler_connect(handler, "media_started", OnMediaSignal, this);
	signal_handler_connect(handler, "media_ended", OnMediaSignal, this);

	overlayScene = obs_scene_create_private("otaku_replay_overlay");
	obs_sceneitem_t *item = obs_scene_add(overlayScene, media);
	if (item) {
		const vec2 bounds{static_cast<float>(ovi.base_width), static_cast<float>(ovi.base_height)};
		const vec2 pos{ovi.base_width / 2.0f, ovi.base_height / 2.0f};
		obs_sceneitem_set_alignment(item, OBS_ALIGN_CENTER);
		obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
		obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
		obs_sceneitem_set_bounds(item, &bounds);
		obs_sceneitem_set_pos(item, &pos);
	}
	obs_set_output_source(OVERLAY_CHANNEL, obs_scene_get_source(overlayScene));
	blog(LOG_INFO, "Replay: playing last %d s of '%s'", seconds, path.Get());
	UpdateStatus();
}

void ReplayDock::OnMediaSignal(void *data, calldata_t *params)
{
	auto *dock = static_cast<ReplayDock *>(data);
	obs_source_t *source = static_cast<obs_source_t *>(calldata_ptr(params, "source"));
	const bool ended = source && obs_source_media_get_state(source) == OBS_MEDIA_STATE_ENDED;
	QPointer<ReplayDock> guard(dock);
	QMetaObject::invokeMethod(
		dock,
		[guard, ended]() {
			if (!guard)
				return;
			if (ended)
				guard->Dismiss();
			else
				guard->OnMediaStarted();
		},
		Qt::QueuedConnection);
}

void ReplayDock::OnMediaStarted()
{
	if (!media)
		return;
	/* The saved file holds the whole buffer; jump to the last N seconds. */
	const int64_t duration = obs_source_media_get_duration(media);
	const int64_t wanted = static_cast<int64_t>(playingSeconds) * 1000;
	if (duration > wanted)
		obs_source_media_set_time(media, duration - wanted);
}

void ReplayDock::Dismiss()
{
	if (overlayScene) {
		OBSSourceAutoRelease current = obs_get_output_source(OVERLAY_CHANNEL);
		if (current.Get() == obs_scene_get_source(overlayScene))
			obs_set_output_source(OVERLAY_CHANNEL, nullptr);
	}
	if (media) {
		signal_handler_t *handler = obs_source_get_signal_handler(media);
		signal_handler_disconnect(handler, "media_started", OnMediaSignal, this);
		signal_handler_disconnect(handler, "media_ended", OnMediaSignal, this);
	}
	overlayScene = nullptr;
	media = nullptr;
	playingSeconds = 0;
	if (!shutDown)
		UpdateStatus();
}
