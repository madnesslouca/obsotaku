#include "OBSBasic.hpp"
#include <utility/StudioCanvasWorkspace.hpp>
#include <utility/DualCanvasController.hpp>
#include <utility/StudioReplayOutputs.hpp>
#include <components/SourceTree.hpp>

#include <docks/StudioInspectorDock.hpp>
#include <docks/StudioClipsDock.hpp>
#include <docks/StudioAudioDock.hpp>
#include <docks/OutputsDock.hpp>
#include <docks/UnifiedChatDock.hpp>
#include <docks/VerticalPreviewDock.hpp>
#include <docks/StreamInfoDock.hpp>
#include <dialogs/OBSBasicTransform.hpp>
#include <widgets/MultistreamChannelBar.hpp>

#include <QLabel>
#include <components/VolumeControl.hpp>
#include <QMenu>
#include <algorithm>
#include <functional>
#include <QToolButton>
#include <QActionGroup>
#include <QHBoxLayout>
#include <QStyle>
#include <QPushButton>
#include <QComboBox>
#include <QTimer>
#include <QToolBar>
#include <QFileInfo>

static const char *StudioClipSection(config_t *config)
{
	const char *mode = config ? config_get_string(config, "Output", "Mode") : nullptr;
	return mode && strcmp(mode, "Advanced") == 0 ? "AdvOut" : "SimpleOutput";
}

OBSScene OBSBasic::GetStudioEditingScene()
{
	if (studioPortraitEditing && dualCanvasController) {
		OBSScene scene = dualCanvasController->CurrentPortraitScene();
		if (scene) {
			return scene;
		}
	}
	return GetCurrentScene();
}

bool OBSBasic::GetStudioVideoInfo(obs_video_info *info)
{
	OBSScene scene = GetStudioEditingScene();
	OBSCanvasAutoRelease canvas = scene ? obs_source_get_canvas(obs_scene_get_source(scene)) : nullptr;
	return canvas ? obs_canvas_get_video_info(canvas, info) : obs_get_video_info(info);
}

void OBSBasic::SetStudioEditingScene(obs_source_t *source)
{
	if (!source) {
		return;
	}
	OBSCanvasAutoRelease canvas = obs_source_get_canvas(source);
	OBSCanvasAutoRelease mainCanvas = obs_get_main_canvas();
	if (canvas && canvas != mainCanvas) {
		// Undo may refer to a hidden portrait scene. Never put it on Main program.
		OBSDataAutoRelease settings = obs_source_get_private_settings(source);
		OBSSourceAutoRelease mainScene =
			obs_get_source_by_uuid(obs_data_get_string(settings, "otaku_main_scene_uuid"));
		if (mainScene && dualCanvasController && dualCanvasController->PortraitCanvas()) {
			SetCurrentScene(mainScene.Get(), true);
			dualCanvasController->SyncToProgramScene();
			SetStudioPortraitEditing(true);
		}
		return;
	}
	SetStudioPortraitEditing(false);
	SetCurrentScene(source, true);
}

void OBSBasic::SetStudioPortraitEditing(bool portrait)
{
	portrait = portrait && dualCanvasController && bool(dualCanvasController->CurrentPortraitScene());
	const bool changed = studioPortraitEditing != portrait;
	// A round-trip within one event-loop turn must still invalidate the old
	// gesture. The editor's timer cannot observe a transient Main selection.
	if (changed && verticalPreviewDock) {
		verticalPreviewDock->CancelPreviewGesture();
	}
	if (changed && transformWindow) {
		transformWindow->close();
	}
	studioPortraitEditing = portrait;
	setProperty("studioEditingCanvas", portrait ? "portrait" : "main");
	if (studioActiveCanvas) {
		const QSignalBlocker blocker(studioActiveCanvas);
		studioActiveCanvas->setCurrentIndex(portrait ? 1 : 0);
		studioActiveCanvas->setStyleSheet(portrait ? "border: 1px solid #ae8bfa;"
							   : "border: 1px solid #58a6ff;");
	}
	if (changed) {
		ui->sources->RefreshItems();
		UpdateContextBar();
		if (studioInspectorDock) {
			studioInspectorDock->Refresh(GetCurrentSceneItem(), GetAllSelectedSourceItems().size());
		}
	}
}

