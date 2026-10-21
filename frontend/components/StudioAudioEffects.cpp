#include "StudioAudioEffects.hpp"
#include "StudioEffectControls.hpp"

#include <widgets/OBSBasic.hpp>
#include <properties-view.hpp>
#include <QCheckBox>
#include <QCloseEvent>
#include <QFrame>
#include <QGuiApplication>
#include <QLabel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QShortcut>
#include <QSignalBlocker>
#include <QTimer>
#include <QTabBar>
#include <QVBoxLayout>
#include <algorithm>

namespace {
QPointer<QFrame> currentPopup;

OBSSource FindFilter(obs_source_t *parent, const QString &uuid)
{
	struct Search { QString uuid; OBSSource found; } search{uuid, {}};
	if (parent) obs_source_enum_filters(parent, [](obs_source_t *, obs_source_t *filter, void *opaque) {
		auto &search = *static_cast<Search *>(opaque);
		if (search.uuid == QString::fromUtf8(obs_source_get_uuid(filter))) search.found = filter;
	}, &search);
	return search.found;
}

void Save() { OBSBasic::Get()->SaveProject(); }

void SetIndex(obs_source_t *parent, obs_source_t *filter, int index)
{
	const size_t count = obs_source_filter_count(parent);
	if (count) obs_source_filter_set_index(parent, filter, std::min(size_t(std::max(0, index)), count - 1));
}

void RestoreSettings(const std::string &parentUUID, const QString &filterUUID, const std::string &json)
{
	OBSSourceAutoRelease parent = obs_get_source_by_uuid(parentUUID.c_str());
	OBSSource filter = FindFilter(parent, filterUUID);
	if (!filter || obs_source_removed(parent)) return;
	OBSDataAutoRelease settings = obs_data_create_from_json(json.c_str());
	OBSDataAutoRelease current = obs_source_get_settings(filter);
	obs_data_clear(current);
	obs_source_update(filter, settings);
	obs_source_update_properties(filter);
	Save();
}

// The native property view previews continuously, then commits one undo step
// after a gesture. Never retain a removed filter through an editor or undo lambda.
void PreviewSettings(void *opaque, obs_data_t *settings)
{
	auto *filter = static_cast<obs_source_t *>(opaque);
	if (filter && obs_filter_get_parent(filter)) obs_source_update(filter, settings);
}

void CommitSettings(void *opaque, obs_data_t *before, obs_data_t *after)
{
	auto *filter = static_cast<obs_source_t *>(opaque);
	auto *parent = filter ? obs_filter_get_parent(filter) : nullptr;
	if (!parent || obs_source_removed(parent) || !before || !after) return;
	const std::string oldJson = obs_data_get_json(before), newJson = obs_data_get_json(after);
	if (oldJson == newJson) return;
	const std::string parentUUID = obs_source_get_uuid(parent);
	const QString filterUUID = QString::fromUtf8(obs_source_get_uuid(filter));
	auto restore = [parentUUID, filterUUID](const std::string &json) { RestoreSettings(parentUUID, filterUUID, json); };
	OBSBasic::Get()->undo_s.add_action(QTStr("Undo.Filters").arg(obs_source_get_name(filter)),
					 restore, restore, oldJson, newJson, false);
	obs_source_update(filter, after);
	Save();
}

void RestoreFilter(const std::string &parentUUID, const std::string &snapshot, int index)
{
	OBSSourceAutoRelease parent = obs_get_source_by_uuid(parentUUID.c_str());
	if (!parent || obs_source_removed(parent)) return;
	OBSDataAutoRelease data = obs_data_create_from_json(snapshot.c_str());
	if (FindFilter(parent, QString::fromUtf8(obs_data_get_string(data, "uuid")))) return;
	OBSSourceAutoRelease filter = obs_load_source(data);
	if (!filter) return;
	obs_source_filter_add(parent, filter);
	SetIndex(parent, filter, index);
	Save();
}

void RemoveFilter(const std::string &parentUUID, const QString &filterUUID)
{
	OBSSourceAutoRelease parent = obs_get_source_by_uuid(parentUUID.c_str());
	OBSSource filter = FindFilter(parent, filterUUID);
	if (filter) { obs_source_filter_remove(parent, filter); Save(); }
}

class EffectPopup final : public QFrame {
	QPointer<QWidget> anchor;
	OBSWeakSource weakSource;
	QString filterUUID;
	OBSPropertiesView *view = nullptr;
	StudioEffectControls *compact = nullptr;
	OBSSignal propertiesSignal;

