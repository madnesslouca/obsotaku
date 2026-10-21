#include "StudioInspectorDock.hpp"

#include <OBSApp.hpp>
#include <widgets/OBSBasic.hpp>
#include <properties-view.hpp>
#include <utility/PortraitGroupEdit.hpp>
#include <utility/StudioSceneEdit.hpp>
#include <QApplication>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <cmath>
#include <vector>

namespace {
bool FindInspectorPath(obs_scene_t *scene, obs_sceneitem_t *target,
                       std::vector<OBSSceneItem> &ancestors)
{
    struct Search {
        obs_sceneitem_t *target;
        std::vector<OBSSceneItem> *ancestors;
        bool found = false;
    } search{target, &ancestors};
    obs_scene_enum_items(scene, [](obs_scene_t *, obs_sceneitem_t *item, void *opaque) {
        auto &search = *static_cast<Search *>(opaque);
        if (item == search.target) {
            search.found = true;
            return false;
        }
        if (obs_sceneitem_is_group(item)) {
            search.ancestors->emplace_back(item);
            if (FindInspectorPath(obs_sceneitem_group_get_scene(item), search.target,
                                  *search.ancestors)) {
                search.found = true;
                return false;
            }
            search.ancestors->pop_back();
        }
        return true;
    }, &search);
    return search.found;
}

bool InspectorTransformAllowed(obs_scene_t *root, obs_sceneitem_t *item,
                               std::vector<OBSSceneItem> &ancestors)
{
    if (!root || !item || !obs_sceneitem_get_scene(item) || obs_sceneitem_locked(item) ||
        !FindInspectorPath(root, item, ancestors))
        return false;
    if (PortraitGroupsSharedWithMain(root, ancestors))
        return false;
    for (const auto &ancestor : ancestors) {
        if (obs_sceneitem_locked(ancestor) || !obs_sceneitem_visible(ancestor))
            return false;
    }
    return true;
}
} // namespace

StudioInspectorDock::StudioInspectorDock(QWidget *parent) : OBSDock(parent)
{
    setObjectName(QStringLiteral("studioInspectorDock"));
    setWindowTitle(QTStr("Studio.Inspector"));
    setMinimumWidth(260);
    auto *body = new QWidget(this);
    auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(14, 12, 14, 10);
    layout->setSpacing(8);
    auto *heading = new QHBoxLayout;
    nameLabel = new QLabel(body);
    nameLabel->setObjectName("studioSelectedSourceName");
    nameLabel->setTextFormat(Qt::PlainText);
    nameLabel->setWordWrap(true);
    auto font = nameLabel->font();
    font.setBold(true);
    nameLabel->setFont(font);
    heading->addWidget(nameLabel, 1);
    propertiesButton = new QPushButton(QStringLiteral("⋯"), body);
    propertiesButton->setObjectName(QStringLiteral("studioSourceProperties"));
    propertiesButton->setToolTip(QTStr("Studio.Properties"));
    propertiesButton->setFixedWidth(26);
    heading->addWidget(propertiesButton);
    layout->addLayout(heading);
    detailsLabel = new QLabel(body);
    detailsLabel->setObjectName(QStringLiteral("studioSourceDetails"));
    detailsLabel->setTextFormat(Qt::PlainText);
    detailsLabel->setWordWrap(true);
    layout->addWidget(detailsLabel);

    transformSection = new QWidget(body);
    auto *grid = new QGridLayout(transformSection);
    grid->setContentsMargins(0, 10, 0, 10);
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(6);
    auto field = [this, grid](const char *name, const QString &prefix, int row, int col) {
        auto *spin = new QDoubleSpinBox(transformSection);
        spin->setObjectName(QString::fromUtf8(name));
        spin->setRange(-100000, 100000);
        spin->setDecimals(1);
        spin->setPrefix(prefix + QStringLiteral("  "));
        spin->setAccessibleName(prefix);
        spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        grid->addWidget(spin, row, col);
        connect(spin, &QDoubleSpinBox::editingFinished, this, &StudioInspectorDock::CommitTransform);
        return spin;
    };
    positionX = field("studioPositionX", "X", 0, 0);
    positionY = field("studioPositionY", "Y", 0, 1);
    sizeX = field("studioWidth", QTStr("Studio.Width"), 1, 0);
    sizeY = field("studioHeight", QTStr("Studio.Height"), 1, 1);
    sizeX->setMinimum(0.1);
    sizeY->setMinimum(0.1);
    rotation = field("studioRotation", QTStr("Studio.Rotation"), 2, 0);
    rotation->setSuffix(QStringLiteral("°"));
    transformButton = new QPushButton(QTStr("Studio.Advanced"), body);
    transformButton->setObjectName(QStringLiteral("studioSourceTransform"));
    grid->addWidget(transformButton, 2, 1);
    layout->addWidget(transformSection);

    auto *sourceLabel = new QLabel(QTStr("Studio.Source"), body);
    sourceLabel->setObjectName(QStringLiteral("studioSectionLabel"));
    layout->addWidget(sourceLabel);
    propertiesLayout = new QVBoxLayout;
    propertiesLayout->setContentsMargins(0, 0, 0, 0);
    layout->addLayout(propertiesLayout, 1);
    applyButton = new QPushButton(QTStr("Apply"), body);
    applyButton->hide();
    layout->addWidget(applyButton);
    connect(applyButton, &QPushButton::clicked, this, [this]() {
        if (propertiesView)
            propertiesView->UpdateSettings();
    });
    filtersButton = new QPushButton(QTStr("Studio.AddEffect"), body);
    filtersButton->setObjectName(QStringLiteral("studioSourceFilters"));
    layout->addWidget(filtersButton);
    setWidget(body);
    connect(propertiesButton, &QPushButton::clicked, this, &StudioInspectorDock::propertiesRequested);
    connect(filtersButton, &QPushButton::clicked, this, &StudioInspectorDock::filtersRequested);
    connect(transformButton, &QPushButton::clicked, this, &StudioInspectorDock::transformRequested);
    Refresh(nullptr, 0);
}