bool OBSBasic::SetStudioCanvasMode(bool multi)
{
	if (obs_video_active() || obs_frontend_streaming_active() || obs_frontend_recording_active() ||
	    obs_frontend_replay_buffer_active() || obs_frontend_virtualcam_active() || !dualCanvasController) {
		return false;
	}
	auto *config = App()->GetUserConfig();
	const bool had = config_has_user_value(config, "BasicWindow", "StudioMultiCanvas");
	const bool before = !had || config_get_bool(config, "BasicWindow", "StudioMultiCanvas");
	config_set_bool(config, "BasicWindow", "StudioMultiCanvas", multi);
	dualCanvasController->Attach();
	if (multi && !dualCanvasController->CurrentPortraitScene()) {
		if (had) {
			config_set_bool(config, "BasicWindow", "StudioMultiCanvas", before);
		} else {
			config_remove_value(config, "BasicWindow", "StudioMultiCanvas");
		}
		dualCanvasController->Attach();
		SetStudioPortraitPreview(this, before);
		return false;
	}
	if (!multi) {
		SetStudioPortraitEditing(false);
	}
	SetStudioPortraitPreview(this, multi);
	config_save_safe(config, "tmp", nullptr);
	SaveProject();
	return true;
}

void OBSBasic::SetupStudioWorkspace()
{
	studioActiveCanvas = new QComboBox(ui->sourcesDock->widget());
	studioActiveCanvas->setObjectName("studioActiveCanvas");
	studioActiveCanvas->setAccessibleName(QTStr("Studio.Canvas.Active"));
	studioActiveCanvas->addItem(QTStr("Studio.Canvas.MainChoice"), "main");
	studioActiveCanvas->addItem(QTStr("Studio.Canvas.PortraitChoice"), "portrait");
	SetStudioPortraitEditing(false);
	qobject_cast<QVBoxLayout *>(ui->sourcesDock->widget()->layout())->insertWidget(0, studioActiveCanvas);
	connect(studioActiveCanvas, &QComboBox::currentIndexChanged, this,
		[this](int index) { SetStudioPortraitEditing(index == 1); });
	connect(dualCanvasController, &DualCanvasController::portraitSceneChanged, this, [this]() {
		const bool available = bool(dualCanvasController->CurrentPortraitScene());
		studioActiveCanvas->setEnabled(available);
		auto *config = App()->GetUserConfig();
		if (!available && config_has_user_value(config, "BasicWindow", "StudioMultiCanvas") &&
		    !config_get_bool(config, "BasicWindow", "StudioMultiCanvas")) {
			SetStudioPortraitEditing(false);
		} else if (studioPortraitEditing) {
			ui->sources->RefreshItems();
		}
	});
	connect(dualCanvasController, &DualCanvasController::layersChanged, this, [this]() {
		if (studioPortraitEditing) {
			ui->sources->RefreshItems();
		}
		SaveProject();
	});
	connect(verticalPreviewDock, &VerticalPreviewDock::editPortraitRequested, this, [this]() {
		SetStudioPortraitEditing(true);
		// A custom tabbed layout must keep the surface that initiated the
		// gesture visible. Raising a sibling would hide it and cancel the edit.
		if (!tabifiedDockWidgets(verticalPreviewDock).contains(ui->sourcesDock)) {
			ui->sourcesDock->show();
			ui->sourcesDock->raise();
		}
	});
	connect(verticalPreviewDock, &VerticalPreviewDock::portraitItemActivated, this,
		[this](qlonglong id, bool toggle, bool preserve, const QString &ownerUuid) {
			SetStudioPortraitEditing(true);
			OBSScene scene = GetStudioEditingScene();
			if (!studioPortraitEditing || !scene) {
				return;
			}
			// IDs are local to each scene. Resolve the owner through this
			// portrait tree before looking up the child, never by ID alone.
			OBSScene owner;
			std::vector<OBSSceneItem> path;
			std::function<bool(obs_scene_t *, unsigned)> findOwner = [&](obs_scene_t *candidate,
										     unsigned depth) {
				if (!candidate || depth > 64) {
					return false;
				}
				if (ownerUuid.isEmpty() || ownerUuid == QString::fromUtf8(obs_source_get_uuid(
										obs_scene_get_source(candidate)))) {
					owner = candidate;
					return true;
				}
				struct Search {
					std::function<bool(obs_scene_t *, unsigned)> *find;
					std::vector<OBSSceneItem> *path;
					unsigned depth;
					bool found = false;
				} search{&findOwner, &path, depth};
				obs_scene_enum_items(
					candidate,
					[](obs_scene_t *, obs_sceneitem_t *item, void *opaque) {
						auto &state = *static_cast<Search *>(opaque);
						if (!obs_sceneitem_is_group(item)) {
							return true;
						}
						state.path->emplace_back(item);
						if ((*state.find)(obs_sceneitem_group_get_scene(item),
								  state.depth + 1)) {
							state.found = true;
							return false;
						}
						state.path->pop_back();
						return true;
					},
					&search);
				return search.found;
			};
			if (!findOwner(scene, 0)) {
				return;
			}
			for (const auto &group : path) {
				OBSDataAutoRelease data = obs_sceneitem_get_private_settings(group);
				if (obs_data_get_bool(data, "collapsed")) {
					obs_data_set_bool(data, "collapsed", false);
				}
			}
			if (!path.empty()) {
				ui->sources->RefreshItems();
			}
			if (!toggle && !preserve) {
				ui->sources->clearSelection();
			}
			if (auto *item = obs_scene_find_sceneitem_by_id(owner, id)) {
				ui->sources->SelectItem(item, !toggle || !obs_sceneitem_selected(item));
			}
			if (!tabifiedDockWidgets(verticalPreviewDock).contains(studioInspectorDock)) {
				studioInspectorDock->show();
				studioInspectorDock->raise();
			}
		});
	studioAudioDock = new StudioAudioDock(this);
	AddDockWidget(studioAudioDock, Qt::RightDockWidgetArea);
	studioAudioDock->hide();
	studioClipsDock = new StudioClipsDock(this);
	AddDockWidget(studioClipsDock, Qt::RightDockWidgetArea);
	studioClipsDock->hide();
	connect(studioClipsDock, &StudioClipsDock::insertRequested, this, &OBSBasic::AddStudioReplaySource);
	studioInspectorDock = new StudioInspectorDock(this);
	AddDockWidget(studioInspectorDock, Qt::RightDockWidgetArea);
	studioInspectorDock->hide();
	connect(studioInspectorDock, &StudioInspectorDock::propertiesRequested, this,
		&OBSBasic::on_actionSourceProperties_triggered);
	connect(studioInspectorDock, &StudioInspectorDock::filtersRequested, this, [this]() { OpenFilters(); });
	connect(studioInspectorDock, &StudioInspectorDock::transformRequested, this, [this]() { OpenEditTransform(); });

	studioToolbar = new QToolBar(QTStr("Studio.Workspace"), this);
	studioToolbar->setObjectName(QStringLiteral("studioToolbar"));
	studioToolbar->setMovable(false);
	studioToolbar->setFloatable(false);
	studioToolbar->addWidget(new QLabel(QStringLiteral("Studio"), studioToolbar));
	auto *spacer = new QWidget(studioToolbar);
	spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	studioToolbar->addWidget(spacer);
	auto *stream = new QPushButton(studioToolbar);
	stream->setObjectName(QStringLiteral("studioStreamButton"));
	auto *record = new QPushButton(studioToolbar);
	record->setObjectName(QStringLiteral("studioRecordButton"));
	auto *clip = new QToolButton(studioToolbar);
	clip->setObjectName(QStringLiteral("studioClipButton"));
	clip->setText(QStringLiteral("Clip"));
	clip->setPopupMode(QToolButton::MenuButtonPopup);
	auto *clipMenu = new QMenu(clip);
	auto *bufferAction = clipMenu->addAction(QTStr("Studio.ClipEnable"));
	bufferAction->setObjectName(QStringLiteral("studioClipBufferAction"));
	bufferAction->setCheckable(true);
	connect(bufferAction, &QAction::triggered, this, &OBSBasic::ToggleStudioClipBuffer);
	for (const QString canvas : {QString("main"), QString("portrait")}) {
		auto *menu = clipMenu->addMenu(QTStr(canvas == "main" ? "Studio.Clip.Main" : "Studio.Clip.Portrait"));
		auto *capture = menu->addAction(QTStr("Studio.ClipEnable"));
		capture->setCheckable(true);
		capture->setObjectName("studioClipCapture_" + canvas);
		auto *save = menu->addAction(QTStr("Studio.Clip.Save"));
		save->setObjectName("studioClipSave_" + canvas);
		auto *force = menu->addAction(QTStr("Studio.Clip.ForceStop"));
		force->setObjectName("studioClipForceStop_" + canvas);
		force->setVisible(false);
		force->setToolTip(QTStr("Studio.Clip.ForceStopHint"));
		auto *state = menu->addAction(QString());
		state->setEnabled(false);
		connect(capture, &QAction::triggered, this, [this, canvas](bool enabled) {
			if (!outputHandler || !outputHandler->replayOutputs) return;
			if (enabled) outputHandler->replayOutputs->StartTargets(canvas);
			else outputHandler->replayOutputs->StopTargets(canvas);
		});
		connect(save, &QAction::triggered, this, [this, canvas]() {
			if (outputHandler && outputHandler->replayOutputs) outputHandler->replayOutputs->SaveTargets(canvas);
		});
		connect(force, &QAction::triggered, this, [this, canvas]() {
			if (outputHandler && outputHandler->replayOutputs) outputHandler->replayOutputs->StopTargets(canvas, true);
		});
		connect(menu, &QMenu::aboutToShow, this, [this, canvas, capture, save, force, state]() {
			const QJsonObject info = outputHandler && outputHandler->replayOutputs
				? outputHandler->replayOutputs->Snapshot().value("buffers").toObject().value(canvas).toObject() : QJsonObject{};
			const bool active = info.value("active").toBool();
			const bool stopping = info.value("stopping").toBool();
			const bool saving = info.value("saving").toBool();
			QSignalBlocker block(capture);
			capture->setChecked(active);
			capture->setEnabled(!stopping);
			capture->setText(QTStr(active ? "Studio.ClipDisable" : "Studio.ClipEnable"));
			save->setEnabled(active && !stopping && !saving);
			force->setVisible(stopping);
			force->setEnabled(stopping);
			state->setText(QTStr(stopping ? "Studio.Clip.Stopping" : saving ? "Studio.Clip.Saving" : active ? "Studio.Clip.Capturing" : "Studio.Clip.Idle"));
			state->setToolTip(info.value("lastError").toString());
		});
	}
	auto *durationMenu = clipMenu->addMenu(QTStr("Studio.ClipDuration"));
	auto *durations = new QActionGroup(durationMenu);
	for (int seconds : {10, 20, 30, 60, 120}) {
		auto *duration = durationMenu->addAction(QStringLiteral("%1 s").arg(seconds));
		duration->setCheckable(true);
		duration->setData(seconds);
		durations->addAction(duration);
		connect(duration, &QAction::triggered, this, [this, seconds]() {
			if (!outputHandler || outputHandler->ReplayBufferActive()) {
				return;
			}
			const char *section = StudioClipSection(Config());
			config_set_int(Config(), section, "RecRBTime", seconds);
			config_save_safe(Config(), "tmp", nullptr);
		});
	}
	clip->setMenu(clipMenu);
	clipMenu->addSeparator();
	auto *library = clipMenu->addAction(QTStr("Studio.Clips"));
	library->setObjectName(QStringLiteral("studioClipLibraryAction"));
	connect(library, &QAction::triggered, this, [this]() {
		studioClipsDock->show();
		studioClipsDock->raise();
	});
	connect(clip, &QToolButton::clicked, this, [this, clip, clipMenu]() {
		if (ReplayBufferActive()) {
			ReplayBufferSave();
		} else {
			clipMenu->popup(clip->mapToGlobal(QPoint(0, clip->height())));
		}
	});
	auto *metrics = new QLabel(studioToolbar);
	metrics->setObjectName(QStringLiteral("studioHeaderMetrics"));
	studioToolbar->addWidget(metrics);
	auto *audio = new QPushButton(QTStr("Studio.Audio"), studioToolbar);
	audio->setObjectName(QStringLiteral("studioAudioRoutingButton"));
	studioToolbar->addWidget(audio);
	connect(audio, &QPushButton::clicked, this, [this]() { OpenStudioAudioRouting(); });
	auto *widgets = new QToolButton(studioToolbar);
	widgets->setObjectName(QStringLiteral("studioWidgetsButton"));
	widgets->setText(QTStr("Studio.Widgets"));
	widgets->setPopupMode(QToolButton::InstantPopup);
	auto *widgetMenu = new QMenu(widgets);
	auto *addWidget = widgetMenu->addAction(QTStr("Studio.WidgetAdd"));
	addWidget->setObjectName(QStringLiteral("studioWidgetAddAction"));
	auto *editWidget = widgetMenu->addAction(QTStr("Studio.WidgetEdit"));
	editWidget->setObjectName(QStringLiteral("studioWidgetEditAction"));
	connect(addWidget, &QAction::triggered, this, [this]() { ShowStudioWidgetEditor(false); });
	connect(editWidget, &QAction::triggered, this, [this]() { ShowStudioWidgetEditor(true); });
	connect(widgetMenu, &QMenu::aboutToShow, this, [this, addWidget, editWidget]() {
		const bool available = obs_source_get_display_name("browser_source") != nullptr;
		addWidget->setEnabled(available);
		OBSSceneItem item = GetCurrentSceneItem();
		OBSDataAutoRelease settings = item ? obs_source_get_settings(obs_sceneitem_get_source(item))
						   : obs_data_create();
		editWidget->setEnabled(available && obs_data_get_bool(settings, "studio_widget"));
	});
	widgets->setMenu(widgetMenu);
	studioToolbar->addWidget(widgets);
	studioToolbar->addWidget(stream);
	studioToolbar->addWidget(record);
	studioToolbar->addWidget(clip);
	addToolBar(Qt::TopToolBarArea, studioToolbar);
	studioToolbar->hide();
	connect(stream, &QPushButton::clicked, this, &OBSBasic::StreamActionTriggered);
	connect(record, &QPushButton::clicked, this, &OBSBasic::RecordActionTriggered);
	studioSidebar = new QToolBar(QTStr("Studio.Inspector"), this);
	studioSidebar->setObjectName(QStringLiteral("studioSidebar"));
	studioSidebar->setMovable(false);
	studioSidebar->setFloatable(false);
	studioSidebar->setFixedWidth(34);
	auto *properties = studioSidebar->addAction(QStringLiteral("◇"));
	properties->setToolTip(QTStr("Studio.Inspector"));
	connect(properties, &QAction::triggered, this, [this]() {
		studioInspectorDock->show();
		studioInspectorDock->raise();
	});
	auto *outputs = studioSidebar->addAction(QStringLiteral("◎"));
	outputs->setObjectName("studioOutputsAction");
	outputs->setToolTip(QTStr("Studio.Outputs"));
	connect(outputs, &QAction::triggered, this, [this]() {
		outputsDock->show();
		outputsDock->raise();
		auto *config = App()->GetUserConfig();
		const bool multi = !config_has_user_value(config, "BasicWindow", "StudioMultiCanvas") ||
				   config_get_bool(config, "BasicWindow", "StudioMultiCanvas");
		if (multi) {
			SetStudioPortraitPreview(this, true);
		}
	});
	auto *chat = studioSidebar->addAction(QStringLiteral("▤"));
	chat->setToolTip(QTStr("Studio.Chat"));
	connect(chat, &QAction::triggered, this, [this]() {
		unifiedChatDock->show();
		unifiedChatDock->raise();
	});
	auto *railSpacer = new QWidget(studioSidebar);
	railSpacer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
	studioSidebar->addWidget(railSpacer);
	auto *settings = studioSidebar->addAction(QStringLiteral("⚙"));
	settings->setToolTip(QTStr("Settings"));
	connect(settings, &QAction::triggered, this, &OBSBasic::on_action_Settings_triggered);
	addToolBar(Qt::RightToolBarArea, studioSidebar);
	studioSidebar->hide();

	/* This observes native state; it does not own sources or start outputs.
	 * Polling only while visible also covers changes made through plugins. */
	auto *timer = new QTimer(this);
	timer->setInterval(250);
	connect(timer, &QTimer::timeout, this,
		[this, stream, record, clip, bufferAction, durations, durationMenu, metrics]() {
			if (isClosing() || !Config()) {
				return;
			}
			if (studioInspectorDock->isVisible()) {
				OBSSceneItem item = GetCurrentSceneItem();
				studioInspectorDock->Refresh(item, GetAllSelectedSourceItems().size());
			}
			if (studioToolbar->isVisible()) {
				stream->setText(QTStr(StreamingActive() ? "Studio.StopLive" : "Studio.GoLive"));
				record->setText(QTStr(RecordingActive() ? "Studio.StopRecord" : "Studio.Record"));
				const bool active = ReplayBufferActive();
				QSignalBlocker blocker(bufferAction);
				bufferAction->setChecked(active);
				bufferAction->setText(QTStr(active ? "Studio.ClipDisable" : "Studio.ClipEnable"));
				const char *section = StudioClipSection(Config());
				const int seconds = config_get_int(Config(), section, "RecRBTime");
				durationMenu->setEnabled(outputHandler && !outputHandler->ReplayBufferActive());
				for (auto *duration : durations->actions()) {
					duration->setChecked(duration->data().toInt() == seconds);
				}
				clip->setToolTip(
					QTStr(active ? "Studio.ClipSaveHint" : "Studio.ClipEnableHint").arg(seconds));
				clip->setStyleSheet(active ? QStringLiteral("color: #6dde92;") : QString());
				metrics->setText(QStringLiteral("%1 FPS").arg(obs_get_active_fps(), 0, 'f', 0));
			}
		});
	timer->start();

	studioWorkspaceAction = new QAction(QTStr("Studio.Workspace"), this);
	studioWorkspaceAction->setObjectName(QStringLiteral("studioWorkspaceAction"));
	studioWorkspaceAction->setCheckable(true);
	ui->menuDocks->insertAction(ui->resetDocks, studioWorkspaceAction);
	connect(studioWorkspaceAction, &QAction::toggled, this, &OBSBasic::SetStudioWorkspaceEnabled);
	/* Hide the stacked optional docks before the first show(), otherwise Qt
	 * expands a new window to the sum of all their minimum heights. */
	if (config_get_bool(App()->GetUserConfig(), "BasicWindow", "StudioWorkspace") &&
	    !config_has_user_value(App()->GetUserConfig(), "BasicWindow", "DockState")) {
		studioWorkspaceAction->setChecked(true);
		studioLayoutSeeded = true;
		/* Core docks are deliberately hidden during construction. Restoring
		 * that transient snapshot would leave a new user's workspace empty. */
		config_set_string(App()->GetUserConfig(), "BasicWindow", "BeforeStudioDockState",
				  startingDockLayout.toBase64().constData());
		config_set_bool(App()->GetUserConfig(), "BasicWindow", "BeforeStudioSideDocks", true);
	}
}