	void closeEvent(QCloseEvent *event) override
	{
		// Flush the property view's pending gesture before another chain action
		// changes undo order. Disconnect reloads before deleting its controls.
		propertiesSignal.Disconnect();
		delete compact;
		compact = nullptr;
		delete view;
		view = nullptr;
		QFrame::closeEvent(event);
	}

public:
	QVBoxLayout *body;
	EffectPopup(QWidget *anchor_, obs_source_t *source, const char *name, const QString &uuid = {})
		: QFrame(anchor_->window(), Qt::Popup), anchor(anchor_), weakSource(OBSGetWeakRef(source)), filterUUID(uuid)
	{
		setObjectName(name);
		setWindowModality(Qt::NonModal);
		setAttribute(Qt::WA_DeleteOnClose);
		setFrameShape(QFrame::StyledPanel);
		body = new QVBoxLayout(this);
		body->setContentsMargins(10, 8, 10, 8);
		body->setSpacing(8);
		for (const auto key : {QKeySequence::Undo, QKeySequence::Redo}) {
			auto *shortcut = new QShortcut(QKeySequence(key), this);
			shortcut->setContext(Qt::WidgetWithChildrenShortcut);
			connect(shortcut, &QShortcut::activated, this, [this, key]() {
				if (view) view->FlushPendingChanges();
				if (compact) compact->Flush();
				if (auto *action = OBSBasic::Get()->findChild<QAction *>(key == QKeySequence::Undo ? "actionMainUndo" : "actionMainRedo")) action->trigger();
			});
		}
		auto *watch = new QTimer(this);
		watch->setInterval(100);
		connect(watch, &QTimer::timeout, this, [this]() {
			OBSSource source = OBSGetStrongRef(weakSource);
			if (!anchor || !anchor->isVisible() || !source || obs_source_removed(source) ||
			    (!filterUUID.isEmpty() && !FindFilter(source, filterUUID))) close();
		});
		watch->start();
	}

	void open()
	{
		adjustSize();
		const QRect bounds = anchor->screen()->availableGeometry();
		resize(std::min(width(), bounds.width()), std::min(height(), bounds.height()));
		QPoint point = anchor->mapToGlobal(QPoint(anchor->width() + 8, 0));
		if (point.x() + width() > bounds.right() + 1) point.setX(anchor->mapToGlobal(QPoint()).x() - width() - 8);
		point.setX(std::clamp(point.x(), bounds.left(), bounds.right() + 1 - width()));
		point.setY(std::clamp(point.y(), bounds.top(), bounds.bottom() + 1 - height()));
		move(point);
		show();
		currentPopup = this;
	}

