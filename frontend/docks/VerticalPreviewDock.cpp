/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "VerticalPreviewDock.hpp"

#include <utility/DualCanvasController.hpp>
#include <utility/PortraitPreviewEditor.hpp>
#include <utility/StudioSceneEdit.hpp>
#include <widgets/OBSBasic.hpp>
#include <utility/StreamPlatformDisplay.hpp>
#include <widgets/OBSQTDisplay.hpp>

#include <qt-wrappers.hpp>

#include <graphics/graphics.h>

#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QToolButton>
#include <QTimer>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>

#include "moc_VerticalPreviewDock.cpp"

namespace {
constexpr float PORTRAIT_WIDTH = 1080.0f;
constexpr float PORTRAIT_HEIGHT = 1920.0f;

QString ChannelLabel(const MultiStreamChannel &channel)
{
	return QStringLiteral("%1 · %2").arg(QString::fromStdString(channel.displayName),
					     StreamPlatformDisplayName(channel.platform));
}
} // namespace

VerticalPreviewDock::VerticalPreviewDock(QWidget *parent) : OBSDock(parent)
{
	setObjectName(QStringLiteral("verticalPreviewDock"));
	setWindowTitle(QTStr("Multistream.Vertical.Title"));

	auto *content = new QWidget(this);
	content->setObjectName(QStringLiteral("verticalPreviewContent"));
	auto *layout = new QVBoxLayout(content);
	layout->setContentsMargins(8, 8, 8, 8);
	layout->setSpacing(8);

	auto *header = new QFrame(content);
	header->setObjectName(QStringLiteral("verticalPreviewHeader"));
	auto *headerLayout = new QVBoxLayout(header);
	headerLayout->setContentsMargins(12, 10, 12, 10);
	headerLayout->setSpacing(3);
	auto *title = new QLabel(QTStr("Multistream.Vertical.HeaderTitle"), header);
	title->setObjectName(QStringLiteral("verticalPreviewHeaderTitle"));
	auto *description = new QLabel(QTStr("Multistream.Vertical.HeaderBody"), header);
	description->setObjectName(QStringLiteral("verticalPreviewHint"));
	description->setWordWrap(true);
	auto *sceneRow = new QHBoxLayout();
	sceneRow->setSpacing(6);
	sceneBadge = new QLabel(QStringLiteral("9:16"), header);
	sceneBadge->setObjectName(QStringLiteral("canvasBadge"));
	sceneBadge->setProperty("canvas", QStringLiteral("portrait"));
	sceneName = new QLabel(header);
	sceneName->setObjectName(QStringLiteral("verticalPreviewSceneName"));
	sceneRow->addWidget(sceneBadge);
	sceneRow->addWidget(sceneName, 1);
	headerLayout->addWidget(title);
	headerLayout->addLayout(sceneRow);
	headerLayout->addWidget(description);
	layout->addWidget(header);

	previewStack = new QStackedWidget(content);
	previewStack->setObjectName(QStringLiteral("verticalPreviewStack"));
	preview = new OBSQTDisplay(previewStack);
	preview->setObjectName(QStringLiteral("verticalPreviewDisplay"));
	preview->setProperty("studioPortraitGroupScope", QString{});
	preview->installEventFilter(this);
	previewEditor = new PortraitPreviewEditor(
		preview, [this]() -> OBSScene { return dualCanvas ? dualCanvas->CurrentPortraitScene() : OBSScene{}; },
		[this](const QString &ownerUuid, int64_t id, bool toggle, bool preserve) {
			emit editPortraitRequested();
			emit portraitItemActivated(id, toggle, preserve, ownerUuid);
			SyncLayerSelection();
			RefreshGroupScope();
		});
	preview->setMinimumSize(180, 320);
	preview->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	preview->SetDisplayBackgroundColor(Qt::black);
	previewStack->addWidget(preview);

	auto *empty = new QFrame(previewStack);
	empty->setObjectName(QStringLiteral("verticalPreviewEmpty"));
	auto *emptyLayout = new QVBoxLayout(empty);
	emptyLayout->setContentsMargins(24, 24, 24, 24);
	emptyLayout->setSpacing(8);
	emptyLayout->addStretch();
	auto *phone = new QLabel(QStringLiteral("9:16"), empty);
	phone->setObjectName(QStringLiteral("verticalPreviewEmptyIcon"));
	phone->setAlignment(Qt::AlignCenter);
	auto *emptyTitle = new QLabel(QTStr("Multistream.Vertical.EmptyTitle"), empty);
	emptyTitle->setObjectName(QStringLiteral("verticalPreviewEmptyTitle"));
	emptyTitle->setAlignment(Qt::AlignCenter);
	auto *emptyBody = new QLabel(QTStr("Multistream.Vertical.EmptyBody"), empty);
	emptyBody->setObjectName(QStringLiteral("verticalPreviewHint"));
	emptyBody->setAlignment(Qt::AlignCenter);
	emptyBody->setWordWrap(true);
	auto *addButton = new QPushButton(QTStr("Multistream.Vertical.AddTikTok"), empty);
	addButton->setObjectName(QStringLiteral("verticalPreviewPrimaryButton"));
	emptyLayout->addWidget(phone);
	emptyLayout->addWidget(emptyTitle);
	emptyLayout->addWidget(emptyBody);
	emptyLayout->addWidget(addButton, 0, Qt::AlignHCenter);
	emptyLayout->addStretch();
	previewStack->addWidget(empty);
	layout->addWidget(previewStack, 1);
	groupScopeRow = new QWidget(content);
	groupScopeRow->setObjectName("studioPortraitGroupBreadcrumb");
	auto *groupScopeLayout = new QHBoxLayout(groupScopeRow);
	groupScopeLayout->setContentsMargins(0, 0, 0, 0);
	exitGroupButton = new QToolButton(groupScopeRow);
	exitGroupButton->setObjectName("studioPortraitExitGroup");
	exitGroupButton->setText(QTStr("Studio.Portrait.ExitGroup"));
	exitGroupButton->setToolTip(QTStr("Studio.Portrait.ExitGroupHint"));
	groupScopeLabel = new QLabel(groupScopeRow);
	groupScopeLabel->setObjectName("studioPortraitGroupName");
	groupScopeLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	groupScopeLayout->addWidget(exitGroupButton);
	groupScopeLayout->addWidget(groupScopeLabel, 1);
	groupScopeRow->setVisible(false);
	layout->addWidget(groupScopeRow);
	connect(exitGroupButton, &QToolButton::clicked, this, [this]() {
		previewEditor->ExitGroup();
		RefreshGroupScope();
		SyncLayerSelection();
	});
	auto *gestures = new QLabel(QTStr("Studio.Portrait.ShortHint"), content);
	gestures->setObjectName("studioPortraitGestures");
	gestures->setWordWrap(true);
	gestures->setToolTip(QTStr("Studio.Portrait.Gestures"));
	auto *gestureRow = new QHBoxLayout();
	cropButton = new QToolButton(content);
	cropButton->setObjectName("studioPortraitCropMode");
	cropButton->setText(QTStr("Studio.Portrait.CropMode"));
	cropButton->setCheckable(true);
	cropButton->setToolTip(QTStr("Studio.Portrait.Gestures"));
	connect(cropButton, &QToolButton::toggled, this, [this](bool enabled) { previewEditor->SetCropMode(enabled); });
	gestureRow->addWidget(cropButton);
	gestureRow->addWidget(gestures, 1);
	layout->addLayout(gestureRow);

	// Keep native preview and gesture controls clear of each other on short/HiDPI screens.
	auto *optionsScroll = new QScrollArea(content);
	optionsScroll->setObjectName("verticalPreviewOptionsScroll");
	optionsScroll->setWidgetResizable(true);
	optionsScroll->setFrameShape(QFrame::NoFrame);
	optionsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	optionsScroll->setMinimumHeight(120);
	optionsScroll->setMaximumHeight(320);
	auto *optionsBody = new QWidget(optionsScroll);
	auto *optionsLayout = new QVBoxLayout(optionsBody);
	optionsLayout->setContentsMargins(0, 0, 0, 0);
	optionsLayout->setSpacing(8);

	/* Layers of the vertical scene linked to the current program scene. */
	layersPanel = new QFrame(content);
	layersPanel->setObjectName(QStringLiteral("verticalPreviewLayers"));
	auto *layersLayout = new QVBoxLayout(layersPanel);
	layersLayout->setContentsMargins(12, 10, 12, 10);
	layersLayout->setSpacing(6);
	auto *layersLabel = new QLabel(QTStr("DualCanvas.Layers"), layersPanel);
	layersLabel->setObjectName(QStringLiteral("verticalPreviewLabel"));
	layersLayout->addWidget(layersLabel);
	auto *editLayers = new QPushButton(QTStr("Studio.Canvas.EditPortrait"), layersPanel);
	editLayers->setObjectName("studioEditPortraitLayers");
	layersLayout->addWidget(editLayers);
	connect(editLayers, &QPushButton::clicked, this, &VerticalPreviewDock::editPortraitRequested);
	layersList = new QListWidget(layersPanel);
	layersList->setObjectName(QStringLiteral("verticalPreviewLayerList"));
	layersList->setMinimumHeight(72);
	layersList->setMaximumHeight(150);
	layersLayout->addWidget(layersList);
	auto *layerButtons = new QHBoxLayout();
	layerButtons->setSpacing(4);
	auto makeButton = [&](const QString &text, const char *tip) {
		auto *button = new QToolButton(layersPanel);
		button->setObjectName(QStringLiteral("verticalPreviewLayerButton"));
		button->setText(text);
		button->setToolTip(QTStr(tip));
		button->setAccessibleName(QTStr(tip));
		layerButtons->addWidget(button);
		return button;
	};
	addLayerButton = makeButton(QStringLiteral("+"), "DualCanvas.AddLayer");
	removeLayerButton = makeButton(QStringLiteral("−"), "DualCanvas.RemoveLayer");
	upLayerButton = makeButton(QStringLiteral("▲"), "DualCanvas.MoveUp");
	downLayerButton = makeButton(QStringLiteral("▼"), "DualCanvas.MoveDown");
	fitLayerButton = makeButton(QStringLiteral("⤢"), "DualCanvas.FitLayer");
	addLayerButton->setProperty("studioCommand", "add");
	removeLayerButton->setProperty("studioCommand", "remove");
	upLayerButton->setProperty("studioCommand", "up");
	downLayerButton->setProperty("studioCommand", "down");
	fitLayerButton->setProperty("studioCommand", "fit");
	layerButtons->addStretch();
	mirrorButton = new QToolButton(layersPanel);
	mirrorButton->setObjectName(QStringLiteral("verticalPreviewLayerButton"));
	mirrorButton->setText(QTStr("DualCanvas.ResetMirror"));
	mirrorButton->setProperty("studioCommand", "mirror");
	mirrorButton->setToolTip(QTStr("DualCanvas.ResetMirror.Tooltip"));
	layerButtons->addWidget(mirrorButton);
	layersLayout->addLayout(layerButtons);
	optionsLayout->addWidget(layersPanel);
	layersPanel->setVisible(false);

	connect(layersList, &QListWidget::itemChanged, this, &VerticalPreviewDock::OnLayerChanged);
	connect(layersList, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
		previewEditor->ExitGroup();
		emit portraitItemActivated(item->data(Qt::UserRole).toLongLong());
		RefreshGroupScope();
	});
	connect(layersList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *) {
		OBSSceneItem item = SelectedItem();
		if (item) {
			obs_frontend_open_source_properties(obs_sceneitem_get_source(item));
		}
	});
	connect(layersList, &QListWidget::currentRowChanged, this, [this](int) { UpdateControls(); });
	connect(addLayerButton, &QToolButton::clicked, this, &VerticalPreviewDock::ShowAddMenu);
	connect(removeLayerButton, &QToolButton::clicked, this, &VerticalPreviewDock::RemoveSelectedLayer);
	connect(upLayerButton, &QToolButton::clicked, this, [this]() { MoveSelectedLayer(true); });
	connect(downLayerButton, &QToolButton::clicked, this, [this]() { MoveSelectedLayer(false); });
	connect(fitLayerButton, &QToolButton::clicked, this, &VerticalPreviewDock::FitSelectedLayer);
	connect(mirrorButton, &QToolButton::clicked, this, [this]() {
		RunSceneCommand("Studio.Portrait.UndoMirror", [this](obs_scene_t *scene) {
			dualCanvas->ResetToMirror();
			obs_scene_enum_items(
				scene,
				[](obs_scene_t *, obs_sceneitem_t *item, void *) {
					obs_sceneitem_select(item, true);
					return true;
				},
				nullptr);
		});
	});
	transitionPanel = new QFrame(optionsBody);
	transitionPanel->setObjectName("studioPortraitTransitionPanel");
	auto *transitionLayout = new QVBoxLayout(transitionPanel);
	transitionLayout->setContentsMargins(12, 10, 12, 10);
	transitionLayout->setSpacing(6);
	transitionLayout->addWidget(new QLabel(QTStr("Studio.Portrait.Transition"), transitionPanel));
	transitionMode = new QComboBox(transitionPanel);
	transitionMode->setObjectName("studioPortraitTransitionMode");
	transitionMode->addItem(QTStr("Studio.Portrait.Cut"),
				static_cast<int>(DualCanvasController::PortraitTransitionMode::Cut));
	transitionMode->addItem(QStringLiteral("Fade"),
				static_cast<int>(DualCanvasController::PortraitTransitionMode::Fade));
	transitionMode->addItem(QTStr("Studio.Portrait.FollowMain"),
				static_cast<int>(DualCanvasController::PortraitTransitionMode::FollowMain));
	transitionLayout->addWidget(transitionMode);
	auto *durationRow = new QHBoxLayout();
	durationRow->addWidget(new QLabel(QTStr("Studio.Portrait.Duration"), transitionPanel));
	transitionDuration = new QSpinBox(transitionPanel);
	transitionDuration->setObjectName("studioPortraitTransitionDuration");
	transitionDuration->setRange(50, 20000);
	transitionDuration->setSingleStep(50);
	transitionDuration->setSuffix(QStringLiteral(" ms"));
	transitionDuration->setValue(300);
	transitionDuration->setKeyboardTracking(false);
	durationRow->addWidget(transitionDuration, 1);
	transitionLayout->addLayout(durationRow);
	transitionHint = new QLabel(transitionPanel);
	transitionHint->setObjectName("studioPortraitTransitionHint");
	transitionHint->setWordWrap(true);
	transitionLayout->addWidget(transitionHint);
	optionsLayout->addWidget(transitionPanel);

	auto *controls = new QFrame(content);
	controls->setObjectName(QStringLiteral("verticalPreviewControls"));
	auto *controlsLayout = new QVBoxLayout(controls);
	controlsLayout->setContentsMargins(12, 10, 12, 10);
	controlsLayout->setSpacing(6);

	auto *channelLabel = new QLabel(QTStr("Multistream.Vertical.Channel"), controls);
	channelLabel->setObjectName(QStringLiteral("verticalPreviewLabel"));
	channelCombo = new QComboBox(controls);
	channelCombo->setObjectName(QStringLiteral("verticalPreviewChannel"));
	controlsLayout->addWidget(channelLabel);
	controlsLayout->addWidget(channelCombo);

	auto *framingLabel = new QLabel(QTStr("Multistream.Vertical.Framing"), controls);
	framingLabel->setObjectName(QStringLiteral("verticalPreviewLabel"));
	framingCombo = new QComboBox(controls);
	framingCombo->setObjectName(QStringLiteral("verticalPreviewFraming"));
	framingCombo->addItem(QTStr("Multistream.ManualChannel.PortraitFill"),
			      static_cast<int>(MultiStreamPortraitFit::Fill));
	framingCombo->addItem(QTStr("Multistream.ManualChannel.PortraitFit"),
			      static_cast<int>(MultiStreamPortraitFit::Fit));
	controlsLayout->addWidget(framingLabel);
	controlsLayout->addWidget(framingCombo);

	auto *footer = new QHBoxLayout();
	footer->setContentsMargins(0, 2, 0, 0);
	statusLabel = new QLabel(QTStr("Multistream.Vertical.Ready"), controls);
	statusLabel->setObjectName(QStringLiteral("verticalPreviewStatus"));
	statusLabel->setWordWrap(true);
	editButton = new QPushButton(QTStr("Multistream.Vertical.Configure"), controls);
	editButton->setObjectName(QStringLiteral("verticalPreviewEditButton"));
	footer->addWidget(statusLabel, 1);
	footer->addWidget(editButton);
	controlsLayout->addLayout(footer);
	optionsLayout->addWidget(controls);
	optionsScroll->setWidget(optionsBody);
	layout->addWidget(optionsScroll);

	setWidget(content);
	auto *selectionTimer = new QTimer(this);
	connect(selectionTimer, &QTimer::timeout, this, [this]() {
		SyncLayerSelection();
		RefreshGroupScope();
	});
	selectionTimer->start(100);
	connect(preview, &OBSQTDisplay::DisplayCreated, this, [this]() { OnDisplayCreated(); });
	connect(channelCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
		&VerticalPreviewDock::OnChannelChanged);
	connect(framingCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
		&VerticalPreviewDock::OnFramingChanged);
	connect(transitionMode, qOverload<int>(&QComboBox::currentIndexChanged), this,
		[this](int) { OnTransitionChanged(); });
	connect(transitionDuration, qOverload<int>(&QSpinBox::valueChanged), this,
		[this](int) { OnTransitionChanged(); });
	connect(editButton, &QPushButton::clicked, this, [this]() {
		const QString channelId = channelCombo->currentData().toString();
		if (!channelId.isEmpty()) {
			emit editChannelRequested(channelId);
		}
	});
	connect(addButton, &QPushButton::clicked, this, &VerticalPreviewDock::addChannelRequested);
	UpdateControls();
}