void OBSBasic::OpenStudioAudioRouting(const QString &uuid)
{
	// A short stacked dock hides the controls that the toolbar promises to open.
	// Share the inspector's full-height area, while preserving floating/custom docks.
	if (!studioAudioDock->isFloating() && dockWidgetArea(studioAudioDock) == Qt::RightDockWidgetArea &&
	    studioAudioDock->height() < 500 && studioInspectorDock->isVisible()) {
		tabifyDockWidget(studioInspectorDock, studioAudioDock);
	}
	studioAudioDock->show();
	studioAudioDock->raise();
	QString selected = uuid;
	if (selected.isEmpty()) {
		OBSSceneItem item = GetCurrentSceneItem();
		obs_source_t *source = item ? obs_sceneitem_get_source(item) : nullptr;
		if (source && obs_source_get_type(source) == OBS_SOURCE_TYPE_INPUT &&
		    (obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO)) {
			selected = QString::fromUtf8(obs_source_get_uuid(source));
		}
	}
	studioAudioDock->SelectSource(selected);
}

void OBSBasic::SetStudioWorkspaceEnabled(bool enabled)
{
	if (!enabled) {
		SetStudioPortraitEditing(false);
	}
	if (studioActiveCanvas) {
		studioActiveCanvas->setVisible(enabled);
	}
	auto *config = App()->GetUserConfig();
	if (enabled) {
		config_set_string(config, "BasicWindow", "BeforeStudioDockState", saveState().toBase64().constData());
		config_set_bool(config, "BasicWindow", "BeforeStudioSideDocks", ui->sideDocks->isChecked());
		/* Remove the affected docks first, so the layout is deterministic even
		 * when a user previously floated or tabified them. Extra docks stay put. */
		QList<QDockWidget *> docks{ui->sourcesDock,     ui->scenesDock, ui->mixerDock,
					   ui->transitionsDock, controlsDock,   studioInspectorDock,
					   unifiedChatDock,     outputsDock,    verticalPreviewDock,
					   streamInfoDock};
		for (auto *dock : docks) {
			if (!dock) {
				continue;
			}
			dock->setFloating(false);
			removeDockWidget(dock);
		}
		ui->sideDocks->setChecked(true);
		setDockCornersVertical(true);
		addDockWidget(Qt::LeftDockWidgetArea, ui->sourcesDock);
		splitDockWidget(ui->sourcesDock, ui->scenesDock, Qt::Vertical);
		addDockWidget(Qt::BottomDockWidgetArea, ui->mixerDock);
		addDockWidget(Qt::RightDockWidgetArea, studioInspectorDock);
		addDockWidget(Qt::RightDockWidgetArea, outputsDock);
		tabifyDockWidget(studioInspectorDock, outputsDock);
		for (auto *dock : {static_cast<QDockWidget *>(unifiedChatDock.data()),
				   static_cast<QDockWidget *>(verticalPreviewDock.data()),
				   static_cast<QDockWidget *>(streamInfoDock.data()),
				   static_cast<QDockWidget *>(controlsDock.data()),
				   static_cast<QDockWidget *>(ui->transitionsDock)}) {
			addDockWidget(Qt::RightDockWidgetArea, dock);
			tabifyDockWidget(studioInspectorDock, dock);
			dock->hide();
		}
		ui->sourcesDock->show();
		ui->scenesDock->show();
		ui->mixerDock->show();
		studioInspectorDock->show();
		outputsDock->show();
		studioInspectorDock->raise();
		if (!config_has_user_value(config, "BasicWindow", "StudioMultiCanvas") ||
		    config_get_bool(config, "BasicWindow", "StudioMultiCanvas")) {
			SetStudioPortraitPreview(this, true);
		}
		resizeDocks({ui->sourcesDock, ui->scenesDock, studioInspectorDock}, {200, 200, 320}, Qt::Horizontal);
		resizeDocks({ui->sourcesDock, ui->scenesDock}, {height() * 46 / 100, height() * 46 / 100},
			    Qt::Vertical);
		resizeDocks({ui->mixerDock}, {std::clamp(height() / 3, 340, 380)}, Qt::Vertical);
		QTimer::singleShot(0, this, [this]() {
			if (studioWorkspaceAction->isChecked()) {
				resizeDocks({ui->mixerDock}, {std::clamp(height() / 3, 340, 380)}, Qt::Vertical);
			}
		});
	} else {
		const bool side = config_get_bool(config, "BasicWindow", "BeforeStudioSideDocks");
		ui->sideDocks->setChecked(side);
		setDockCornersVertical(side);
		const char *saved = config_get_string(config, "BasicWindow", "BeforeStudioDockState");
		if (saved && *saved) {
			restoreState(QByteArray::fromBase64(saved));
		}
		studioInspectorDock->hide();
		studioClipsDock->hide();
	}
	studioToolbar->setVisible(enabled);
	ApplyStudioChrome(enabled);
	ui->sourcesDock->setWindowTitle(QTStr(enabled ? "Studio.Layers" : "Basic.Main.Sources"));
	config_set_bool(config, "BasicWindow", "StudioWorkspace", enabled);
	if (enabled) {
		config_set_int(config, "BasicWindow", "StudioLayoutVersion", 2);
	}
	config_save_safe(config, "tmp", nullptr);
}

