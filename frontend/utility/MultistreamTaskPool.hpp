/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <QThreadPool>
#include <QCoreApplication>
#include <QThread>
#include <functional>
#include <atomic>
#include <memory>
#include <mutex>
#include <utility>

/* Workers perform blocking network calls. A credential transaction may await
 * a bounded, cancellable GUI decision only if StopToken/cancellation can settle
 * rejection and recovery without processing GUI events during shutdown.
 * The delivery context outlives accepted tasks. Capture it on GUI before dispatch;
 * access QPointer receivers only INSIDE the queued GUI closure. */
class MultistreamWorkerPool {
public:
	MultistreamWorkerPool()
	{
		pool.setMaxThreadCount(3);
		pool.setExpiryTimeout(1000);
		pool.setObjectName(QStringLiteral("MultistreamTaskPool"));
	}
	~MultistreamWorkerPool() { Shutdown(); }
	std::shared_ptr<std::atomic_bool> StopToken() const { return stopToken; }
	QObject *Dispatcher()
	{
		Q_ASSERT(QCoreApplication::instance() &&
			 QThread::currentThread() == QCoreApplication::instance()->thread());
		if (!dispatcher) {
			dispatcher = new QObject(QCoreApplication::instance());
			QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, dispatcher,
					 [this] { Shutdown(); });
		}
		return dispatcher;
	}
	template<class Task> bool start(Task &&task)
	{
		Dispatcher();
		std::lock_guard lock(gate);
		if (closing) {
			return false;
		}
		pool.start(std::forward<Task>(task));
		return true;
	}
	void SetBeforeShutdown(std::function<void()> hook)
	{
		Dispatcher();
		std::lock_guard lock(gate);
		beforeShutdown = std::move(hook);
	}
	void Shutdown()
	{
		Q_ASSERT(!QCoreApplication::instance() ||
			 QThread::currentThread() == QCoreApplication::instance()->thread());
		std::function<void()> prepare;
		{
			std::lock_guard lock(gate);
			closing = true;
			stopToken->store(true, std::memory_order_relaxed);
			prepare = beforeShutdown;
		}
		// Store cancellation/rollback must complete before OAuth workers waiting on
		// the same account mutex are drained. No nested event loop is used.
		if (prepare) {
			prepare();
		}
		pool.waitForDone();
	}

private:
	std::shared_ptr<std::atomic_bool> stopToken = std::make_shared<std::atomic_bool>(false);
	QThreadPool pool;
	QObject *dispatcher = nullptr;
	std::mutex gate;
	bool closing = false;
	std::function<void()> beforeShutdown;
};
inline MultistreamWorkerPool &MultistreamTaskPool()
{
	static MultistreamWorkerPool pool;
	return pool;
}
inline QObject *MultistreamGuiDispatcher()
{
	return MultistreamTaskPool().Dispatcher();
}
inline void ShutdownMultistreamTasks()
{
	MultistreamTaskPool().Shutdown();
}