VerticalPreviewDock::~VerticalPreviewDock()
{
	Shutdown();
}

void VerticalPreviewDock::SetChannels(const std::vector<MultiStreamChannel> &channels)
{
	if (shuttingDown.load()) {
		return;
	}
	const QString selectedId = channelCombo->currentData().toString();
	portraitChannels.clear();
	std::copy_if(channels.begin(), channels.end(), std::back_inserter(portraitChannels),
		     [](const MultiStreamChannel &channel) {
			     return channel.videoLayout == MultiStreamVideoLayout::Portrait;
		     });

	refreshing = true;
	channelCombo->clear();
	for (const auto &channel : portraitChannels) {
		channelCombo->addItem(ChannelLabel(channel), QString::fromStdString(channel.id));
	}
	const int restore = channelCombo->findData(selectedId);
	channelCombo->setCurrentIndex(restore >= 0 ? restore : 0);
	refreshing = false;
	UpdateControls();
}

void VerticalPreviewDock::Shutdown()
{
	if (shuttingDown.exchange(true)) {
		return;
	}
	if (previewEditor) {
		previewEditor->Cancel();
	}
	if (dualCanvas) {
		disconnect(dualCanvas, nullptr, this, nullptr);
		dualCanvas = nullptr;
	}
	{
		std::lock_guard lock(canvasMutex);
		renderCanvas = nullptr;
	}
	if (preview && drawCallbackRegistered && preview->GetDisplay()) {
		obs_display_remove_draw_callback(preview->GetDisplay(), RenderPreview, this);
		drawCallbackRegistered = false;
	}
	if (preview) {
		preview->DestroyDisplay();
	}
}

