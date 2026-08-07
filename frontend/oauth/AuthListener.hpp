#pragma once

#include <QObject>

class QTcpServer;

class AuthListener : public QObject {
	Q_OBJECT

	QTcpServer *server;
	QString state;

signals:
	void ok(const QString &code);
	/* reason carries what the platform sent back when it explained itself
	 * (error / error_description). Empty when the callback said nothing. */
	void fail(const QString &reason = QString());

protected:
	void NewConnection();

public:
	explicit AuthListener(QObject *parent = 0, quint16 preferredPort = 0);
	quint16 GetPort();
	bool IsListening() const;
	void SetState(QString state);
};
