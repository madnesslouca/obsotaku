#pragma once

#include <QDockWidget>
#include <QMainWindow>
#include <algorithm>

/* Keep the two previews visible together instead of replacing the inspector's
 * selected tab. Floating previews and an already visible custom layout stay put. */
inline void SetStudioPortraitPreview(QMainWindow *window, bool enabled)
{
	auto *dock = window->findChild<QDockWidget *>("verticalPreviewDock");
	if (!dock) {
		return;
	}
	if (!enabled) {
		dock->hide();
		return;
	}
	if (!dock->isFloating() && (!dock->isVisible() || !window->tabifiedDockWidgets(dock).empty())) {
		window->removeDockWidget(dock);
		window->addDockWidget(Qt::RightDockWidgetArea, dock, Qt::Horizontal);
	}
	dock->show();
	dock->raise();
	if (!dock->isFloating()) {
		window->resizeDocks({dock}, {std::clamp(window->width() / 4, 300, 440)}, Qt::Horizontal);
	}
}