void OBSBasic::AddStudioReplaySource(const QString &path, bool followLatest)
{
	OBSScene scene = GetStudioEditingScene();
	if (!scene || !QFileInfo(path).isFile()) {
		return;
	}
	obs_video_info video{};
	if (!GetStudioVideoInfo(&video) || !video.base_width || !video.base_height) {
		return;
	}
	const char *type = obs_get_latest_input_type_id("ffmpeg_source");
	if (!type) {
		return;
	}
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_bool(settings, "is_local_file", true);
	obs_data_set_string(settings, "local_file", path.toUtf8().constData());
	obs_data_set_bool(settings, "looping", false);
	obs_data_set_bool(settings, "clear_on_media_end", false);
	obs_data_set_bool(settings, "restart_on_activate", true);
	obs_data_set_bool(settings, "close_when_inactive", false);
	obs_data_set_int(settings, "speed_percent", 100);
	QString candidate = QTStr("Studio.ClipSource");
	for (int suffix = 2;; ++suffix) {
		OBSSourceAutoRelease existing = obs_get_source_by_name(candidate.toUtf8().constData());
		if (!existing) {
			break;
		}
		candidate = QStringLiteral("%1 (%2)").arg(QTStr("Studio.ClipSource")).arg(suffix);
	}
	const std::string name = candidate.toUtf8().constData();
	OBSSourceAutoRelease source = obs_source_create(type, name.c_str(), settings, nullptr);
	if (!source) {
		return;
	}
	OBSDataAutoRelease metadata = obs_source_get_private_settings(source);
	obs_data_set_bool(metadata, "studio_replay_managed", true);
	obs_data_set_bool(metadata, "studio_replay_follow", followLatest);
	const QString originCanvas = studioClipsDock ? studioClipsDock->CanvasForClip(path) : QStringLiteral("main");
	obs_data_set_string(metadata, "studio_replay_canvas", originCanvas.toUtf8().constData());
	obs_data_set_bool(metadata, "studio_replay_loop", false);
	obs_data_set_int(metadata, "studio_replay_a", 0);
	obs_data_set_int(metadata, "studio_replay_b", 0);
	obs_transform_info transform{};
	transform.scale = {1.0f, 1.0f};
	transform.alignment = OBS_ALIGN_TOP | OBS_ALIGN_LEFT;
	transform.bounds_type = OBS_BOUNDS_SCALE_INNER;
	transform.bounds_alignment = OBS_ALIGN_CENTER;
	transform.bounds = {float(video.base_width), float(video.base_height)};
	auto *item = obs_scene_add(scene, source);
	if (!item) {
		return;
	}
	obs_sceneitem_set_info2(item, &transform);
	OBSDataAutoRelease wrapper = obs_save_source(source);
	const std::string sceneUUID = obs_source_get_uuid(obs_scene_get_source(scene));
	const std::string sourceUUID = obs_source_get_uuid(source);
	auto undo = [sourceUUID, sceneUUID](const std::string &) {
		OBSSourceAutoRelease source = obs_get_source_by_uuid(sourceUUID.c_str());
		if (source) {
			obs_source_remove(source);
		}
		OBSSourceAutoRelease scene = obs_get_source_by_uuid(sceneUUID.c_str());
		if (scene) {
			OBSBasic::Get()->SetStudioEditingScene(scene.Get());
		}
	};
	auto redo = [sceneUUID, transform](const std::string &json) {
		OBSSourceAutoRelease scene = obs_get_source_by_uuid(sceneUUID.c_str());
		if (!scene || !obs_scene_from_source(scene)) {
			return;
		}
		OBSBasic::Get()->SetStudioEditingScene(scene.Get());
		OBSDataAutoRelease data = obs_data_create_from_json(json.c_str());
		OBSSourceAutoRelease source = obs_load_source(data);
		if (source) {
			if (auto *item = obs_scene_add(obs_scene_from_source(scene), source)) {
				obs_sceneitem_set_info2(item, &transform);
			}
		}
	};
	undo_s.add_action(QTStr("Undo.Add").arg(QString::fromUtf8(name.c_str())), undo, redo, "",
			  obs_data_get_json(wrapper));
	SaveProject();
}

