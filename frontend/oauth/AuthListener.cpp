#include "AuthListener.hpp"

#include <OBSApp.hpp>

#include <qt-wrappers.hpp>

#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>

#include <memory>

#include "moc_AuthListener.cpp"

#define LOGO_URL "https://obsproject.com/assets/images/new_icon_small-r.png"

static const QString serverResponseHeader = QStringLiteral("HTTP/1.0 200 OK\r\n"
							   "Connection: close\r\n"
							   "Content-Type: text/html; charset=UTF-8\r\n"
							   "Server: OBS Studio\r\n"
							   "\r\n"
							   "<html><head><title>OBS Studio"
							   "</title></head>");

static const QString responseTemplate = "<center>"
					"<img src=\"" LOGO_URL
					"\" alt=\"OBS\" class=\"center\"  height=\"60\" width=\"60\">"
					"</center>"
					"<center><p style=\"font-family:verdana; font-size:13pt\">%1</p></center>";

AuthListener::AuthListener(QObject *parent, quint16 preferredPort) : QObject(parent)
{
	server = new QTcpServer(this);
	connect(server, &QTcpServer::newConnection, this, &AuthListener::NewConnection);
	if (!server->listen(QHostAddress::LocalHost, preferredPort)) {
		blog(LOG_DEBUG, "Server could not start");
		emit fail();
	} else {
		blog(LOG_DEBUG, "Server started at port %d", server->serverPort());
	}
}

quint16 AuthListener::GetPort()
{
	return server ? server->serverPort() : 0;
}

bool AuthListener::IsListening() const
{
	return server && server->isListening();
}

void AuthListener::SetState(QString state)
{
	this->state = state;
}

void AuthListener::NewConnection()
{
	QTcpSocket *socket = server->nextPendingConnection();
	if (socket) {
		connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
		auto buffer = std::make_shared<QByteArray>();
		connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer]() {
			buffer->append(socket->readAll());
			if (!buffer->contains("\r\n\r\n"))
				return;

			socket->write(QT_TO_UTF8(serverResponseHeader));
			QString redirect = QString::fromLatin1(*buffer);
			blog(LOG_DEBUG, "OAuth loopback redirect received");

			const QString requestLine = redirect.section(QStringLiteral("\r\n"), 0, 0);
			const QString requestTarget = requestLine.section(' ', 1, 1);
			const QUrlQuery query(QUrl::fromEncoded(requestTarget.toLatin1()));
			const QString receivedState = query.queryItemValue(QStringLiteral("state"), QUrl::FullyDecoded);
			QString code;

			/* Browsers also request favicons and prefetch the callback. Those
			 * carry no authorization result, so they must not end the flow. */
			const bool isAuthorizationResponse = query.hasQueryItem(QStringLiteral("state")) ||
							     query.hasQueryItem(QStringLiteral("code")) ||
							     query.hasQueryItem(QStringLiteral("error"));
			if (!isAuthorizationResponse) {
				blog(LOG_DEBUG, "ignoring unrelated request on the OAuth loopback listener");
				socket->write(QT_TO_UTF8(responseTemplate.arg(QString())));
				socket->flush();
				socket->close();
				return;
			}

			/* The platform explains a refusal here. Passing it on turns
			 * "authorization failed" into something actionable, such as
			 * a redirect URI that is not registered. */
			QString reason = query.queryItemValue(QStringLiteral("error_description"), QUrl::FullyDecoded);
			const QString errorCode = query.queryItemValue(QStringLiteral("error"), QUrl::FullyDecoded);
			if (reason.isEmpty())
				reason = errorCode;
			else if (!errorCode.isEmpty())
				reason = QStringLiteral("%1 (%2)").arg(reason, errorCode);

			if (!receivedState.isEmpty()) {
				if (state == receivedState) {
					code = query.queryItemValue(QStringLiteral("code"), QUrl::FullyDecoded);
					if (code.isEmpty() && reason.isEmpty())
						reason = QStringLiteral("the callback carried no authorization code");
				} else {
					blog(LOG_WARNING, "state mismatch while handling redirect");
					if (reason.isEmpty())
						reason = QStringLiteral("the callback state did not match");
				}
			} else if (reason.isEmpty()) {
				reason = QStringLiteral("the callback carried no state");
			}

			if (code.isEmpty()) {
				blog(LOG_WARNING, "OAuth callback refused: %s", QT_TO_UTF8(reason));
				auto data = responseTemplate.arg(QTStr("YouTube.Auth.NoCode"));
				socket->write(QT_TO_UTF8(data));
				server->close();
				emit fail(reason);
			} else {
				auto data = responseTemplate.arg(QTStr("YouTube.Auth.Ok"));
				socket->write(QT_TO_UTF8(data));
				server->close();
				emit ok(code);
			}
			socket->flush();
			socket->close();
		});
	} else {
		emit fail();
	}
}
