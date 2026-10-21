#pragma once
#include <QString>
#include <util/bmem.h>
extern QString clipLibraryConfigRoot;
inline QString QTStr(const char *key)
{
	return QString::fromUtf8(key) == QStringLiteral("Studio.ClipPair") ? QStringLiteral("Clip pair: %1")
									   : QString::fromUtf8(key);
}
inline char *GetAppConfigPathPtr(const char *path)
{
	return bstrdup((clipLibraryConfigRoot + QLatin1Char('/') + QString::fromUtf8(path)).toUtf8().constData());
}