void VerticalPreviewDock::OnDisplayCreated()
{
	if (shuttingDown.load() || !preview->GetDisplay()) {
		return;
	}
	obs_display_add_draw_callback(preview->GetDisplay(), RenderPreview, this);
	drawCallbackRegistered = true;
}

void VerticalPreviewDock::OnChannelChanged(int)
{
	if (!refreshing) {
		UpdateControls();
	}
}

void VerticalPreviewDock::OnFramingChanged(int)
{
	if (refreshing) {
		return;
	}
	const auto fit = static_cast<MultiStreamPortraitFit>(framingCombo->currentData().toInt());
	activeFit.store(static_cast<int>(fit));
	if (DualCanvasActive()) {
		/* Framing is a property of the mirrored layer of this scene. */
		RunSceneCommand("Studio.Portrait.UndoFraming",
				[this, fit](obs_scene_t *) { dualCanvas->SetMirrorFit(fit); });
		return;
	}
	if (channelCombo->currentIndex() < 0) {
		return;
	}
	emit portraitFitChanged(channelCombo->currentData().toString(), fit);
}

void VerticalPreviewDock::RefreshTransitionControls()
{
	const bool enabled = DualCanvasActive();
	transitionPanel->setVisible(enabled);
	transitionMode->setEnabled(enabled);
	const QSignalBlocker modeSignals(transitionMode);
	const QSignalBlocker durationSignals(transitionDuration);
	const int mode = dualCanvas ? dualCanvas->PortraitTransitionModeValue() : 0;
	transitionMode->setCurrentIndex(std::max(transitionMode->findData(mode), 0));
	transitionDuration->setValue(dualCanvas ? dualCanvas->PortraitTransitionDuration() : 300);
	transitionDuration->setEnabled(enabled &&
				       mode == static_cast<int>(DualCanvasController::PortraitTransitionMode::Fade));
	if (mode == static_cast<int>(DualCanvasController::PortraitTransitionMode::FollowMain)) {
		transitionHint->setText(QTStr("Studio.Portrait.FollowMainHint"));
	} else {
		transitionHint->setText(QTStr("Studio.Portrait.TransitionHint"));
	}
}

