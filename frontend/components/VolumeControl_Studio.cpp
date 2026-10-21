#include "VolumeControl.hpp"
#include "StudioAudioEffects.hpp"
#include <components/VolumeMeter.hpp>
#include <widgets/OBSBasic.hpp>
#include <utility/StudioVodControls.hpp>
#include <QBoxLayout>
#include <QApplication>
#include <QFontMetrics>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMutexLocker>
#include <QMouseEvent>
#include <QPainter>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <algorithm>
#include <cmath>
#include <util/platform.h>

namespace {
class EffectDelegate final : public QStyledItemDelegate {
    std::function<void(const QModelIndex &)> edit;
public:
    EffectDelegate(QObject *parent, std::function<void(const QModelIndex &)> edit_)
        : QStyledItemDelegate(parent), edit(std::move(edit_)) {}
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override
    {
        return QSize(0, 19);
    }
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt(option); initStyleOption(&opt, index);
        const QRect indicator = opt.widget->style()->subElementRect(QStyle::SE_ItemViewItemCheckIndicator, &opt, opt.widget);
        painter->save(); painter->setRenderHint(QPainter::Antialiasing);
        if (opt.state & QStyle::State_MouseOver) painter->fillRect(opt.rect, QColor("#303036"));
        painter->setPen(Qt::NoPen);
        painter->setBrush(index.data(Qt::CheckStateRole).toInt() == Qt::Checked ? QColor("#57dfb0") : QColor("#686870"));
        painter->drawEllipse(indicator.center(), 3, 3);
        painter->setPen(opt.palette.color(QPalette::Text));
        const QRect textRect = opt.rect.adjusted(indicator.right() - opt.rect.left() + 4, 0, -22, 0);
        painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, opt.fontMetrics.elidedText(opt.text, Qt::ElideRight, textRect.width()));
        painter->setPen(QPen(QColor("#a2a2ad"), 1));
        const int x = opt.rect.right() - 15, y = opt.rect.center().y();
        for (int offset : {-3, 3}) {
            painter->drawLine(x, y + offset, x + 11, y + offset);
            painter->setBrush(QColor("#252527"));
            painter->drawEllipse(QPointF(x + (offset < 0 ? 4 : 8), y + offset), 1.5, 1.5);
        }
        painter->restore();
    }
    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option, const QModelIndex &index) override
    {
        if (event->type() == QEvent::MouseButtonRelease) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            QStyleOptionViewItem opt(option); initStyleOption(&opt, index);
            const QRect indicator = opt.widget->style()->subElementRect(QStyle::SE_ItemViewItemCheckIndicator, &opt, opt.widget);
            if (mouse->button() == Qt::LeftButton && !indicator.contains(mouse->position().toPoint())) {
                edit(index); return true;
            }
        }
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }
};
OBSSource FindFilter(obs_source_t *source, const QString &uuid)
{
    struct Search { QString uuid; OBSSource filter; } search{uuid, {}};
    obs_source_enum_filters(source, [](obs_source_t *, obs_source_t *filter, void *opaque) {
        auto &search = *static_cast<Search *>(opaque);
        if (search.uuid == QString::fromUtf8(obs_source_get_uuid(filter))) search.filter = filter;
    }, &search);
    return search.filter;
}
QIcon LinkIcon(bool linked)
{
    QPixmap image(20, 20); image.fill(Qt::transparent);
    QPainter painter(&image); painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(linked ? QColor("#57dfb0") : QColor("#93939d"), 1.4));
    painter.translate(10, 10); painter.rotate(-38);
    painter.drawRoundedRect(QRectF(-8, -3, 9, 6), 3, 3);
    painter.drawRoundedRect(QRectF(-1, -3, 9, 6), 3, 3);
    if (linked) painter.drawLine(QPointF(-3, 0), QPointF(3, 0));
    else painter.drawLine(QPointF(0, -5), QPointF(0, 5));
    return QIcon(image);
}
void RestoreDevice(const std::string &uuid, const std::string &id)
{
    OBSSourceAutoRelease source = obs_get_source_by_uuid(uuid.c_str());
    if (!source || obs_source_removed(source)) return;
    OBSDataAutoRelease settings = obs_data_create();
    obs_data_set_string(settings, "device_id", id.c_str());
    obs_source_update(source, settings);
    OBSBasic::Get()->SaveProject();
}
}