	void edit(obs_source_t *source, obs_source_t *filter)
	{
		const bool curated = StudioEffectControls::Supports(obs_source_get_id(filter));
		setFixedWidth(curated ? 340 : 440);
		auto *header = new QHBoxLayout;
		auto *enabled = new QCheckBox(QString::fromUtf8(obs_source_get_name(filter)), this);
		enabled->setObjectName("studioEffectEnabled");
		enabled->setChecked(obs_source_enabled(filter));
		header->addWidget(enabled, 1);
		if (curated) {
			auto *reset = new QPushButton(QStringLiteral("↶"), this);
			reset->setObjectName("studioEffectReset"); reset->setFixedWidth(28);
			reset->setToolTip(QTStr("Studio.EffectReset")); reset->setAccessibleName(QTStr("Studio.EffectReset"));
			header->addWidget(reset);
			connect(reset, &QPushButton::clicked, this, [this]() { if (compact) compact->Reset(); });
		}
		auto *closeButton = new QPushButton(QStringLiteral("×"), this);
		closeButton->setObjectName("studioEffectClose");
		closeButton->setAccessibleName(QTStr("Close"));
		closeButton->setFixedWidth(28);
		header->addWidget(closeButton);
		connect(closeButton, &QPushButton::clicked, this, &QWidget::close);
		body->addLayout(header);
		const std::string parentUUID = obs_source_get_uuid(source);
		const QString uuid = filterUUID;
		connect(enabled, &QCheckBox::toggled, this, [this, parentUUID, uuid](bool on) {
			if (view) view->FlushPendingChanges();
			if (compact) compact->Flush();
			OBSSourceAutoRelease parent = obs_get_source_by_uuid(parentUUID.c_str());
			OBSSource filter = FindFilter(parent, uuid);
			if (!filter) return;
			const bool before = obs_source_enabled(filter);
			if (before == on) return;
			auto restore = [parentUUID, uuid](const std::string &value) {
				OBSSourceAutoRelease parent = obs_get_source_by_uuid(parentUUID.c_str());
				OBSSource filter = FindFilter(parent, uuid);
				if (filter) obs_source_set_enabled(filter, value == "on");
				Save();
			};
			obs_source_set_enabled(filter, on);
			OBSBasic::Get()->undo_s.add_action(QTStr("Studio.MixerEffectToggle"), restore, restore,
						 before ? "on" : "off", on ? "on" : "off", false);
			Save();
		});
		auto *watch = new QTimer(this);
		connect(watch, &QTimer::timeout, this, [this, enabled]() {
			OBSSource source = OBSGetStrongRef(weakSource);
			OBSSource filter = FindFilter(source, filterUUID);
			if (!filter) return;
			const QSignalBlocker block(enabled);
			enabled->setChecked(obs_source_enabled(filter));
			enabled->setText(QString::fromUtf8(obs_source_get_name(filter)));
		});
		watch->start(150);
		if (curated) {
			compact = new StudioEffectControls(filter, [parentUUID, uuid](const std::string &before, const std::string &after) {
				auto restore = [parentUUID, uuid](const std::string &json) { RestoreSettings(parentUUID, uuid, json); };
				OBSBasic::Get()->undo_s.add_action(QTStr("Studio.EffectEdit"), restore, restore, before, after, false);
				Save();
			}, this);
			body->addWidget(compact);
		} else {
		OBSDataAutoRelease settings = obs_source_get_settings(filter);
		view = new OBSPropertiesView(settings.Get(), filter, (PropertiesReloadCallback)obs_source_properties,
			(PropertiesUpdateCallback)CommitSettings, (PropertiesVisualUpdateCb)PreviewSettings);
		view->setObjectName("studioEffectProperties");
		view->SetDeferrable(false);
		view->setMinimumHeight(160);
		view->setMaximumHeight(390);
		body->addWidget(view);
		connect(view, &OBSPropertiesView::PropertiesRefreshed, this, [this]() {
			auto *content = view ? static_cast<QScrollArea *>(view)->widget() : nullptr;
			if (!content || !anchor) return;
			const int available = std::max(100, std::min(390, anchor->screen()->availableGeometry().height() - 110));
			view->setFixedHeight(std::clamp(content->minimumSizeHint().height() + 8, 100, available));
			open();
		});
		propertiesSignal.Connect(obs_source_get_signal_handler(filter), "update_properties", [](void *opaque, calldata_t *) {
			auto *popup = static_cast<EffectPopup *>(opaque);
			if (popup->view) QMetaObject::invokeMethod(popup->view, "ReloadProperties", Qt::QueuedConnection);
		}, this);
		}
		auto *tools = new QHBoxLayout;
		for (const int direction : {-1, 1}) {
			auto *button = new QPushButton(QTStr(direction < 0 ? "Studio.EffectUp" : "Studio.EffectDown"), this);
			button->setObjectName(direction < 0 ? "studioEffectUp" : "studioEffectDown");
			tools->addWidget(button);
			connect(button, &QPushButton::clicked, this, [parentUUID, uuid, direction]() {
				OBSSourceAutoRelease parent = obs_get_source_by_uuid(parentUUID.c_str());
				if (parent) MoveStudioAudioEffect(parent, uuid, direction);
			});
		}
		tools->addStretch();
		auto *remove = new QPushButton(QTStr("Remove"), this);
		remove->setObjectName("studioEffectRemove");
		tools->addWidget(remove);
		connect(remove, &QPushButton::clicked, this, [parentUUID, uuid]() {
			OBSSourceAutoRelease parent = obs_get_source_by_uuid(parentUUID.c_str());
			if (parent) RemoveStudioAudioEffect(parent, uuid);
		});
		body->addLayout(tools);
	}
};

void CloseCurrent()
{
	if (currentPopup) currentPopup->close();
	currentPopup = nullptr;
}
}