void OBSBasic::RestoreStudioWorkspace()
{
	const bool enabled = config_get_bool(App()->GetUserConfig(), "BasicWindow", "StudioWorkspace");
	QSignalBlocker blocker(studioWorkspaceAction);
	studioWorkspaceAction->setChecked(enabled);
	studioToolbar->setVisible(enabled);
	ApplyStudioChrome(enabled);
	ui->sourcesDock->setWindowTitle(QTStr(enabled ? "Studio.Layers" : "Basic.Main.Sources"));
	if (enabled && config_get_int(App()->GetUserConfig(), "BasicWindow", "StudioLayoutVersion") < 2) {
		const char *saved = config_get_string(App()->GetUserConfig(), "BasicWindow", "BeforeStudioDockState");
		const std::string previous = saved ? saved : "";
		const bool side = config_get_bool(App()->GetUserConfig(), "BasicWindow", "BeforeStudioSideDocks");
		SetStudioWorkspaceEnabled(true);
		config_set_string(App()->GetUserConfig(), "BasicWindow", "BeforeStudioDockState", previous.c_str());
		config_set_bool(App()->GetUserConfig(), "BasicWindow", "BeforeStudioSideDocks", side);
	}
	/* DockState already restores placement and visibility. Don't overwrite a
	 * customized Studio arrangement or the saved previous layout on startup. */
	if (enabled && !studioLayoutSeeded &&
	    !config_has_user_value(App()->GetUserConfig(), "BasicWindow", "DockState")) {
		SetStudioWorkspaceEnabled(true);
	}
}