void StudioInspectorDock::RecordPropertyUndo(void *object, obs_data_t *before, obs_data_t *after)
{
    auto *source = static_cast<obs_source_t *>(object);
    auto *main = OBSBasic::Get();
    if (!source || obs_source_removed(source) || !main || main->isClosing())
        return;
    OBSDataAutoRelease previous;
    if (!before) {
        previous = obs_source_get_settings(source);
        before = previous;
    }
    const std::string oldJson = obs_data_get_json(before);
    const std::string newJson = obs_data_get_json(after);
    if (previous)
        obs_source_update(source, after);
    if (oldJson == newJson)
        return;
    const std::string uuid = obs_source_get_uuid(source);
    auto apply = [uuid](const std::string &json) {
        OBSSourceAutoRelease target = obs_get_source_by_uuid(uuid.c_str());
        if (!target)
            return;
        OBSDataAutoRelease settings = obs_data_create_from_json(json.c_str());
        obs_source_reset_settings(target, settings);
        obs_source_update_properties(target);
    };
    main->undo_s.add_action(QTStr("Undo.Properties").arg(obs_source_get_name(source)),
                            apply, apply, oldJson, newJson);
    main->SaveProject();
}

void StudioInspectorDock::BindSource(obs_source_t *source)
{
    delete propertiesView;
    propertiesView = nullptr;
    selectedSource = source;
    applyButton->hide();
    if (!source || !obs_source_configurable(source))
        return;
    // Dynamic properties can install device-specific defaults. Hydrate those
    // before cloning; obs_data_apply copies user values only, not defaults.
    OBSProperties nativeProperties(obs_source_properties(source));
    OBSDataAutoRelease current = obs_source_get_settings(source);
    OBSDataAutoRelease settings = obs_data_create();
    obs_data_apply(settings, current);
    OBSDataAutoRelease defaults = obs_data_get_defaults(current);
    for (obs_data_item_t *item = obs_data_first(defaults); item; obs_data_item_next(&item)) {
        const char *name = obs_data_item_get_name(item);
        switch (obs_data_item_gettype(item)) {
        case OBS_DATA_STRING:
            obs_data_set_default_string(settings, name, obs_data_item_get_string(item));
            break;
        case OBS_DATA_NUMBER:
            if (obs_data_item_numtype(item) == OBS_DATA_NUM_INT)
                obs_data_set_default_int(settings, name, obs_data_item_get_int(item));
            else
                obs_data_set_default_double(settings, name, obs_data_item_get_double(item));
            break;
        case OBS_DATA_BOOLEAN:
            obs_data_set_default_bool(settings, name, obs_data_item_get_bool(item));
            break;
        case OBS_DATA_OBJECT: {
            OBSDataAutoRelease value = obs_data_item_get_obj(item);
            obs_data_set_default_obj(settings, name, value);
            break;
        }
        case OBS_DATA_ARRAY: {
            OBSDataArrayAutoRelease value = obs_data_item_get_array(item);
            obs_data_set_default_array(settings, name, value);
            break;
        }
        default:
            break;
        }
    }
    propertiesView = new OBSPropertiesView(settings.Get(), source,
        (PropertiesReloadCallback)obs_source_properties, RecordPropertyUndo,
        (PropertiesVisualUpdateCb)obs_source_update);
    propertiesView->setObjectName(QStringLiteral("studioInlineProperties"));
    propertiesView->setMinimumSize(0, 0);
    propertiesLayout->addWidget(propertiesView);
    connect(propertiesView, &OBSPropertiesView::PropertiesRefreshed, this, [this]() {
        if (!propertiesView)
            return;
        for (auto *form : propertiesView->findChildren<QFormLayout *>())
            form->setRowWrapPolicy(QFormLayout::WrapAllRows);
        propertiesView->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
        for (auto *label : propertiesView->findChildren<QLabel *>())
            label->setWordWrap(true);
        for (auto *combo : propertiesView->findChildren<QComboBox *>()) {
            combo->setMinimumContentsLength(8);
            combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        }
        applyButton->setVisible(propertiesView->DeferUpdate());
    });
}

