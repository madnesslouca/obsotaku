#pragma once
#include <QObject>
#include <QUrl>
#include "facts.hpp"
// Test-only registered handler absorbs browser URLs; no browser is launched.
class UrlSink : public QObject {
 Q_OBJECT
public slots:
 void Accept(const QUrl &){++browserCalls;}
};