void ShowStudioAudioEffectPicker(QWidget *anchor, obs_source_t *source)
{
	CloseCurrent();
	if (!source || obs_source_removed(source)) return;
	auto *popup = new EffectPopup(anchor, source, "studioEffectPicker");
	popup->setFixedWidth(300);
	popup->body->addWidget(new QLabel(QTStr("Studio.MixerAddEffect"), popup));
	auto *search = new QLineEdit(popup);
	search->setObjectName("studioEffectSearch");
	search->setPlaceholderText(QTStr("Studio.EffectSearch"));
	popup->body->addWidget(search);
	auto *tabs = new QTabBar(popup);
	tabs->setObjectName("studioEffectTabs");
	tabs->addTab(QTStr("Studio.EffectBuiltins")); tabs->addTab(QTStr("Studio.EffectMore"));
	const bool hasVst3 = obs_source_get_display_name("vst3_filter") != nullptr;
	if (hasVst3) tabs->addTab(QStringLiteral("VST3"));
	popup->body->addWidget(tabs);
	auto *list = new QListWidget(popup);
	list->setObjectName("studioEffectCatalog");
	list->setMinimumHeight(220);
	const char *type = nullptr;
	for (size_t i = 0; obs_enum_filter_types(i, &type); ++i) {
		if (QByteArray(type) == "vst3_filter") continue;
		const uint32_t flags = obs_get_source_output_flags(type);
		if (!(flags & OBS_SOURCE_AUDIO) || (flags & OBS_SOURCE_VIDEO) ||
		    (flags & (OBS_SOURCE_DEPRECATED | OBS_SOURCE_CAP_DISABLED | OBS_SOURCE_CAP_OBSOLETE))) continue;
		auto *item = new QListWidgetItem(QString::fromUtf8(obs_source_get_display_name(type)), list);
		item->setData(Qt::UserRole, QString::fromUtf8(type));
		item->setData(Qt::UserRole + 1, StudioEffectControls::Supports(type));
		if (QByteArray(type) == "gain_filter") item->setText(item->text() + QStringLiteral(" (dB)"));
	}
	list->sortItems();
	popup->body->addWidget(list);
	auto *vstHint = new QLabel(QTStr("Studio.Vst3Hint"), popup);
	vstHint->setObjectName("studioVst3Hint"); vstHint->setWordWrap(true);
	popup->body->addWidget(vstHint);
	auto *rescan = new QPushButton(QTStr("Studio.Vst3Rescan"), popup);
	rescan->setObjectName("studioVst3Rescan"); popup->body->addWidget(rescan);
	QObject::connect(rescan, &QPushButton::clicked, popup, []() {
		calldata_t args; calldata_init(&args);
		proc_handler_call(obs_get_proc_handler(), "studio_vst3_rescan", &args);
		calldata_free(&args);
	});
	auto refreshVst = [list, hasVst3]() {
		if (!hasVst3) return;
		OBSProperties properties = obs_get_source_properties("vst3_filter");
		auto *catalog = obs_properties_get(properties, "vst3_plugin");
		if (!catalog) return;
		for (size_t i = 0; i < obs_property_list_item_count(catalog); ++i) {
			const QString id = QString::fromUtf8(obs_property_list_item_string(catalog, i));
			if (id.isEmpty()) continue;
			bool found = false;
			for (int row = 0; row < list->count(); ++row) if (list->item(row)->data(Qt::UserRole + 2).toString() == id) { found = true; break; }
			if (found) continue;
			auto *item = new QListWidgetItem(QString::fromUtf8(obs_property_list_item_name(catalog, i)), list);
			item->setData(Qt::UserRole, QStringLiteral("vst3_filter"));
			item->setData(Qt::UserRole + 1, false);
			item->setData(Qt::UserRole + 2, id);
		}
	};
	refreshVst();
	auto filterItems = [list, tabs, search, vstHint, rescan]() {
		vstHint->setVisible(tabs->currentIndex() == 2); rescan->setVisible(tabs->currentIndex() == 2);
		for (int row = 0; row < list->count(); ++row) {
			auto *item = list->item(row);
			const int category = !item->data(Qt::UserRole + 2).toString().isEmpty() ? 2 : item->data(Qt::UserRole + 1).toBool() ? 0 : 1;
			item->setHidden(category != tabs->currentIndex() || !item->text().contains(search->text(), Qt::CaseInsensitive));
		}
	};
	QObject::connect(search, &QLineEdit::textChanged, list, filterItems);
	QObject::connect(tabs, &QTabBar::currentChanged, list, filterItems);
	filterItems();
	if (hasVst3) {
		auto *timer = new QTimer(popup);
		QObject::connect(timer, &QTimer::timeout, popup, [refreshVst, filterItems]() { refreshVst(); filterItems(); });
		timer->start(500);
	}
	const std::string parentUUID = obs_source_get_uuid(source);
	auto add = [popup, list, parentUUID](QListWidgetItem *item) {
		if (!list->isEnabled()) return;
		list->setEnabled(false);
		const QByteArray type = item->data(Qt::UserRole).toString().toUtf8();
		const QByteArray pluginId = item->data(Qt::UserRole + 2).toString().toUtf8();
		const QString pluginName = item->text();
		popup->close();
		OBSSourceAutoRelease parent = obs_get_source_by_uuid(parentUUID.c_str());
		if (!parent || obs_source_removed(parent)) return;
		const QString base = pluginId.isEmpty() ? QString::fromUtf8(obs_source_get_display_name(type.constData())) : pluginName;
		QString name = base;
		for (int suffix = 2; ; ++suffix) {
			OBSSourceAutoRelease existing = obs_source_get_filter_by_name(parent, name.toUtf8().constData());
			if (!existing) break;
			name = base + QStringLiteral(" %1").arg(suffix);
		}
		OBSDataAutoRelease settings = obs_data_create();
		if (!pluginId.isEmpty()) obs_data_set_string(settings, "vst3_plugin", pluginId.constData());
		OBSSourceAutoRelease filter = obs_source_create(type.constData(), name.toUtf8().constData(), settings, nullptr);
		if (!filter) { blog(LOG_WARNING, "Studio: failed to create audio filter '%s'", type.constData()); return; }
		obs_source_filter_add(parent, filter);
		const int index = obs_source_filter_get_index(parent, filter);
		const QString uuid = QString::fromUtf8(obs_source_get_uuid(filter));
		OBSDataAutoRelease data = obs_save_source(filter);
		const std::string snapshot = obs_data_get_json(data);
		auto undo = [parentUUID, uuid](const std::string &) { RemoveFilter(parentUUID, uuid); };
		auto redo = [parentUUID, snapshot, index](const std::string &) { RestoreFilter(parentUUID, snapshot, index); };
		OBSBasic::Get()->undo_s.add_action(QTStr("Undo.Add").arg(name), undo, redo, "", "", false);
		Save();
	};
	QObject::connect(list, &QListWidget::itemClicked, popup, add);
	QObject::connect(list, &QListWidget::itemActivated, popup, add);
	popup->open();
	search->setFocus();
}