void VerticalPreviewDock::OnTransitionChanged()
{
	if (!dualCanvas || !DualCanvasActive() || shuttingDown.load()) {
		return;
	}
	const bool saved =
		dualCanvas->SetPortraitTransition(transitionMode->currentData().toInt(), transitionDuration->value());
	RefreshTransitionControls();
	statusLabel->setText(saved ? QTStr("Studio.Portrait.TransitionSaved")
				   : QTStr("Studio.Portrait.TransitionError"));
}

void VerticalPreviewDock::CancelPreviewGesture()
{
	if (previewEditor) {
		previewEditor->Cancel();
	}
}

void VerticalPreviewDock::RefreshGroupScope()
{
	if (shuttingDown.load()) {
		return;
	}
	const QString uuid = previewEditor ? previewEditor->GroupScopeUuid() : QString{};
	preview->setProperty("studioPortraitGroupScope", uuid);
	const bool inGroup = DualCanvasActive() && !uuid.isEmpty();
	groupScopeRow->setVisible(inGroup);
	exitGroupButton->setEnabled(inGroup);
	if (inGroup) {
		OBSSourceAutoRelease source = obs_get_source_by_uuid(uuid.toUtf8().constData());
		const QString name = source ? QString::fromUtf8(obs_source_get_name(source)) : uuid;
		groupScopeLabel->setText(QTStr("Studio.Portrait.GroupName").arg(name));
		groupScopeLabel->setToolTip(name);
	} else {
		groupScopeLabel->clear();
	}
}

