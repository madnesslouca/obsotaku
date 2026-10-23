#pragma once
#include <QString>
inline QString QTStr(const char *key) { return QString::fromUtf8(key); }