void ShowStudioAudioEffectEditor(QWidget *anchor, obs_source_t *source, const QString &filterUUID)
{
	CloseCurrent();
	OBSSource filter = FindFilter(source, filterUUID);
	if (!filter || !source || obs_source_removed(source)) return;
	auto *popup = new EffectPopup(anchor, source, "studioEffectEditor", filterUUID);
	popup->edit(source, filter);
	popup->open();
}

void MoveStudioAudioEffect(obs_source_t *source, const QString &uuid, int direction)
{
	CloseCurrent();
	OBSSource filter = FindFilter(source, uuid);
	if (!filter || obs_source_removed(source)) return;
	const std::string parentUUID = obs_source_get_uuid(source);
	const int before = obs_source_filter_get_index(source, filter);
	obs_source_filter_set_order(source, filter, direction < 0 ? OBS_ORDER_MOVE_UP : OBS_ORDER_MOVE_DOWN);
	const int after = obs_source_filter_get_index(source, filter);
	if (before == after) return;
	auto restore = [parentUUID, uuid](const std::string &value) {
		OBSSourceAutoRelease parent = obs_get_source_by_uuid(parentUUID.c_str());
		OBSSource filter = FindFilter(parent, uuid);
		if (filter) SetIndex(parent, filter, std::stoi(value));
		Save();
	};
	OBSBasic::Get()->undo_s.add_action(QTStr("Studio.EffectOrder"), restore, restore, std::to_string(before), std::to_string(after), false);
	Save();
}

