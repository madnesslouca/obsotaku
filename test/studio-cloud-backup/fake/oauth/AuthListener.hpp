#pragma once
#include <QObject>
class AuthListener : public QObject {
 Q_OBJECT
public:
 explicit AuthListener(QObject *parent=nullptr, quint16=0):QObject(parent){}
 quint16 GetPort(){return 12345;}
 bool IsListening()const{return true;}
 void SetState(QString){}
signals:
 void ok(const QString &code);
 void fail(const QString &reason={});
};