void OBSBasic::ApplyStudioChrome(bool enabled)
{
	studioSidebar->setVisible(enabled);
	multistreamChannelBar->setVisible(!enabled);
	ui->contextContainer->setVisible(!enabled &&
					 config_get_bool(App()->GetUserConfig(), "BasicWindow", "ShowContextToolbars"));
	ui->sourcesToolbar->setVisible(!enabled &&
				       config_get_bool(App()->GetUserConfig(), "BasicWindow", "ShowListboxToolbars"));
	ui->scenesToolbar->setVisible(!enabled &&
				      config_get_bool(App()->GetUserConfig(), "BasicWindow", "ShowListboxToolbars"));
	ui->previewXContainer->setVisible(!enabled);
	if (auto *mixer = ui->mixerDock->widget()) {
		for (auto *bar : mixer->findChildren<QToolBar *>()) {
			bar->setVisible(!enabled);
		}
	}
	auto header = [this, enabled](QDockWidget *dock, std::function<void()> add) {
		if (enabled && !dock->titleBarWidget()) {
			auto *bar = new QWidget(dock);
			bar->setObjectName(QStringLiteral("studioDockHeader"));
			auto *row = new QHBoxLayout(bar);
			row->setContentsMargins(10, 0, 6, 0);
			row->setSpacing(4);
			auto *title = new QLabel(dock->windowTitle(), bar);
			row->addWidget(title, 1);
			connect(dock, &QDockWidget::windowTitleChanged, title, &QLabel::setText);
			if (add) {
				auto *button = new QToolButton(bar);
				button->setText(QStringLiteral("+"));
				button->setToolTip(QTStr("Add"));
				row->addWidget(button);
				connect(button, &QToolButton::clicked, this, add);
			}
			dock->setTitleBarWidget(bar);
		} else if (!enabled && dock->titleBarWidget()) {
			auto *bar = dock->titleBarWidget();
			dock->setTitleBarWidget(nullptr);
			bar->deleteLater();
		}
	};
	header(ui->sourcesDock, [this]() { ui->actionAddSource->trigger(); });
	header(ui->scenesDock, [this]() { on_actionAddScene_triggered(); });
	header(ui->mixerDock, [this]() { ui->actionAddSource->trigger(); });
	header(studioInspectorDock, {});
	header(outputsDock, {});
	setProperty("studioWorkspace", enabled);
	for (auto *control : findChildren<VolumeControl *>()) {
		control->setStudioStyle(enabled);
	}
	drawSpacingHelpers = !enabled &&
			     config_get_bool(App()->GetUserConfig(), "BasicWindow", "SpacingHelpersEnabled");
	for (auto *widget : findChildren<QWidget *>()) {
		widget->style()->unpolish(widget);
		widget->style()->polish(widget);
	}
}

void OBSBasic::ToggleStudioClipBuffer()
{
	if (ReplayBufferActive()) {
		StopReplayBuffer();
		return;
	}
	if (!outputHandler) {
		return;
	}
	const char *section = StudioClipSection(Config());
	config_set_bool(Config(), section, "RecRB", true);
	config_save_safe(Config(), "tmp", nullptr);
	// Independent buffers may be enabled while recording or streaming. Resetting
	// the whole output handler here would disrupt those outputs.
	StartReplayBuffer();
}

void OBSBasic::SetStudioAudioLinked(const QString &uuid, bool linked)
{
	studioAudioDock->SetSceneLinked(uuid, linked);
}