void StudioInspectorDock::CommitTransform()
{
    auto *main = OBSBasic::Get();
    auto *edited = qobject_cast<QDoubleSpinBox *>(sender());
    if (!main || main->isClosing() || !selectedItem ||
        !edited || !obs_sceneitem_get_scene(selectedItem) || obs_sceneitem_locked(selectedItem))
        return;
    OBSSource root = OBSGetStrongRef(selectedScene);
    OBSScene scene = root ? obs_scene_from_source(root) : nullptr;
    std::vector<OBSSceneItem> ancestors;
    if (!scene || main->GetStudioEditingScene() != scene ||
        !InspectorTransformAllowed(scene, selectedItem, ancestors))
        return;
    // Group autosizing can still be queued by the renderer. Settle before the
    // history baseline, then apply only the field whose editing finished;
    // other displayed fields may predate a sibling's coordinate rebase.
    ApplyStudioSceneEdit(scene, [](obs_scene_t *scene) { obs_scene_prune_sources(scene); });
    ancestors.clear();
    if (!InspectorTransformAllowed(scene, selectedItem, ancestors))
        return;
    const bool grouped = !ancestors.empty();
    std::string oldJson;
    if (grouped) {
        oldJson = CapturePortraitGroupTransforms(scene, ancestors.front());
    } else {
        OBSDataAutoRelease before = obs_scene_save_transform_states(scene, false);
        oldJson = obs_data_get_json(before);
    }
    obs_transform_info info;
    obs_sceneitem_get_info2(selectedItem, &info);
    if (edited == positionX)
        info.pos.x = positionX->value();
    else if (edited == positionY)
        info.pos.y = positionY->value();
    else if (edited == rotation)
        info.rot = rotation->value();
    if (info.bounds_type != OBS_BOUNDS_NONE) {
        if (edited == sizeX)
            info.bounds.x = sizeX->value();
        else if (edited == sizeY)
            info.bounds.y = sizeY->value();
    } else {
        obs_sceneitem_crop crop;
        obs_sceneitem_get_crop(selectedItem, &crop);
        const int width = int(obs_source_get_width(selectedSource)) - crop.left - crop.right;
        const int height = int(obs_source_get_height(selectedSource)) - crop.top - crop.bottom;
        if (edited == sizeX && width > 0)
            info.scale.x = std::copysign(sizeX->value() / width, info.scale.x);
        if (edited == sizeY && height > 0)
            info.scale.y = std::copysign(sizeY->value() / height, info.scale.y);
    }
    FreezePortraitGroups freeze(ancestors);
    ApplyStudioSceneEdit(scene, [&](obs_scene_t *) {
        obs_sceneitem_set_info2(selectedItem, &info);
        obs_sceneitem_force_update_transform(selectedItem);
    });
    freeze.ReleaseAndSettle(scene);
    std::string newJson;
    if (grouped) {
        newJson = CapturePortraitGroupTransforms(scene, ancestors.front());
    } else {
        OBSDataAutoRelease after = obs_scene_save_transform_states(scene, false);
        newJson = obs_data_get_json(after);
    }
    if (oldJson != newJson) {
        const std::string rootUuid = obs_source_get_uuid(root);
        auto apply = [grouped, rootUuid](const std::string &json) {
            auto *main = OBSBasic::Get();
            OBSSourceAutoRelease target = obs_get_source_by_uuid(rootUuid.c_str());
            if (!main || main->isClosing() || !target || obs_source_removed(target))
                return;
            main->SetStudioEditingScene(target);
            if (grouped) {
                RestorePortraitGroupTransforms(json);
            } else {
                OBSScene targetScene = obs_scene_from_source(target);
                if (targetScene)
                    ApplyStudioSceneEdit(targetScene, [&json](obs_scene_t *) {
                        obs_scene_load_transform_states(json.c_str());
                    });
            }
            main->SaveProject();
        };
        main->undo_s.add_action(QTStr("Studio.Transform"), apply, apply, oldJson, newJson);
        main->SaveProject();
    }
}

