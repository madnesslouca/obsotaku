/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <docks/OBSDock.hpp>
#include <utility/MultiStreamManager.hpp>

#include <atomic>
#include <mutex>
#include <vector>
#include <functional>

class DualCanvasController;
class PortraitPreviewEditor;
class QListWidget;
class QListWidgetItem;
class QToolButton;

class QComboBox;
class QLabel;
class QPushButton;
class QStackedWidget;
class QSpinBox;
class OBSQTDisplay;

/* A phone-shaped monitor and editor for the vertical (9:16) canvas.
 *
 * With dual-canvas scenes (DualCanvasController attached) it renders the
 * persistent vertical canvas and edits the layers of the vertical scene that
 * is linked to the current program scene. Without it, it falls back to the
 * old behaviour: a reframed preview of the horizontal program. */
class VerticalPreviewDock : public OBSDock {
	Q_OBJECT

public:
	explicit VerticalPreviewDock(QWidget *parent = nullptr);
	~VerticalPreviewDock() override;

	void SetChannels(const std::vector<MultiStreamChannel> &channels);
	void SetDualCanvas(DualCanvasController *controller);
	void Shutdown();
	void CancelPreviewGesture();

signals:
	void editPortraitRequested();
	void portraitItemActivated(qlonglong id, bool toggle = false, bool preserve = false,
				   const QString &ownerUuid = {});
	void addChannelRequested();
	void editChannelRequested(const QString &channelId);
	void portraitFitChanged(const QString &channelId, MultiStreamPortraitFit fit);

private:
	bool eventFilter(QObject *object, QEvent *event) override;
	static void RenderPreview(void *data, uint32_t width, uint32_t height);
	void OnDisplayCreated();
	void OnChannelChanged(int index);
	void OnFramingChanged(int index);
	void UpdateControls();
	void RefreshCanvas();
	void RefreshLayers();
	void SyncLayerSelection();
	void RefreshGroupScope();
	void RefreshTransitionControls();
	void OnTransitionChanged();
	void OnLayerChanged(QListWidgetItem *item);
	void ShowAddMenu();
	void RunSceneCommand(const char *label, const std::function<void(obs_scene_t *)> &change,
			     const std::string &expectedScene = {});
	void RemoveSelectedLayer();
	void MoveSelectedLayer(bool up);
	void FitSelectedLayer();
	OBSSceneItem SelectedItem() const;
	bool DualCanvasActive() const;

	OBSQTDisplay *preview = nullptr;
	PortraitPreviewEditor *previewEditor = nullptr;
	QStackedWidget *previewStack = nullptr;
	QComboBox *channelCombo = nullptr;
	QComboBox *framingCombo = nullptr;
	QLabel *statusLabel = nullptr;
	QPushButton *editButton = nullptr;
	QLabel *sceneBadge = nullptr;
	QLabel *sceneName = nullptr;
	QWidget *layersPanel = nullptr;
	QListWidget *layersList = nullptr;
	QToolButton *addLayerButton = nullptr;
	QToolButton *removeLayerButton = nullptr;
	QToolButton *upLayerButton = nullptr;
	QToolButton *downLayerButton = nullptr;
	QToolButton *fitLayerButton = nullptr;
	QToolButton *cropButton = nullptr;
	QToolButton *mirrorButton = nullptr;
	QWidget *groupScopeRow = nullptr;
	QToolButton *exitGroupButton = nullptr;
	QLabel *groupScopeLabel = nullptr;
	QWidget *transitionPanel = nullptr;
	QComboBox *transitionMode = nullptr;
	QSpinBox *transitionDuration = nullptr;
	QLabel *transitionHint = nullptr;
	DualCanvasController *dualCanvas = nullptr;
	/* Read by the render callback on the graphics thread. */
	mutable std::mutex canvasMutex;
	OBSWeakCanvas renderCanvas;
	std::vector<MultiStreamChannel> portraitChannels;
	std::atomic<int> activeFit{static_cast<int>(MultiStreamPortraitFit::Fill)};
	std::atomic_bool shuttingDown{false};
	bool refreshing = false;
	bool drawCallbackRegistered = false;
};
