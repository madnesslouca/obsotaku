#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <atomic>

enum class StudioVst3ScanStatus { Complete, Empty, Failed, TimedOut, Cancelled };

struct StudioVst3ScanResult {
	StudioVst3ScanStatus status = StudioVst3ScanStatus::Failed;
	QByteArray diagnostics;
	QString error;
	quint64 stdoutBytes = 0, stderrBytes = 0;
	int exitCode = -1;
};

/* Runs only on the scanner worker. Both pipes are drained; retained diagnostics
 * are capped, not the child's or QProcess' entire transient memory usage. */
StudioVst3ScanResult RunStudioVst3ScanProcess(const QString &program, const QStringList &arguments,
					   const std::atomic<bool> &cancelled, qint64 timeoutMs = 120000);
