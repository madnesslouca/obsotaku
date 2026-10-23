/* Standalone Qt Widgets regression; no OBS startup, devices or user profile.
 * Build with StudioMonitoringDevices.cpp and pkg-config Qt6Widgets. */
#include <utility/StudioMonitoringDevices.hpp>

#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardItemModel>
#include <cstdio>

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	QJsonArray checks;
	bool passed = true;
	auto check = [&](const char *name, bool ok) {
		checks.append(QJsonObject{{"name", QString::fromUtf8(name)}, {"passed", ok}});
		passed &= ok;
	};
	const QString defaultName = QString::fromUtf8("Padrão do sistema");
	const QString missingLabel = QString::fromUtf8("Indisponível: %1");
	const std::vector<StudioMonitoringDevice> devices{{"Headset A", "endpoint-a"}, {"Headset B", "endpoint-b"}};
	QComboBox combo;
	int activations = 0;
	QObject::connect(&combo, &QComboBox::activated, [&]() { ++activations; });
	auto populate = [&](const std::vector<StudioMonitoringDevice> &items, const QString &id, const QString &name) {
		PopulateStudioMonitoringDevices(&combo, items, id, name, defaultName, missingLabel);
	};
	auto enabled = [&](int index) {
		return combo.model()->flags(combo.model()->index(index, 0)).testFlag(Qt::ItemIsEnabled);
	};
	populate(devices, "endpoint-a", "Old name");
	check("available endpoint selected by ID", combo.currentData() == "endpoint-a");
	check("available label uses canonical inventory name", combo.currentText() == "Headset A");
	check("available endpoint enabled", enabled(combo.currentIndex()));
	check("default remains first", combo.itemData(0) == "default" && combo.itemText(0) == defaultName);
	auto selected = ResolveStudioMonitoringDevice(devices, "endpoint-a", defaultName);
	check("selection resolves canonical ID and name", selected && selected->id == "endpoint-a" && selected->name == "Headset A");
	const std::vector<StudioMonitoringDevice> unplugged{{"Headset B", "endpoint-b"}};
	check("unplug between popup inventory and activation is rejected",
	      !ResolveStudioMonitoringDevice(unplugged, "endpoint-a", defaultName));
	populate(unplugged, "endpoint-a", "Headset A");
	check("missing endpoint ID preserved", combo.currentData() == "endpoint-a");
	check("missing endpoint displayed explicitly", combo.currentText() == missingLabel.arg("Headset A"));
	check("missing endpoint cannot be selected", !enabled(combo.currentIndex()));
	check("missing endpoint does not become default", combo.currentIndex() != 0);
	check("decorated presentation cannot resolve as endpoint ID",
	      !ResolveStudioMonitoringDevice(devices, combo.currentText(), defaultName));
	populate(devices, "endpoint-a", "Headset A");
	check("returning endpoint becomes enabled", combo.currentData() == "endpoint-a" && enabled(combo.currentIndex()));
	check("returning endpoint loses missing decoration", combo.currentText() == "Headset A");
	check("returning endpoint can be selected", bool(ResolveStudioMonitoringDevice(devices, "endpoint-a", defaultName)));
	const std::vector<StudioMonitoringDevice> renamed{{"Renamed headset", "endpoint-a"}};
	populate(renamed, "endpoint-a", "Headset A");
	selected = ResolveStudioMonitoringDevice(renamed, "endpoint-a", defaultName);
	check("renamed endpoint keeps stable ID", combo.currentData() == "endpoint-a");
	check("renamed endpoint stores canonical new name", selected && selected->name == "Renamed headset");
	populate({}, "default", "Default");
	selected = ResolveStudioMonitoringDevice({}, "default", defaultName);
	check("default stays selectable without explicit endpoints", enabled(0) && combo.currentData() == "default");
	check("translated default label is separated from stable ID", selected && selected->id == "default" && selected->name == defaultName);
	check("empty enumeration rejects explicit endpoint", !ResolveStudioMonitoringDevice({}, "endpoint-a", defaultName));
	check("empty ID rejected", !ResolveStudioMonitoringDevice(devices, {}, defaultName));
	check("empty default name rejected", !ResolveStudioMonitoringDevice({}, "default", {}));
	populate({}, "endpoint-a", {});
	check("missing canonical name falls back to ID for display", combo.currentText() == missingLabel.arg("endpoint-a"));
	populate(devices, {}, {});
	check("unset current ID displays default", combo.currentData() == "default");
	const std::vector<StudioMonitoringDevice> noisy{{"Headset A", "endpoint-a"}, {"Duplicate", "endpoint-a"},
						    {"", "empty-name"}, {"Empty ID", ""}, {"Wrong default", "default"}};
	populate(noisy, "endpoint-a", "Headset A");
	check("duplicates and invalid entries omitted", combo.count() == 2);
	check("enumerated default cannot override translated logical default", combo.itemText(0) == defaultName);
	check("empty canonical endpoint name rejected", !ResolveStudioMonitoringDevice(noisy, "empty-name", defaultName));
	check("enumerated default cannot bypass invalid logical default name",
	      !ResolveStudioMonitoringDevice(noisy, "default", {}));
	check("population never emits activated", activations == 0);
	check("input inventory remains unchanged", devices.size() == 2 && devices[0].name == "Headset A");
	QJsonObject result{{"passed", passed}, {"checks", checks}, {"check_count", checks.size()},
			   {"scope", "Standalone native Qt Widgets policy and presentation, synthetic inventory; no OBS, physical hotplug, media, profile I/O or Windows execution."}};
	const QByteArray json = QJsonDocument(result).toJson(QJsonDocument::Indented);
	if (argc > 1) {
		QFile file(QString::fromLocal8Bit(argv[1]));
		if (!file.open(QIODevice::WriteOnly) || file.write(json) != json.size()) {
			return 2;
		}
	}
	std::fwrite(json.constData(), 1, size_t(json.size()), stdout);
	return passed ? 0 : 1;
}
