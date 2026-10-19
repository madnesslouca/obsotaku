/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "VerticalPreviewDock.hpp"

#include <utility/DualCanvasController.hpp>
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
#include <QPushButton>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

#include "moc_VerticalPreviewDock.cpp"

namespace {
constexpr float PORTRAIT_WIDTH = 1080.0f;
constexpr float PORTRAIT_HEIGHT = 1920.0f;

QString ChannelLabel(const MultiStreamChannel &channel)
{
	return QStringLiteral("%1 · %2")
		.arg(QString::fromStdString(channel.displayName), StreamPlatformDisplayName(channel.platform));
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

	/* Layers of the vertical scene linked to the current program scene. */
	layersPanel = new QFrame(content);
	layersPanel->setObjectName(QStringLiteral("verticalPreviewLayers"));
	auto *layersLayout = new QVBoxLayout(layersPanel);
	layersLayout->setContentsMargins(12, 10, 12, 10);
	layersLayout->setSpacing(6);
	auto *layersLabel = new QLabel(QTStr("DualCanvas.Layers"), layersPanel);
	layersLabel->setObjectName(QStringLiteral("verticalPreviewLabel"));
	layersLayout->addWidget(layersLabel);
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
	layerButtons->addStretch();
	mirrorButton = new QToolButton(layersPanel);
	mirrorButton->setObjectName(QStringLiteral("verticalPreviewLayerButton"));
	mirrorButton->setText(QTStr("DualCanvas.ResetMirror"));
	mirrorButton->setToolTip(QTStr("DualCanvas.ResetMirror.Tooltip"));
	layerButtons->addWidget(mirrorButton);
	layersLayout->addLayout(layerButtons);
	layout->addWidget(layersPanel);
	layersPanel->setVisible(false);

	connect(layersList, &QListWidget::itemChanged, this, &VerticalPreviewDock::OnLayerChanged);
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
		if (dualCanvas) {
			dualCanvas->ResetToMirror();
		}
	});

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
	layout->addWidget(controls);

	setWidget(content);
	connect(preview, &OBSQTDisplay::DisplayCreated, this, [this]() { OnDisplayCreated(); });
	connect(channelCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
		&VerticalPreviewDock::OnChannelChanged);
	connect(framingCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
		&VerticalPreviewDock::OnFramingChanged);
	connect(editButton, &QPushButton::clicked, this, [this]() {
		const QString channelId = channelCombo->currentData().toString();
		if (!channelId.isEmpty())
			emit editChannelRequested(channelId);
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
	if (shuttingDown.load())
		return;
	const QString selectedId = channelCombo->currentData().toString();
	portraitChannels.clear();
	std::copy_if(channels.begin(), channels.end(), std::back_inserter(portraitChannels),
		     [](const MultiStreamChannel &channel) {
			     return channel.videoLayout == MultiStreamVideoLayout::Portrait;
		     });

	refreshing = true;
	channelCombo->clear();
	for (const auto &channel : portraitChannels)
		channelCombo->addItem(ChannelLabel(channel), QString::fromStdString(channel.id));
	const int restore = channelCombo->findData(selectedId);
	channelCombo->setCurrentIndex(restore >= 0 ? restore : 0);
	refreshing = false;
	UpdateControls();
}

void VerticalPreviewDock::Shutdown()
{
	if (shuttingDown.exchange(true))
		return;
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
	if (preview)
		preview->DestroyDisplay();
}

void VerticalPreviewDock::OnDisplayCreated()
{
	if (shuttingDown.load() || !preview->GetDisplay() || drawCallbackRegistered)
		return;
	obs_display_add_draw_callback(preview->GetDisplay(), RenderPreview, this);
	drawCallbackRegistered = true;
}

void VerticalPreviewDock::OnChannelChanged(int)
{
	if (!refreshing)
		UpdateControls();
}

void VerticalPreviewDock::OnFramingChanged(int)
{
	if (refreshing)
		return;
	const auto fit = static_cast<MultiStreamPortraitFit>(framingCombo->currentData().toInt());
	activeFit.store(static_cast<int>(fit));
	if (DualCanvasActive()) {
		/* Framing is a property of the mirrored layer of this scene. */
		dualCanvas->SetMirrorFit(fit);
		return;
	}
	if (channelCombo->currentIndex() < 0) {
		return;
	}
	emit portraitFitChanged(channelCombo->currentData().toString(), fit);
}

void VerticalPreviewDock::UpdateControls()
{
	const int selected = channelCombo->currentIndex();
	const bool hasChannel = selected >= 0 && selected < static_cast<int>(portraitChannels.size());
	const bool dual = DualCanvasActive();
	previewStack->setCurrentIndex(hasChannel || dual ? 0 : 1);
	channelCombo->setEnabled(hasChannel);
	editButton->setEnabled(hasChannel);
	layersPanel->setVisible(dual);
	sceneBadge->setVisible(dual);
	sceneName->setVisible(dual);

	const bool hasLayer = dual && layersList->currentRow() >= 0;
	removeLayerButton->setEnabled(hasLayer);
	upLayerButton->setEnabled(hasLayer && layersList->currentRow() > 0);
	downLayerButton->setEnabled(hasLayer && layersList->currentRow() < layersList->count() - 1);
	fitLayerButton->setEnabled(hasLayer);

	if (dual) {
		sceneName->setText(QTStr("DualCanvas.LinkedTo").arg(dualCanvas->CurrentMainSceneName()));
		MultiStreamPortraitFit fit = MultiStreamPortraitFit::Fill;
		const bool hasMirror = dualCanvas->MirrorFit(fit);
		framingCombo->setEnabled(hasMirror);
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
	if (!dock || dock->shuttingDown.load() || width == 0 || height == 0)
		return;

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
		gs_load_vertexbuffer(nullptr);
		gs_projection_pop();
		gs_viewport_pop();
		return;
	}

	OBSSourceAutoRelease source = obs_get_output_source(0);
	obs_video_info info{};
	if (!source && !obs_get_video_info(&info))
		return;
	const float sourceWidth = static_cast<float>(source ? std::max(obs_source_get_width(source), 1u)
						       : std::max(info.base_width, 1u));
	const float sourceHeight = static_cast<float>(source ? std::max(obs_source_get_height(source), 1u)
							: std::max(info.base_height, 1u));

	const float frameScale = std::min(static_cast<float>(width) / PORTRAIT_WIDTH,
					  static_cast<float>(height) / PORTRAIT_HEIGHT);
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

	if (source)
		obs_source_video_render(source);
	else
		obs_render_main_texture();
	gs_load_vertexbuffer(nullptr);
	gs_projection_pop();
	gs_viewport_pop();
}

bool VerticalPreviewDock::DualCanvasActive() const
{
	return dualCanvas && dualCanvas->CurrentPortraitScene();
}

void VerticalPreviewDock::SetDualCanvas(DualCanvasController *controller)
{
	if (dualCanvas) {
		disconnect(dualCanvas, nullptr, this, nullptr);
	}
	dualCanvas = controller;
	if (dualCanvas) {
		connect(dualCanvas, &DualCanvasController::portraitSceneChanged, this, [this]() {
			RefreshCanvas();
			RefreshLayers();
		});
		connect(dualCanvas, &DualCanvasController::layersChanged, this, &VerticalPreviewDock::RefreshLayers);
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
	const int previousRow = layersList->currentRow();
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
	if (layersList->count() > 0) {
		layersList->setCurrentRow(std::clamp(previousRow, 0, layersList->count() - 1));
	}
	refreshing = false;
	UpdateControls();
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
		obs_sceneitem_set_visible(item, row->checkState() == Qt::Checked);
	}
}

void VerticalPreviewDock::ShowAddMenu()
{
	if (!DualCanvasActive()) {
		return;
	}
	QMenu menu(this);
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
		OBSSource sceneSource = mainScene.Get();
		connect(whole, &QAction::triggered, this, [this, sceneSource]() {
			if (dualCanvas) {
				dualCanvas->AddSource(sceneSource);
			}
		});
		menu.addSeparator();
	}
	for (auto &source : collect.sources) {
		QAction *action = menu.addAction(QString::fromUtf8(obs_source_get_name(source)));
		connect(action, &QAction::triggered, this, [this, source]() {
			if (dualCanvas) {
				dualCanvas->AddSource(source);
			}
		});
	}
	if (collect.sources.empty() && !mainScene) {
		menu.addAction(QTStr("DualCanvas.NothingToAdd"))->setEnabled(false);
	}
	menu.exec(addLayerButton->mapToGlobal(QPoint(0, addLayerButton->height())));
}

void VerticalPreviewDock::RemoveSelectedLayer()
{
	OBSSceneItem item = SelectedItem();
	if (item) {
		obs_sceneitem_remove(item);
	}
}

void VerticalPreviewDock::MoveSelectedLayer(bool up)
{
	OBSSceneItem item = SelectedItem();
	if (!item) {
		return;
	}
	/* The list shows the top layer first, so "up" raises the layer. */
	obs_sceneitem_set_order(item, up ? OBS_ORDER_MOVE_UP : OBS_ORDER_MOVE_DOWN);
	const int row = layersList->currentRow() + (up ? -1 : 1);
	RefreshLayers();
	if (row >= 0 && row < layersList->count()) {
		layersList->setCurrentRow(row);
	}
}

void VerticalPreviewDock::FitSelectedLayer()
{
	OBSSceneItem item = SelectedItem();
	if (item) {
		DualCanvasController::FitItemToCanvas(item, MultiStreamPortraitFit::Fit);
	}
}
