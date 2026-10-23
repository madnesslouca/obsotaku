#pragma once

#include <QString>

inline QString StudioCanvasCardStyleSheet()
{
	return QStringLiteral(
		"QPushButton#studioCanvasSingle, QPushButton#studioCanvasMulti {text-align: left; padding: 12px; border: 1px solid palette(mid); border-radius: 6px;}"
		"QPushButton#studioCanvasSingle:checked, QPushButton#studioCanvasMulti:checked {border: 2px solid palette(highlight); background: palette(highlight); color: palette(highlighted-text);}");
}
