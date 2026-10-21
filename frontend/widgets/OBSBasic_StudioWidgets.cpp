#include "OBSBasic.hpp"

#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QSpinBox>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

void OBSBasic::ShowStudioWidgetEditor(bool editSelected)
{
	OBSScene scene = GetStudioEditingScene();
	if (!scene || !obs_source_get_display_name("browser_source"))
		return;
	OBSSceneItem selected = GetCurrentSceneItem();
	OBSSource source = editSelected && selected ? obs_sceneitem_get_source(selected) : nullptr;
	OBSDataAutoRelease original = source ? obs_source_get_settings(source) : obs_data_create();
	if (editSelected && (!source || !obs_data_get_bool(original, "studio_widget")))
		return;
	std::string html;
	if (!GetDataFilePath("widgets/studio-widget.html", html))
		return;
	QDialog dialog(this);
	dialog.setObjectName(QStringLiteral("studioWidgetEditor"));
	dialog.setWindowTitle(QTStr("Studio.WidgetEditor"));
	auto *layout = new QVBoxLayout(&dialog);
	auto *form = new QFormLayout;
	auto *type = new QComboBox(&dialog);
	type->setObjectName(QStringLiteral("studioWidgetType"));
	type->addItem(QTStr("Studio.WidgetClock"), QStringLiteral("clock"));
	type->addItem(QTStr("Studio.WidgetCountdown"), QStringLiteral("countdown"));
	type->addItem(QTStr("Studio.WidgetLowerThird"), QStringLiteral("lower-third"));
	if (source)
		type->setCurrentIndex(std::max(0, type->findData(QString::fromUtf8(obs_data_get_string(original, "studio_widget_type")))));
	auto *title = new QLineEdit(QString::fromUtf8(obs_data_get_string(original, "studio_widget_title")), &dialog);
	title->setObjectName(QStringLiteral("studioWidgetTitle"));
	title->setMaxLength(160);
	auto *subtitle = new QLineEdit(QString::fromUtf8(obs_data_get_string(original, "studio_widget_subtitle")), &dialog);
	subtitle->setObjectName(QStringLiteral("studioWidgetSubtitle"));
	subtitle->setMaxLength(240);
	auto *seconds = new QSpinBox(&dialog);
	seconds->setObjectName(QStringLiteral("studioWidgetSeconds"));
	seconds->setRange(1, 86400);
	seconds->setValue(source ? int(obs_data_get_int(original, "studio_widget_seconds")) : 300);
	seconds->setSuffix(QStringLiteral(" s"));
	auto *font = new QSpinBox(&dialog);
	font->setObjectName(QStringLiteral("studioWidgetFont"));
	font->setRange(12, 120);
	font->setValue(source ? int(obs_data_get_int(original, "studio_widget_font")) : 64);
	form->addRow(QTStr("Studio.WidgetType"), type);
	form->addRow(QTStr("Studio.WidgetTitle"), title);
	form->addRow(QTStr("Studio.WidgetSubtitle"), subtitle);
	form->addRow(QTStr("Studio.WidgetSeconds"), seconds);
	form->addRow(QTStr("Studio.WidgetFont"), font);
	layout->addLayout(form);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
	layout->addWidget(buttons);
	connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	if (dialog.exec() != QDialog::Accepted)
		return;
	const std::string before = obs_data_get_json(original);
	OBSDataAutoRelease settings = obs_data_create_from_json(before.c_str());
	const QString kind = type->currentData().toString();
	const int64_t end = QDateTime::currentMSecsSinceEpoch() + int64_t(seconds->value()) * 1000;
	QUrl url = QUrl::fromLocalFile(QString::fromStdString(html));
	QUrlQuery query;
	query.addQueryItem(QStringLiteral("type"), kind);
	query.addQueryItem(QStringLiteral("title"), title->text());
	query.addQueryItem(QStringLiteral("subtitle"), subtitle->text());
	query.addQueryItem(QStringLiteral("end"), QString::number(end));
	query.addQueryItem(QStringLiteral("font"), QString::number(font->value()));
	url.setQuery(query);
	obs_data_set_bool(settings, "studio_widget", true);
	obs_data_set_string(settings, "studio_widget_type", kind.toUtf8().constData());
	obs_data_set_string(settings, "studio_widget_title", title->text().toUtf8().constData());
	obs_data_set_string(settings, "studio_widget_subtitle", subtitle->text().toUtf8().constData());
	obs_data_set_int(settings, "studio_widget_seconds", seconds->value());
	obs_data_set_int(settings, "studio_widget_font", font->value());
	obs_data_set_bool(settings, "is_local_file", false);
	obs_data_set_string(settings, "url", url.toEncoded().constData());
	obs_data_set_int(settings, "width", 1100);
	obs_data_set_int(settings, "height", 220);
	obs_data_set_int(settings, "fps", 60);
	obs_data_set_bool(settings, "fps_custom", true);
	obs_data_set_bool(settings, "shutdown", false);
	obs_data_set_bool(settings, "reroute_audio", true);
	if (source) {
		obs_source_update(source, settings);
		const std::string uuid = obs_source_get_uuid(source);
		auto restore = [uuid](const std::string &json) {
			OBSSourceAutoRelease source = obs_get_source_by_uuid(uuid.c_str());
			OBSDataAutoRelease settings = obs_data_create_from_json(json.c_str());
			if (source)
				obs_source_update(source, settings);
		};
		undo_s.add_action(QTStr("Studio.WidgetEditor"), restore, restore, before, obs_data_get_json(settings));
	} else {
		QString name = QTStr("Studio.WidgetPrefix") + QStringLiteral(" — ") + type->currentText();
		const QString base = name;
		for (int suffix = 2;; ++suffix) {
			OBSSourceAutoRelease existing = obs_get_source_by_name(name.toUtf8().constData());
			if (!existing)
				break;
			name = QStringLiteral("%1 (%2)").arg(base).arg(suffix);
		}
		OBSSourceAutoRelease created = obs_source_create("browser_source", name.toUtf8().constData(), settings, nullptr);
		source = created.Get();
		if (!source)
			return;
		auto *item = obs_scene_add(scene, source);
		vec2 pos{60, 60};
		obs_sceneitem_set_pos(item, &pos);
		OBSDataAutoRelease saved = obs_save_source(source);
		const std::string sourceUuid = obs_source_get_uuid(source), sceneUuid = obs_source_get_uuid(obs_scene_get_source(scene));
		auto undo = [sourceUuid](const std::string &) {
			OBSSourceAutoRelease source = obs_get_source_by_uuid(sourceUuid.c_str());
			if (source)
				obs_source_remove(source);
		};
		auto redo = [sceneUuid, pos](const std::string &json) {
			OBSSourceAutoRelease scene = obs_get_source_by_uuid(sceneUuid.c_str());
			if (!scene || !obs_scene_from_source(scene))
				return;
			OBSDataAutoRelease data = obs_data_create_from_json(json.c_str());
			OBSSourceAutoRelease source = obs_load_source(data);
			if (source)
				if (auto *item = obs_scene_add(obs_scene_from_source(scene), source))
					obs_sceneitem_set_pos(item, &pos);
		};
		undo_s.add_action(QTStr("Undo.Add").arg(name), undo, redo, "", obs_data_get_json(saved));
	}
	SaveProject();
}