void StudioInspectorDock::Refresh(obs_scene_item *item, int selectionCount)
{
    obs_source_t *source = item && obs_sceneitem_get_scene(item) && selectionCount == 1 ? obs_sceneitem_get_source(item) : nullptr;
    selectedItem = source ? item : nullptr;
    selectedScene = source ? OBSGetWeakRef(obs_scene_get_source(OBSBasic::Get()->GetStudioEditingScene())) : OBSWeakSource{};
    if (selectedSource.Get() != source) {
        BindSource(source);
    } else if (source && propertiesView && !propertiesView->isAncestorOf(QApplication::focusWidget())) {
        OBSDataAutoRelease settings = obs_source_get_settings(source);
        if (strcmp(obs_data_get_json(settings), obs_data_get_json(propertiesView->GetSettings())) != 0)
            BindSource(source);
    }
    const bool video = source && (obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO);
    propertiesButton->setEnabled(source && obs_source_configurable(source));
    filtersButton->setEnabled(source && !obs_source_is_group(source));
    transformButton->setEnabled(video);
    transformSection->setVisible(video);
    if (!source) {
        nameLabel->setText(QTStr(selectionCount > 1 ? "Studio.MultipleSelection" : "Studio.SelectLayer"));
        detailsLabel->setText(QTStr("Studio.InspectorHint"));
        return;
    }
    nameLabel->setText(QString::fromUtf8(obs_source_get_name(source)));
    const char *type = obs_source_get_display_name(obs_source_get_id(source));
    detailsLabel->setText(QString::fromUtf8(type ? type : ""));
    if (!video)
        return;
    std::vector<OBSSceneItem> ancestors;
    const bool editable = InspectorTransformAllowed(OBSBasic::Get()->GetStudioEditingScene(), item, ancestors);
    transformSection->setEnabled(editable);
    for (auto *spin : {positionX, positionY, sizeX, sizeY, rotation}) {
        if (spin->hasFocus() || spin->isAncestorOf(QApplication::focusWidget()))
            return;
    }
    obs_transform_info info;
    obs_sceneitem_get_info2(item, &info);
    obs_sceneitem_crop crop;
    obs_sceneitem_get_crop(item, &crop);
    positionX->setValue(info.pos.x);
    positionY->setValue(info.pos.y);
    rotation->setValue(info.rot);
    sizeX->setValue(info.bounds_type != OBS_BOUNDS_NONE ? info.bounds.x :
        std::abs(info.scale.x) * (int(obs_source_get_width(source)) - crop.left - crop.right));
    sizeY->setValue(info.bounds_type != OBS_BOUNDS_NONE ? info.bounds.y :
        std::abs(info.scale.y) * (int(obs_source_get_height(source)) - crop.top - crop.bottom));
}
