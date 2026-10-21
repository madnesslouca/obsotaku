#pragma once
#include <QString>
#include <QHash>
#include <util/config-file.h>
struct ChatTestApp {
	config_t *config = nullptr;
	QHash<QString, QString> translations;
	config_t *GetUserConfig() const { return config; }
};
ChatTestApp *App();
inline QString QTStr(const char *key)
{
	return App()->translations.value(QString::fromUtf8(key), QString::fromUtf8(key));
}