void VerticalPreviewDock::UpdateControls()
{
	const int selected = channelCombo->currentIndex();
	const bool hasChannel = selected >= 0 && selected < static_cast<int>(portraitChannels.size());
	const bool dual = DualCanvasActive();
	const bool disabled = dualCanvas && !dual;
	RefreshTransitionControls();
	RefreshGroupScope();
	previewStack->setCurrentIndex(!disabled && (hasChannel || dual) ? 0 : 1);
	channelCombo->setEnabled(hasChannel && !disabled);
	editButton->setEnabled(hasChannel && !disabled);
	addLayerButton->setEnabled(dual);
	cropButton->setEnabled(dual);
	mirrorButton->setEnabled(dual);
	findChild<QLabel *>("verticalPreviewEmptyTitle")
		->setText(disabled ? QTStr("Studio.Canvas.DisabledTitle") : QTStr("Multistream.Vertical.EmptyTitle"));
	findChild<QLabel *>("verticalPreviewHint")
		->setText(disabled ? QTStr("Studio.Canvas.DisabledHint")
			  : dual   ? QTStr("DualCanvas.PreviewHint")
				   : QTStr("Multistream.Vertical.EmptyBody"));
	findChild<QPushButton *>("verticalPreviewPrimaryButton")->setVisible(!disabled);
	layersPanel->setVisible(dual);
	sceneBadge->setVisible(dual);
	sceneName->setVisible(dual);

	const bool hasLayer = dual && layersList->currentRow() >= 0;
	removeLayerButton->setEnabled(hasLayer);
	upLayerButton->setEnabled(hasLayer && layersList->currentRow() > 0);
	downLayerButton->setEnabled(hasLayer && layersList->currentRow() < layersList->count() - 1);
	fitLayerButton->setEnabled(hasLayer && !obs_sceneitem_locked(SelectedItem()));
	if (disabled) {
		framingCombo->setEnabled(false);
		statusLabel->setText(QTStr("Studio.Canvas.DisabledTitle"));
		return;
	}

	if (dual) {
		sceneName->setText(QTStr("DualCanvas.LinkedTo").arg(dualCanvas->CurrentMainSceneName()));
		MultiStreamPortraitFit fit = MultiStreamPortraitFit::Fill;
		const bool hasMirror = dualCanvas->MirrorFit(fit);
		bool editableMirror = false;
		obs_scene_enum_items(
			dualCanvas->CurrentPortraitScene(),
			[](obs_scene_t *, obs_sceneitem_t *item, void *data) {
				if (DualCanvasController::IsMirrorItem(item) && !obs_sceneitem_locked(item)) {
					*static_cast<bool *>(data) = true;
				}
				return true;
			},
			&editableMirror);
		framingCombo->setEnabled(hasMirror && editableMirror);
		refreshing = true;
		framingCombo->setCurrentIndex(std::max(framingCombo->findData(static_cast<int>(fit)), 0));
		refreshing = false;
		activeFit.store(static_cast<int>(fit));
		statusLabel->setText(
			hasChannel ? QTStr("Multistream.Vertical.Previewing")
					     .arg(ChannelLabel(portraitChannels[static_cast<size_t>(selected)]))
				   : QTStr("DualCanvas.NoChannelYet"));
		return;
	}

	framingCombo->setEnabled(hasChannel);
	if (!hasChannel) {
		statusLabel->setText(QTStr("Multistream.Vertical.NoChannel"));
		return;
	}

	const auto &channel = portraitChannels[static_cast<size_t>(selected)];
	refreshing = true;
	const int fitIndex = framingCombo->findData(static_cast<int>(channel.portraitFit));
	framingCombo->setCurrentIndex(std::max(fitIndex, 0));
	refreshing = false;
	activeFit.store(static_cast<int>(channel.portraitFit));
	statusLabel->setText(QTStr("Multistream.Vertical.Previewing").arg(ChannelLabel(channel)));
}