void RemoveStudioAudioEffect(obs_source_t *source, const QString &uuid)
{
	CloseCurrent();
	OBSSource filter = FindFilter(source, uuid);
	if (!filter || obs_source_removed(source)) return;
	const std::string parentUUID = obs_source_get_uuid(source);
	const int index = obs_source_filter_get_index(source, filter);
	OBSDataAutoRelease data = obs_save_source(filter);
	const std::string snapshot = obs_data_get_json(data);
	auto undo = [parentUUID, snapshot, index](const std::string &) { RestoreFilter(parentUUID, snapshot, index); };
	auto redo = [parentUUID, uuid](const std::string &) { RemoveFilter(parentUUID, uuid); };
	RemoveFilter(parentUUID, uuid);
	OBSBasic::Get()->undo_s.add_action(QTStr("Undo.Delete").arg(obs_source_get_name(filter)), undo, redo, "", "", false);
}

void ReorderStudioAudioEffects(obs_source_t *source, const QStringList &audioOrder)
{
	CloseCurrent();
	if (!source || obs_source_removed(source)) return;
	struct Order { QStringList all, audio; } order;
	obs_source_enum_filters(source, [](obs_source_t *, obs_source_t *filter, void *opaque) {
		auto &order = *static_cast<Order *>(opaque);
		const QString uuid = QString::fromUtf8(obs_source_get_uuid(filter));
		order.all.append(uuid);
		if (obs_source_get_output_flags(filter) & OBS_SOURCE_AUDIO) order.audio.append(uuid);
	}, &order);
	QStringList sortedBefore = order.audio, sortedAfter = audioOrder;
	sortedBefore.sort(); sortedAfter.sort();
	if (sortedBefore != sortedAfter || order.audio == audioOrder) return;
	QStringList next = order.all;
	for (int row = 0, audioRow = 0; row < next.size(); ++row)
		if (order.audio.contains(next[row])) next[row] = audioOrder[audioRow++];
	const std::string parentUUID = obs_source_get_uuid(source);
	auto restore = [parentUUID](const std::string &json) {
		OBSSourceAutoRelease parent = obs_get_source_by_uuid(parentUUID.c_str());
		if (!parent || obs_source_removed(parent)) return;
		const auto array = QJsonDocument::fromJson(QByteArray::fromStdString(json)).array();
		for (int row = 0; row < array.size(); ++row) {
			OBSSource filter = FindFilter(parent, array[row].toString());
			if (filter) SetIndex(parent, filter, int(array.size()) - 1 - row);
		}
		Save();
	};
	auto encode = [](const QStringList &list) { return QJsonDocument(QJsonArray::fromStringList(list)).toJson(QJsonDocument::Compact).toStdString(); };
	const std::string before = encode(order.all), after = encode(next);
	restore(after);
	OBSBasic::Get()->undo_s.add_action(QTStr("Studio.EffectOrder"), restore, restore, before, after, false);
}