void VolumeControl::CreateStudioWidgets()
{
    studioEffects = new QWidget(this);
    studioEffects->setObjectName("studioMixerEffects");
    auto *effectsLayout = new QVBoxLayout(studioEffects);
    effectsLayout->setContentsMargins(0, 0, 0, 0); effectsLayout->setSpacing(0);
    studioVodBadge = new QLabel(QStringLiteral("VOD-EXCL"), studioEffects);
    studioVodBadge->setObjectName("studioMixerVodBadge");
    studioVodBadge->setAlignment(Qt::AlignCenter);
    studioVodBadge->setFixedHeight(14);
    studioVodBadge->hide();
    effectsLayout->addWidget(studioVodBadge);
    studioEffectList = new QListWidget(studioEffects);
    studioEffectList->setObjectName("studioMixerEffectList");
    studioEffectList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    studioEffectList->setSelectionMode(QAbstractItemView::SingleSelection);
    studioEffectList->setDragDropMode(QAbstractItemView::InternalMove);
    studioEffectList->setDropIndicatorShown(true);
    studioEffectList->setDragDropOverwriteMode(false);
    studioEffectList->setDefaultDropAction(Qt::MoveAction);
    studioEffectList->setContextMenuPolicy(Qt::CustomContextMenu);
    studioEffectList->setToolTip(QTStr("Studio.MixerEffectHint"));
    effectsLayout->addWidget(studioEffectList);
    studioAddEffect = new QPushButton(QStringLiteral("+"), studioEffects);
    studioAddEffect->setObjectName("studioMixerAddEffect");
    studioAddEffect->setToolTip(QTStr("Studio.MixerAddEffect"));
    studioAddEffect->setAccessibleName(QTStr("Studio.MixerAddEffect"));
    effectsLayout->addWidget(studioAddEffect);
    auto addEffect = [this]() {
        OBSSource source = OBSGetStrongRef(weakSource());
        if (source) ShowStudioAudioEffectPicker(studioEffects, source);
    };
    auto editEffect = [this](const QModelIndex &index) {
        OBSSource source = OBSGetStrongRef(weakSource());
        if (source) ShowStudioAudioEffectEditor(studioEffects, source, index.data(Qt::UserRole).toString());
    };
    studioEffectList->setMouseTracking(true);
    studioEffectList->setItemDelegate(new EffectDelegate(studioEffectList, editEffect));
    connect(studioEffectList->model(), &QAbstractItemModel::rowsMoved, this, [this]() {
        OBSSource source = OBSGetStrongRef(weakSource());
        QStringList order;
        for (int row = 0; row < studioEffectList->count(); ++row) order.append(studioEffectList->item(row)->data(Qt::UserRole).toString());
        if (source) ReorderStudioAudioEffects(source, order);
    });
    connect(studioEffectList, &QWidget::customContextMenuRequested, this, [this](const QPoint &point) {
        auto *item = studioEffectList->itemAt(point);
        if (!item) return;
        const QString uuid = item->data(Qt::UserRole).toString();
        QMenu menu(this);
        auto *edit = menu.addAction(QTStr("Studio.EffectEdit"));
        auto *up = menu.addAction(QTStr("Studio.EffectUp"));
        menu.addAction(QTStr("Studio.EffectDown"));
        menu.addSeparator(); auto *remove = menu.addAction(QTStr("Remove"));
        auto *selected = menu.exec(studioEffectList->viewport()->mapToGlobal(point));
        OBSSource source = OBSGetStrongRef(weakSource());
        if (!source || !selected) return;
        if (selected == edit) ShowStudioAudioEffectEditor(studioEffects, source, uuid);
        else if (selected == remove) RemoveStudioAudioEffect(source, uuid);
        else MoveStudioAudioEffect(source, uuid, selected == up ? -1 : 1);
    });
    connect(studioAddEffect, &QPushButton::clicked, this, addEffect);
    connect(studioEffectList, &QListWidget::itemActivated, this, [this, editEffect](QListWidgetItem *item) {
        editEffect(studioEffectList->indexFromItem(item));
    });
    connect(studioEffectList, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
        OBSSource source = OBSGetStrongRef(weakSource());
        if (!source) return;
        const QString filterUUID = item->data(Qt::UserRole).toString();
        OBSSource filter = FindFilter(source, filterUUID);
        if (!filter) return;
        const bool before = obs_source_enabled(filter), after = item->checkState() == Qt::Checked;
        if (before == after) return;
        obs_source_set_enabled(filter, after);
        const std::string parentUUID = obs_source_get_uuid(source);
        auto restore = [parentUUID, filterUUID](const std::string &value) {
            OBSSourceAutoRelease parent = obs_get_source_by_uuid(parentUUID.c_str());
            if (!parent) return;
            OBSSource filter = FindFilter(parent, filterUUID);
            if (filter) obs_source_set_enabled(filter, value == "on");
            OBSBasic::Get()->SaveProject();
        };
        OBSBasic::Get()->undo_s.add_action(QTStr("Studio.MixerEffectToggle"), restore, restore,
                                         before ? "on" : "off", after ? "on" : "off");
        OBSBasic::Get()->SaveProject();
    });
    studioDevice = new QToolButton(this);
    studioDevice->setObjectName("studioMixerDevice");
    studioDevice->setPopupMode(QToolButton::InstantPopup);
    auto *deviceMenu = new QMenu(studioDevice);
    studioDevice->setMenu(deviceMenu);
    connect(deviceMenu, &QMenu::aboutToShow, this, &VolumeControl::ShowStudioDevices);
    studioPeak = new QLabel(QStringLiteral("−∞"), this);
    studioPeak->setObjectName("studioMixerPeak");
    studioPeak->setAlignment(Qt::AlignCenter);
    studioPeak->setToolTip(QTStr("Studio.MixerPeakHint"));
    studioRefresh = new QTimer(this);
    studioRefresh->setInterval(200);
    connect(studioRefresh, &QTimer::timeout, this, &VolumeControl::RefreshStudioStrip);
    studioRefresh->start();
}