void VerticalPreviewDock::RenderPreview(void *data, uint32_t width, uint32_t height)
{
	auto *dock = static_cast<VerticalPreviewDock *>(data);
	if (!dock || dock->shuttingDown.load() || width == 0 || height == 0) {
		return;
	}

	OBSCanvas canvas;
	{
		std::lock_guard lock(dock->canvasMutex);
		if (dock->renderCanvas) {
			canvas = OBSGetStrongRef(dock->renderCanvas);
		}
	}
	if (canvas && !obs_canvas_removed(canvas)) {
		const float scale = std::min(static_cast<float>(width) / PORTRAIT_WIDTH,
					     static_cast<float>(height) / PORTRAIT_HEIGHT);
		const int frameWidth = std::max(1, static_cast<int>(PORTRAIT_WIDTH * scale));
		const int frameHeight = std::max(1, static_cast<int>(PORTRAIT_HEIGHT * scale));
		gs_viewport_push();
		gs_projection_push();
		gs_set_viewport((static_cast<int>(width) - frameWidth) / 2,
				(static_cast<int>(height) - frameHeight) / 2, frameWidth, frameHeight);
		gs_ortho(0.0f, PORTRAIT_WIDTH, 0.0f, PORTRAIT_HEIGHT, -100.0f, 100.0f);
		obs_render_canvas_texture(canvas);
		if (dock->previewEditor) {
			dock->previewEditor->Draw(scale);
		}
		gs_load_vertexbuffer(nullptr);
		gs_projection_pop();
		gs_viewport_pop();
		return;
	}

	OBSSourceAutoRelease source = obs_get_output_source(0);
	obs_video_info info{};
	if (!source && !obs_get_video_info(&info)) {
		return;
	}
	const float sourceWidth =
		static_cast<float>(source ? std::max(obs_source_get_width(source), 1u) : std::max(info.base_width, 1u));
	const float sourceHeight = static_cast<float>(source ? std::max(obs_source_get_height(source), 1u)
							     : std::max(info.base_height, 1u));

	const float frameScale =
		std::min(static_cast<float>(width) / PORTRAIT_WIDTH, static_cast<float>(height) / PORTRAIT_HEIGHT);
	const int frameWidth = std::max(1, static_cast<int>(PORTRAIT_WIDTH * frameScale));
	const int frameHeight = std::max(1, static_cast<int>(PORTRAIT_HEIGHT * frameScale));
	const int frameX = (static_cast<int>(width) - frameWidth) / 2;
	const int frameY = (static_cast<int>(height) - frameHeight) / 2;

	gs_viewport_push();
	gs_projection_push();
	const auto fit = static_cast<MultiStreamPortraitFit>(dock->activeFit.load());
	if (fit == MultiStreamPortraitFit::Fill) {
		const float targetAspect = PORTRAIT_WIDTH / PORTRAIT_HEIGHT;
		const float sourceAspect = sourceWidth / sourceHeight;
		float left = 0.0f;
		float top = 0.0f;
		float visibleWidth = sourceWidth;
		float visibleHeight = sourceHeight;
		if (sourceAspect > targetAspect) {
			visibleWidth = sourceHeight * targetAspect;
			left = (sourceWidth - visibleWidth) * 0.5f;
		} else {
			visibleHeight = sourceWidth / targetAspect;
			top = (sourceHeight - visibleHeight) * 0.5f;
		}
		gs_set_viewport(frameX, frameY, frameWidth, frameHeight);
		gs_ortho(left, left + visibleWidth, top, top + visibleHeight, -100.0f, 100.0f);
	} else {
		const float contentScale = std::min(static_cast<float>(frameWidth) / sourceWidth,
						    static_cast<float>(frameHeight) / sourceHeight);
		const int contentWidth = std::max(1, static_cast<int>(sourceWidth * contentScale));
		const int contentHeight = std::max(1, static_cast<int>(sourceHeight * contentScale));
		gs_set_viewport(frameX + (frameWidth - contentWidth) / 2, frameY + (frameHeight - contentHeight) / 2,
				contentWidth, contentHeight);
		gs_ortho(0.0f, sourceWidth, 0.0f, sourceHeight, -100.0f, 100.0f);
	}

	if (source) {
		obs_source_video_render(source);
	} else {
		obs_render_main_texture();
	}
	gs_load_vertexbuffer(nullptr);
	gs_projection_pop();
	gs_viewport_pop();
}

bool VerticalPreviewDock::DualCanvasActive() const
{
	return dualCanvas && dualCanvas->CurrentPortraitScene();
}

bool VerticalPreviewDock::eventFilter(QObject *object, QEvent *event)
{
	if (object == preview && event->type() == QEvent::MouseButtonPress && DualCanvasActive()) {
		emit editPortraitRequested();
	}
	return OBSDock::eventFilter(object, event);
}

void VerticalPreviewDock::SetDualCanvas(DualCanvasController *controller)
{
	if (dualCanvas) {
		disconnect(dualCanvas, nullptr, this, nullptr);
	}
	dualCanvas = controller;
	if (dualCanvas) {
		connect(dualCanvas, &DualCanvasController::portraitSceneChanged, this, [this]() {
			previewEditor->Cancel();
			RefreshCanvas();
			RefreshLayers();
		});
		connect(dualCanvas, &DualCanvasController::layersChanged, this, &VerticalPreviewDock::RefreshLayers);
		connect(dualCanvas, &DualCanvasController::portraitTransitionSettingsChanged, this,
			&VerticalPreviewDock::RefreshTransitionControls);
		connect(dualCanvas, &QObject::destroyed, this, [this]() {
			dualCanvas = nullptr;
			RefreshCanvas();
			UpdateControls();
		});
	}
	RefreshCanvas();
	RefreshLayers();
}

