/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "VerticalPreviewDock.hpp"

#include <utility/StreamPlatformDisplay.hpp>
#include <widgets/OBSQTDisplay.hpp>

#include <qt-wrappers.hpp>

#include <graphics/graphics.h>

#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
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
	headerLayout->addWidget(title);
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
	if (refreshing || channelCombo->currentIndex() < 0)
		return;
	const auto fit = static_cast<MultiStreamPortraitFit>(framingCombo->currentData().toInt());
	activeFit.store(static_cast<int>(fit));
	emit portraitFitChanged(channelCombo->currentData().toString(), fit);
}

void VerticalPreviewDock::UpdateControls()
{
	const int selected = channelCombo->currentIndex();
	const bool hasChannel = selected >= 0 && selected < static_cast<int>(portraitChannels.size());
	previewStack->setCurrentIndex(hasChannel ? 0 : 1);
	channelCombo->setEnabled(hasChannel);
	framingCombo->setEnabled(hasChannel);
	editButton->setEnabled(hasChannel);
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