void VolumeControl::BuildStudioStrip(QBoxLayout *layout)
{
    setFixedWidth(154); setMinimumHeight(310);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    layout->setContentsMargins(7, 0, 7, 7); layout->setSpacing(4);
    layout->addWidget(nameButton);
    nameButton->setAlignment(Qt::AlignCenter);
    nameButton->setFixedHeight(24);
    studioEffects->setFixedHeight(78);
    layout->addWidget(studioEffects);
    auto *middle = new QHBoxLayout;
    middle->setContentsMargins(0, 0, 0, 0); middle->setSpacing(3);
    auto *meter = new QVBoxLayout;
    meter->setSpacing(0); meter->addWidget(studioPeak);
    volumeMeter->setVertical(true);
    volumeMeter->setFixedWidth(44);
    volumeMeter->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    meter->addWidget(volumeMeter, 1);
    middle->addLayout(meter, 1);
    auto *fader = new QVBoxLayout;
    fader->setSpacing(0);
    volumeLabel->setAlignment(Qt::AlignCenter);
    fader->addWidget(volumeLabel);
    slider->setOrientation(Qt::Vertical);
    slider->setDisplayTicks(false);
    slider->setLayoutDirection(Qt::LeftToRight);
    slider->setFixedWidth(22);
    fader->addWidget(slider, 1, Qt::AlignHCenter);
    middle->addLayout(fader);
    auto *buttons = new QVBoxLayout;
    buttons->setContentsMargins(3, 0, 0, 0); buttons->setSpacing(4);
    for (auto *button : {studioEditButton, studioLinkButton, monitorButton, muteButton})
        button->setFixedWidth(46);
    studioDevice->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    buttons->addWidget(studioEditButton);
    buttons->addStretch(1);
    buttons->addWidget(studioLinkButton);
    buttons->addWidget(monitorButton);
    buttons->addWidget(muteButton);
    middle->addLayout(buttons);
    layout->addLayout(middle, 1);
    layout->addWidget(studioDevice);
    volumeMeter->setFocusProxy(slider);
    RefreshStudioStrip();
}

