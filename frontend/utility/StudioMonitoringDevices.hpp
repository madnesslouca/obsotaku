#pragma once

#include <QString>
#include <optional>
#include <vector>

class QComboBox;

struct StudioMonitoringDevice {
	QString name;
	QString id;
};

/* Enumeration is supplied by the caller so the same policy can be exercised
 * with disappearing endpoints without touching the user's audio session. */
std::optional<StudioMonitoringDevice> ResolveStudioMonitoringDevice(
	const std::vector<StudioMonitoringDevice> &devices, const QString &id, const QString &defaultName);

void PopulateStudioMonitoringDevices(QComboBox *combo, const std::vector<StudioMonitoringDevice> &devices,
				    const QString &currentId, const QString &currentName, const QString &defaultName,
				    const QString &missingLabel);
