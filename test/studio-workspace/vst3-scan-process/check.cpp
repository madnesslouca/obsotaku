#include "StudioVst3ScanProcess.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <cstdio>
#include <thread>
#include <utility>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	const auto args = app.arguments();
	if (args.value(1) == "--child") {
#ifdef _WIN32
		_setmode(_fileno(stdout), _O_BINARY);
		_setmode(_fileno(stderr), _O_BINARY);
#endif
		const QString mode = args.value(2);
		if (mode == "flood") {
			QByteArray bytes(2 * 1024 * 1024, 'x');
			std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stderr);
			std::fflush(stderr);
			std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
		}
		if (mode == "hang") {
			QFile pid(args.value(3));
			if (pid.open(QIODevice::WriteOnly))
				pid.write(QByteArray::number(app.applicationPid()));
			pid.close();
			for (;;)
				QThread::msleep(100);
		}
		if (mode == "empty") {
			std::fputs("NO_VST3\r\n", stdout);
			return 0;
		}
		if (mode == "missing-marker")
			return 0;
		if (mode == "literal" && args.value(3) != QString::fromUtf8("áudio pasta [teste]"))
			return 8;
		if (mode == "split") {
			std::fputs("VST3_SCAN_", stdout);
			std::fflush(stdout);
			QThread::msleep(60);
			std::fputs("DONE\r\n", stdout);
		} else {
			std::fputs("VST3_SCAN_DONE\n", stdout);
		}
		return mode == "failed-marker" ? 7 : 0;
	}
	if (args.size() < 2)
		return 2;
	const QString root = QDir(args[1]).absolutePath();
	QDir().mkpath(root);
	QJsonArray checks, cases;
	bool passed = true;
	auto check = [&](bool ok, const char *name) {
		checks.append(QJsonObject{{"check", name}, {"passed", ok}});
		passed &= ok;
	};
	auto run = [&](const QString &mode, qint64 timeout = 3000, bool doCancel = false) {
		std::atomic<bool> cancelled{false};
		std::thread cancellation;
		if (doCancel)
			cancellation = std::thread([&] { QThread::msleep(100); cancelled.store(true); });
		QElapsedTimer clock;
		clock.start();
		const auto r = RunStudioVst3ScanProcess(app.applicationFilePath(),
			{"--child", mode, mode == "literal" ? QString::fromUtf8("áudio pasta [teste]") : root + "/" + mode + ".pid"},
			cancelled, timeout);
		const auto ms = clock.elapsed();
		if (cancellation.joinable())
			cancellation.join();
		cases.append(QJsonObject{{"mode", mode}, {"elapsed_ms", double(ms)}, {"status", int(r.status)},
			{"stdout_bytes", double(r.stdoutBytes)}, {"stderr_bytes", double(r.stderrBytes)},
			{"retained_bytes", int(r.diagnostics.size())}, {"exit", r.exitCode}, {"error", r.error}});
		check(r.diagnostics.size() <= 65536, "Retained diagnostic tail stays within64KiB");
		return std::make_pair(r, ms);
	};
	const auto flood = run("flood");
	check(flood.first.status == StudioVst3ScanStatus::Complete, "stderr-before-stdout flood cannot deadlock scan");
	check(flood.first.stderrBytes == 2 * 1024 * 1024, "Drain complete stderr flood");
	check(flood.first.stdoutBytes > 2 * 1024 * 1024, "Drain complete stdout flood plus protocol");
	check(flood.first.diagnostics.size() == 65536, "Only the bounded final tail is retained");
	check(flood.second < 3000, "Flood finishes within deadline");
	check(run("split").first.status == StudioVst3ScanStatus::Complete, "Recognize CRLF marker split across reads");
	check(run("empty").first.status == StudioVst3ScanStatus::Empty, "Accept legitimate empty catalog");
	check(run("literal").first.status == StudioVst3ScanStatus::Complete, "Unicode and spaced arguments arrive literally");
	const auto failed = run("failed-marker");
	check(failed.first.status == StudioVst3ScanStatus::Failed && failed.first.exitCode == 7,
	      "A completion marker cannot override failed process exit");
	check(run("missing-marker").first.status == StudioVst3ScanStatus::Failed, "Exit zero alone is not a completed scan");
	const auto timeout = run("hang", 200);
	check(timeout.first.status == StudioVst3ScanStatus::TimedOut, "Hung helper reaches deadline");
	check(timeout.second < 1500, "Deadline stops owned helper promptly");
	const auto cancelled = run("hang", 4000, true);
	check(cancelled.first.status == StudioVst3ScanStatus::Cancelled, "Module shutdown cancellation stops helper");
	check(cancelled.second < 1500, "Cancellation does not wait for full scan deadline");
	std::atomic<bool> no{false};
	const auto missing = RunStudioVst3ScanProcess(root + "/missing helper", {}, no, 400);
	check(missing.status == StudioVst3ScanStatus::Failed && !missing.error.isEmpty(), "Missing executable reports startup failure");
	std::atomic<bool> yes{true};
	check(RunStudioVst3ScanProcess(app.applicationFilePath(), {"--child", "hang"}, yes).status == StudioVst3ScanStatus::Cancelled,
	      "Precancelled scan never launches a helper");
	QFile report(root + "/result.json");
	if (!report.open(QIODevice::WriteOnly))
		return 3;
	report.write(QJsonDocument(QJsonObject{{"passed", passed}, {"checks", checks}, {"cases", cases},
		{"scope", "Real helper processes, pipe pressure, deadlines/cancel and retained logs; no VST3 DSP or full OBS runtime certification"}}).toJson());
	std::printf("%s: %lld checks\n", passed ? "PASS" : "FAIL", static_cast<long long>(checks.size()));
	return passed ? 0 : 1;
}