void VolumeControl::RefreshStudioStrip()
{
    if (!studioStyle || !vertical || !isVisible()) return;
    OBSSource source = OBSGetStrongRef(weakSource());
    if (!source || obs_source_removed(source)) return;
    const auto vod = CurrentStudioVodRouting();
    const bool excluded = !vod.invalid && !vod.conflict && (obs_source_get_audio_mixers(source) & vod.vodMask) == 0;
    studioVodBadge->setVisible(excluded);
    studioVodBadge->setToolTip(StudioVodRoutingHint(vod));
    const QSignalBlocker block(studioEffectList);
    struct Entry { QString uuid, name; bool enabled; };
    std::vector<Entry> effects;
    obs_source_enum_filters(source, [](obs_source_t *, obs_source_t *filter, void *opaque) {
        if (obs_source_get_output_flags(filter) & OBS_SOURCE_AUDIO)
            static_cast<std::vector<Entry> *>(opaque)->push_back({QString::fromUtf8(obs_source_get_uuid(filter)),
                QString::fromUtf8(obs_source_get_name(filter)), obs_source_enabled(filter)});
    }, &effects);
    if (QApplication::mouseButtons() == Qt::NoButton) {
    while (studioEffectList->count() > int(effects.size())) delete studioEffectList->takeItem(studioEffectList->count() - 1);
    for (int row = 0; row < int(effects.size()); ++row) {
        const auto &effect = effects[row];
        auto *item = studioEffectList->item(row);
        if (!item) { item = new QListWidgetItem(studioEffectList); item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled); }
        item->setData(Qt::UserRole, effect.uuid); item->setText(effect.name);
        item->setToolTip(effect.name + QStringLiteral("\n") + QTStr("Studio.MixerEffectHint"));
        item->setCheckState(effect.enabled ? Qt::Checked : Qt::Unchecked);
    }
    int rowsHeight = 4;
    for (int row = 0; row < std::min(6, int(effects.size())); ++row)
        rowsHeight += studioEffectList->sizeHintForRow(row);
    studioEffectList->setFixedHeight(rowsHeight);
    studioEffects->setFixedHeight(std::max(78, rowsHeight + std::max(23, studioAddEffect->sizeHint().height()) + (excluded ? 14 : 0)));
    studioEffectList->setVisible(!effects.empty());
    }
    float peak = -INFINITY;
    {
        QMutexLocker lock(&volumeMeter->dataMutex);
        if (volumeMeter->currentLastUpdateTime && volumeMeter->currentLastUpdateTime + 1000000000ULL > os_gettime_ns())
            for (int channel = 0; channel < MAX_AUDIO_CHANNELS; ++channel) peak = std::max(peak, volumeMeter->currentPeak[channel]);
    }
    studioPeak->setText(std::isfinite(peak) ? QString::number(peak, 'f', 1) : QStringLiteral("−∞"));
    RefreshStudioDevice();
}

void VolumeControl::RefreshStudioDevice()
{
    OBSSource source = OBSGetStrongRef(weakSource());
    if (!source) return;
    OBSDataAutoRelease settings = obs_source_get_settings(source);
    const QString id = QString::fromUtf8(obs_data_get_string(settings, "device_id"));
    if (studioDeviceKnown && studioDeviceId == id) return;
    studioDeviceKnown = true; studioDeviceId = id;
    QString label = QString::fromUtf8(obs_source_get_display_name(obs_source_get_id(source)));
    if (!id.isEmpty()) {
        OBSProperties props = obs_source_properties(source);
        obs_property_t *property = obs_properties_get(props, "device_id");
        if (property && obs_property_get_type(property) == OBS_PROPERTY_LIST && obs_property_list_format(property) == OBS_COMBO_FORMAT_STRING) {
            label = id;
            for (size_t i = 0; i < obs_property_list_item_count(property); ++i)
                if (id == QString::fromUtf8(obs_property_list_item_string(property, i)))
                    label = QString::fromUtf8(obs_property_list_item_name(property, i));
        }
    }
    studioDevice->setText(QFontMetrics(studioDevice->font()).elidedText(label, Qt::ElideRight, 108));
    studioDevice->setToolTip(label + QStringLiteral("\n") + QTStr("Studio.AudioSourceProperties"));
    studioDevice->setAccessibleName(QTStr("Studio.AudioSourceProperties") + QStringLiteral(" — ") + label);
}