void VerticalPreviewDock::RefreshCanvas()
{
	OBSCanvas canvas = dualCanvas ? dualCanvas->PortraitCanvas() : OBSCanvas();
	std::lock_guard lock(canvasMutex);
	renderCanvas = canvas ? OBSGetWeakRef(canvas) : OBSWeakCanvas();
}

void VerticalPreviewDock::RefreshLayers()
{
	if (shuttingDown.load()) {
		return;
	}
	const QSignalBlocker listSignals(layersList);
	refreshing = true;
	layersList->clear();
	OBSScene scene = dualCanvas ? dualCanvas->CurrentPortraitScene() : OBSScene();
	if (scene) {
		std::vector<OBSSceneItem> items;
		obs_scene_enum_items(
			scene,
			[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
				static_cast<std::vector<OBSSceneItem> *>(param)->emplace_back(item);
				return true;
			},
			&items);
		/* libobs lists bottom-to-top; show the top layer first like OBS. */
		std::reverse(items.begin(), items.end());
		for (auto &item : items) {
			obs_source_t *source = obs_sceneitem_get_source(item);
			QString name = QString::fromUtf8(obs_source_get_name(source));
			if (DualCanvasController::IsMirrorItem(item)) {
				name = QTStr("DualCanvas.MirrorLayer").arg(name);
			}
			auto *row = new QListWidgetItem(name, layersList);
			row->setFlags(row->flags() | Qt::ItemIsUserCheckable);
			row->setCheckState(obs_sceneitem_visible(item) ? Qt::Checked : Qt::Unchecked);
			row->setData(Qt::UserRole, QVariant::fromValue<qlonglong>(obs_sceneitem_get_id(item)));
		}
	}
	refreshing = false;
	SyncLayerSelection();
	UpdateControls();
}

void VerticalPreviewDock::SyncLayerSelection()
{
	if (shuttingDown.load() || refreshing) {
		return;
	}
	OBSScene scene = dualCanvas ? dualCanvas->CurrentPortraitScene() : OBSScene{};
	int selected = -1;
	for (int row = 0; scene && row < layersList->count(); ++row) {
		auto *item =
			obs_scene_find_sceneitem_by_id(scene, layersList->item(row)->data(Qt::UserRole).toLongLong());
		if (item && obs_sceneitem_selected(item)) {
			selected = row;
			break;
		}
	}
	if (layersList->currentRow() != selected) {
		const QSignalBlocker blocker(layersList);
		layersList->setCurrentRow(selected);
		UpdateControls();
	}
}

OBSSceneItem VerticalPreviewDock::SelectedItem() const
{
	OBSScene scene = dualCanvas ? dualCanvas->CurrentPortraitScene() : OBSScene();
	QListWidgetItem *row = layersList->currentItem();
	if (!scene || !row) {
		return nullptr;
	}
	return obs_scene_find_sceneitem_by_id(scene, row->data(Qt::UserRole).toLongLong());
}

void VerticalPreviewDock::OnLayerChanged(QListWidgetItem *row)
{
	if (refreshing || !row || !dualCanvas) {
		return;
	}
	OBSScene scene = dualCanvas->CurrentPortraitScene();
	if (!scene) {
		return;
	}
	obs_sceneitem_t *item = obs_scene_find_sceneitem_by_id(scene, row->data(Qt::UserRole).toLongLong());
	if (item) {
		OBSSceneItem held = item;
		const bool visible = row->checkState() == Qt::Checked;
		if (obs_sceneitem_visible(item) == visible) {
			return;
		}
		RunSceneCommand("Studio.Portrait.UndoVisibility",
				[held, visible](obs_scene_t *) { obs_sceneitem_set_visible(held, visible); });
	}
}

