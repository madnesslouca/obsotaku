#include "StudioMonitoringDevices.hpp"

#include <QComboBox>
#include <QSignalBlocker>
#include <QStandardItemModel>

std::optional<StudioMonitoringDevice> ResolveStudioMonitoringDevice(
	const std::vector<StudioMonitoringDevice> &devices, const QString &id, const QString &defaultName)
{
	if (id == QStringLiteral("default")) {
		return defaultName.isEmpty() ? std::nullopt
					     : std::optional<StudioMonitoringDevice>{{defaultName, id}};
	}
	for (const auto &device : devices) {
		if (!id.isEmpty() && device.id == id && !device.name.isEmpty()) {
			return device;
		}
	}
	return std::nullopt;
}

void PopulateStudioMonitoringDevices(QComboBox *combo, const std::vector<StudioMonitoringDevice> &devices,
				    const QString &currentId, const QString &currentName, const QString &defaultName,
				    const QString &missingLabel)
{
	const QSignalBlocker blocker(combo);
	combo->clear();
	combo->addItem(defaultName, QStringLiteral("default"));
	for (const auto &device : devices) {
		if (!device.id.isEmpty() && !device.name.isEmpty() && combo->findData(device.id) < 0) {
			combo->addItem(device.name, device.id);
		}
	}
	const QString id = currentId.isEmpty() ? QStringLiteral("default") : currentId;
	int index = combo->findData(id);
	if (index < 0) {
		combo->addItem(missingLabel.arg(currentName.isEmpty() ? id : currentName), id);
		index = combo->count() - 1;
		if (auto *model = qobject_cast<QStandardItemModel *>(combo->model())) {
			model->item(index)->setEnabled(false);
		}
	}
	combo->setCurrentIndex(index);
}