void VolumeControl::ShowStudioDevices()
{
    auto *menu = studioDevice->menu(); menu->clear();
    OBSSource source = OBSGetStrongRef(weakSource());
    if (!source) return;
    OBSDataAutoRelease settings = obs_source_get_settings(source);
    OBSProperties props = obs_source_properties(source);
    obs_property_t *property = obs_properties_get(props, "device_id");
    if (property && obs_property_get_type(property) == OBS_PROPERTY_LIST && obs_property_list_format(property) == OBS_COMBO_FORMAT_STRING) {
        const QString selected = QString::fromUtf8(obs_data_get_string(settings, "device_id"));
        for (size_t i = 0; i < obs_property_list_item_count(property); ++i) {
            const QString id = QString::fromUtf8(obs_property_list_item_string(property, i));
            auto *action = menu->addAction(QString::fromUtf8(obs_property_list_item_name(property, i)));
            action->setData(id); action->setCheckable(true); action->setChecked(id == selected);
            action->setEnabled(!obs_property_list_item_disabled(property, i));
            connect(action, &QAction::triggered, this, [this, id]() {
                OBSSource source = OBSGetStrongRef(weakSource());
                if (!source || obs_source_removed(source)) return;
                OBSDataAutoRelease settings = obs_source_get_settings(source);
                if (id == QString::fromUtf8(obs_data_get_string(settings, "device_id"))) return;
                const std::string before = obs_data_get_string(settings, "device_id"), uuid = obs_source_get_uuid(source);
                const std::string after = id.toStdString();
                RestoreDevice(uuid, after);
                auto restore = [uuid](const std::string &deviceId) { RestoreDevice(uuid, deviceId); };
                OBSBasic::Get()->undo_s.add_action(QTStr("Studio.AudioSourceProperties"), restore, restore, before, after);
                studioDeviceKnown = false; RefreshStudioDevice();
            });
        }
        menu->addSeparator();
    }
    auto *properties = menu->addAction(QTStr("Studio.AudioSourceProperties"));
    properties->setObjectName("studioMixerDeviceProperties");
    connect(properties, &QAction::triggered, this, [this]() {
        OBSSource source = OBSGetStrongRef(weakSource());
        if (source) OBSBasic::Get()->CreatePropertiesWindow(source);
    });
}

void VolumeControl::UpdateStudioButtons()
{
    const bool studio = studioStyle && vertical;
    // Studio uses text controls; inherited icon classes would overwrite them.
    utils->toggleClass(muteButton, "btn-mute", !studio);
    utils->toggleClass(monitorButton, "btn-monitor", !studio);
    if (studio) {
        muteButton->setText(QStringLiteral("MUTE")); muteButton->setIcon(QIcon());
        monitorButton->setText(QStringLiteral("CUE")); monitorButton->setIcon(QIcon());
        studioLinkButton->setText(QString()); studioLinkButton->setIcon(LinkIcon(studioLinkButton->isChecked()));
        studioLinkButton->setAccessibleName(QTStr(studioLinkButton->isChecked() ? "Studio.AudioLinkShort" : "Studio.AudioSceneShort"));
    } else {
        muteButton->setText(QString()); monitorButton->setText(QString()); studioLinkButton->setIcon(QIcon());
    }
}

void VolumeControl::setStudioStyle(bool enabled)
{
    if (studioStyle == enabled) return;
    studioStyle = enabled;
    setLayoutVertical(vertical);
    style()->unpolish(this); style()->polish(this);
    for (auto *child : findChildren<QWidget *>()) { child->style()->unpolish(child); child->style()->polish(child); }
    for (auto *parent = parentWidget(); parent; parent = parent->parentWidget()) {
        if (parent->inherits("AudioMixer")) {
            QMetaObject::invokeMethod(parent, "queueLayoutUpdate", Qt::QueuedConnection);
            break;
        }
    }
}
