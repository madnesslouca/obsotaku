#include "StudioVst3ScanProcess.hpp"

#include <QElapsedTimer>
#include <QProcess>
#include <algorithm>

namespace {
constexpr qsizetype DiagnosticLimit = 64 * 1024;
constexpr qsizetype ChunkLimit = 16 * 1024;

void RetainTail(QByteArray &tail, const QByteArray &chunk, qsizetype limit)
{
	if (chunk.size() >= limit) {
		tail = chunk.last(limit);
		return;
	}
	const qsizetype discard = tail.size() + chunk.size() - limit;
	if (discard > 0)
		tail.remove(0, discard);
	tail.append(chunk);
}
} // namespace

StudioVst3ScanResult RunStudioVst3ScanProcess(const QString &program, const QStringList &arguments,
					   const std::atomic<bool> &cancelled, qint64 timeoutMs)
{
	StudioVst3ScanResult result;
	if (cancelled.load(std::memory_order_acquire)) {
		result.status = StudioVst3ScanStatus::Cancelled;
		return result;
	}
	QProcess process;
	QElapsedTimer elapsed;
	elapsed.start();
	process.start(program, arguments);
	if (!process.waitForStarted(int(std::clamp<qint64>(timeoutMs, 1, 1000)))) {
		result.error = process.errorString();
		return result;
	}

	QByteArray protocolTail;
	bool complete = false, empty = false;
	auto drain = [&]() {
		for (auto channel : {QProcess::StandardOutput, QProcess::StandardError}) {
			process.setReadChannel(channel);
			/* Bound one turn so a noisy child cannot starve cancel/deadline. */
			for (int i = 0; i < 16 && process.bytesAvailable() > 0; ++i) {
				const QByteArray chunk = process.read(ChunkLimit);
				if (chunk.isEmpty())
					break;
				if (channel == QProcess::StandardOutput) {
					result.stdoutBytes += quint64(chunk.size());
					const QByteArray text = protocolTail + chunk;
					complete |= text.contains("VST3_SCAN_DONE\n") || text.contains("VST3_SCAN_DONE\r\n");
					empty |= text.contains("NO_VST3\n") || text.contains("NO_VST3\r\n");
					RetainTail(protocolTail, chunk, 32);
				} else {
					result.stderrBytes += quint64(chunk.size());
				}
				RetainTail(result.diagnostics, chunk, DiagnosticLimit);
			}
		}
	};

	for (;;) {
		drain();
		if (cancelled.load(std::memory_order_acquire) || elapsed.elapsed() >= timeoutMs) {
			result.status = cancelled.load(std::memory_order_acquire) ? StudioVst3ScanStatus::Cancelled
									       : StudioVst3ScanStatus::TimedOut;
			result.error = result.status == StudioVst3ScanStatus::TimedOut
				? QStringLiteral("Scanner exceeded its %1ms deadline").arg(timeoutMs)
				: QStringLiteral("Scanner cancelled");
			process.kill(); // Only the helper owned by this QProcess.
			process.waitForFinished(1000);
			return result;
		}
		if (process.state() == QProcess::NotRunning) {
			/* Drain buffered tail after exit, still subject to the deadline. */
			process.setReadChannel(QProcess::StandardOutput);
			const bool outputPending = process.bytesAvailable() > 0;
			process.setReadChannel(QProcess::StandardError);
			if (outputPending || process.bytesAvailable() > 0)
				continue;
			break;
		}
		process.waitForReadyRead(25);
	}
	result.exitCode = process.exitCode();
	if (process.exitStatus() != QProcess::NormalExit || result.exitCode != 0) {
		result.error = QStringLiteral("Scanner exited unsuccessfully (%1)").arg(result.exitCode);
	} else if (complete) {
		result.status = StudioVst3ScanStatus::Complete;
	} else if (empty) {
		result.status = StudioVst3ScanStatus::Empty;
	} else {
		result.error = QStringLiteral("Scanner exited without a completion marker");
	}
	return result;
}