void VerticalPreviewDock::ShowAddMenu()
{
	if (!DualCanvasActive()) {
		return;
	}
	QMenu menu(this);
	menu.setObjectName("studioPortraitAddMenu");
	const std::string expectedScene = obs_source_get_uuid(obs_scene_get_source(dualCanvas->CurrentPortraitScene()));
	/* Offer the sources of the horizontal program scene first: those are the
	 * ones the user wants to rearrange for vertical. */
	OBSSourceAutoRelease mainScene = obs_frontend_get_current_scene();
	struct Collect {
		std::vector<OBSSource> sources;
	} collect;
	if (obs_scene_t *scene = obs_scene_from_source(mainScene)) {
		obs_scene_enum_items(
			scene,
			[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
				auto *out = static_cast<Collect *>(param);
				obs_source_t *source = obs_sceneitem_get_source(item);
				if (std::find(out->sources.begin(), out->sources.end(), source) == out->sources.end()) {
					out->sources.emplace_back(source);
				}
				return true;
			},
			&collect);
	}
	std::reverse(collect.sources.begin(), collect.sources.end());
	if (mainScene) {
		QAction *whole = menu.addAction(
			QTStr("DualCanvas.AddWholeScene").arg(QString::fromUtf8(obs_source_get_name(mainScene))));
		whole->setObjectName("studioPortraitAddWholeScene");
		OBSSource sceneSource = mainScene.Get();
		connect(whole, &QAction::triggered, this, [this, sceneSource, expectedScene]() {
			RunSceneCommand(
				"Studio.Portrait.UndoAdd",
				[this, sceneSource](obs_scene_t *scene) {
					if (!obs_source_removed(sceneSource) && dualCanvas->AddSource(sceneSource)) {
						OBSSceneItem last;
						obs_scene_enum_items(
							scene,
							[](obs_scene_t *, obs_sceneitem_t *item, void *data) {
								obs_sceneitem_select(item, false);
								*static_cast<OBSSceneItem *>(data) = item;
								return true;
							},
							&last);
						if (last) {
							obs_sceneitem_select(last, true);
						}
					}
				},
				expectedScene);
		});
		menu.addSeparator();
	}
	for (auto &source : collect.sources) {
		QAction *action = menu.addAction(QString::fromUtf8(obs_source_get_name(source)));
		action->setData(QString::fromUtf8(obs_source_get_uuid(source)));
		connect(action, &QAction::triggered, this, [this, source, expectedScene]() {
			RunSceneCommand(
				"Studio.Portrait.UndoAdd",
				[this, source](obs_scene_t *scene) {
					if (!obs_source_removed(source) && dualCanvas->AddSource(source)) {
						OBSSceneItem last;
						obs_scene_enum_items(
							scene,
							[](obs_scene_t *, obs_sceneitem_t *item, void *data) {
								obs_sceneitem_select(item, false);
								*static_cast<OBSSceneItem *>(data) = item;
								return true;
							},
							&last);
						if (last) {
							obs_sceneitem_select(last, true);
						}
					}
				},
				expectedScene);
		});
	}
	if (collect.sources.empty() && !mainScene) {
		menu.addAction(QTStr("DualCanvas.NothingToAdd"))->setEnabled(false);
	}
	menu.exec(addLayerButton->mapToGlobal(QPoint(0, addLayerButton->height())));
}

void VerticalPreviewDock::RunSceneCommand(const char *label, const std::function<void(obs_scene_t *)> &change,
					  const std::string &expectedScene)
{
	auto *main = OBSBasic::Get();
	OBSScene scene = dualCanvas ? dualCanvas->CurrentPortraitScene() : OBSScene{};
	if (shuttingDown.load() || !main || main->isClosing() || !scene) {
		return;
	}
	if (!expectedScene.empty() && expectedScene != obs_source_get_uuid(obs_scene_get_source(scene))) {
		statusLabel->setText(QTStr("Studio.Portrait.SceneChanged"));
		return;
	}
	previewEditor->Cancel();
	emit editPortraitRequested();
	OBSData before = CaptureStudioSceneEdit(scene);
	ApplyStudioSceneEdit(scene, change);
	OBSData after = CaptureStudioSceneEdit(scene);
	const int64_t selected = obs_data_get_int(after, "selected_id");
	const std::string sceneUuid = obs_source_get_uuid(obs_scene_get_source(scene));
	QMetaObject::invokeMethod(
		this,
		[this, selected, sceneUuid]() {
			OBSScene current = dualCanvas ? dualCanvas->CurrentPortraitScene() : OBSScene{};
			if (!current || sceneUuid != obs_source_get_uuid(obs_scene_get_source(current))) {
				return;
			}
			RefreshLayers();
			emit portraitItemActivated(selected);
		},
		Qt::QueuedConnection);
	if (!StudioSceneEditChanged(before, after)) {
		return;
	}
	QPointer<VerticalPreviewDock> guard(this);
	auto apply = [guard](const std::string &json) {
		int64_t id = -1;
		if (RestoreStudioSceneEdit(json, id) && guard) {
			guard->RefreshLayers();
			emit guard->portraitItemActivated(id);
		}
	};
	main->undo_s.add_action(QTStr(label), apply, apply, obs_data_get_json(before), obs_data_get_json(after));
	main->SaveProject();
}

void VerticalPreviewDock::RemoveSelectedLayer()
{
	OBSSceneItem item = SelectedItem();
	if (!item) {
		return;
	}
	int row = layersList->currentRow();
	int next = row + 1 < layersList->count() ? row + 1 : row - 1;
	const int64_t nextId = next >= 0 ? layersList->item(next)->data(Qt::UserRole).toLongLong() : -1;
	RunSceneCommand("Studio.Portrait.UndoRemove", [item, nextId](obs_scene_t *scene) {
		obs_sceneitem_remove(item);
		obs_scene_enum_items(
			scene,
			[](obs_scene_t *, obs_sceneitem_t *remaining, void *data) {
				obs_sceneitem_select(remaining, obs_sceneitem_get_id(remaining) ==
									*static_cast<const int64_t *>(data));
				return true;
			},
			const_cast<int64_t *>(&nextId));
	});
}

void VerticalPreviewDock::MoveSelectedLayer(bool up)
{
	OBSSceneItem item = SelectedItem();
	if (!item) {
		return;
	}
	RunSceneCommand(up ? "Studio.Portrait.UndoUp" : "Studio.Portrait.UndoDown", [item, up](obs_scene_t *) {
		obs_sceneitem_set_order(item, up ? OBS_ORDER_MOVE_UP : OBS_ORDER_MOVE_DOWN);
	});
}

void VerticalPreviewDock::FitSelectedLayer()
{
	OBSSceneItem item = SelectedItem();
	if (!item || obs_sceneitem_locked(item)) {
		return;
	}
	RunSceneCommand("Studio.Portrait.UndoFit", [item](obs_scene_t *) {
		DualCanvasController::FitItemToCanvas(item, MultiStreamPortraitFit::Fit);
	});
}
