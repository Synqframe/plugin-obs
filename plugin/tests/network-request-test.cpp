// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

// Checks the plugin's calls to the service (network-request.hpp) against a fake HTTP server in
// this process: the method, path, bearer token and JSON body sent, and what is made of an
// answer, a refusal, a reply that is not JSON, and no reply at all, through libcurl as the plugin
// makes them; and the asynchronous form. No OBS.
#include "network-request.hpp"

#include "check.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTcpServer>
#include <QTcpSocket>

#include <memory>
#include <optional>

namespace {

struct Request {
  QByteArray method, path, authorization, contentType, body;
};

// Answers every request with `response`, a whole HTTP response, or never while it is empty;
// keeps the last request it read.
struct FakeService {
  QTcpServer server;
  QByteArray response;
  Request last;

  FakeService()
  {
    server.listen(QHostAddress::LocalHost);
    QObject::connect(&server, &QTcpServer::newConnection, [this] {
      QTcpSocket *socket = server.nextPendingConnection();
      auto received = std::make_shared<QByteArray>();
      QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, received] {
        received->append(socket->readAll());
        const qsizetype headersEnd = received->indexOf("\r\n\r\n");
        if (headersEnd < 0) return;
        Request request;
        qsizetype length = 0;
        const QList<QByteArray> lines = received->left(headersEnd).split('\n');
        const QList<QByteArray> requestLine = lines.first().trimmed().split(' ');
        request.method = requestLine.value(0);
        request.path = requestLine.value(1);
        for (const QByteArray &raw : lines.mid(1)) {
          const QByteArray line = raw.trimmed();
          const qsizetype colon = line.indexOf(':');
          const QByteArray name = line.left(colon).toLower(), value = line.mid(colon + 1).trimmed();
          if (name == "authorization") request.authorization = value;
          if (name == "content-type") request.contentType = value;
          if (name == "content-length") length = value.toLongLong();
        }
        if (received->size() < headersEnd + 4 + length) return;
        request.body = received->mid(headersEnd + 4, length);
        last = request;
        if (response.isEmpty()) return;
        socket->write(response);
        socket->disconnectFromHost();
      });
    });
  }

  ClientSettings settings() const
  {
    ClientSettings settings;
    settings.serviceUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
    settings.accessToken = QStringLiteral("tok-123");
    return settings;
  }
};

QByteArray http(int status, const QByteArray &body, const QByteArray &type = "application/json")
{
  return "HTTP/1.1 " + QByteArray::number(status) + " Status\r\nContent-Type: " + type +
         "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}

} // namespace

int main(int argc, char **argv)
{
  QCoreApplication app(argc, argv);
  FakeService service;
  const ClientSettings settings = service.settings();

  // A GET: the path under the service URL, the token as bearer, no body; the answer's JSON.
  service.response = http(200, R"({"sessionId":"s1"})");
  SynqframeReply reply = callSynqframe(settings, "GET", "/api/broadcast");
  CHECK(reply.completed && reply.ok() && reply.status == 200);
  CHECK(reply.body.value("sessionId").toString() == "s1");
  CHECK(service.last.method == "GET" && service.last.path == "/api/broadcast");
  CHECK(service.last.authorization == "Bearer tok-123");
  CHECK(service.last.contentType.isEmpty() && service.last.body.isEmpty());

  // PUT and POST send the payload as compact JSON.
  service.response = http(204, "");
  reply = callSynqframe(settings, "PUT", "/api/live", QJsonObject{{"live", false}});
  CHECK(reply.ok() && reply.status == 204 && reply.body.isEmpty());
  CHECK(service.last.method == "PUT" && service.last.contentType == "application/json");
  CHECK(service.last.body == R"({"live":false})");
  CHECK(service.last.authorization == "Bearer tok-123");
  service.response = http(200, "{}");
  reply = callSynqframe(settings, "POST", "/api/contributions/authorize", QJsonObject{{"dryRun", true}});
  CHECK(reply.ok() && service.last.method == "POST" && service.last.body == R"({"dryRun":true})");

  // A refusal keeps its status and the service's detail; it is no network failure.
  service.response = http(403, R"({"detail":"not accepting contributors"})");
  reply = callSynqframe(settings, "PUT", "/api/broadcast", QJsonObject{});
  CHECK(reply.completed && !reply.ok() && reply.status == 403);
  CHECK(reply.body.value("detail").toString() == "not accepting contributors");
  CHECK(!reply.networkFailed());

  // An answer that is not JSON reads as an empty body.
  service.response = http(502, "oops", "text/plain");
  reply = callSynqframe(settings, "GET", "/api/sessions/s1");
  CHECK(reply.completed && reply.status == 502 && reply.body.isEmpty());

  // No answer: given up within the timeout, with no status.
  service.response.clear();
  QElapsedTimer elapsed;
  elapsed.start();
  reply = callSynqframe(settings, "GET", "/api/slow", {}, 300);
  CHECK(!reply.ok() && reply.status == 0 && reply.networkFailed() && !reply.completed);
  CHECK(elapsed.elapsed() < 2000);

  // The asynchronous form: the same request, its answer handed back on this thread.
  service.response = http(200, R"({"scenes":[]})");
  QObject context;
  std::optional<HttpReply> answered;
  asyncHttp(&context, synqframeCall(settings, "GET", "/api/sessions/s1/layout?scope=resolved"),
            [&answered](const HttpReply &reply) { answered = reply; });
  QElapsedTimer waited;
  waited.start();
  while (!answered && waited.elapsed() < 5000) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  CHECK(answered && answered->ok() && answered->json().contains("scenes"));
  CHECK(service.last.path == "/api/sessions/s1/layout?scope=resolved" && service.last.authorization == "Bearer tok-123");
  waitForHttpCalls();

  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("network request: all checks passed\n");
  return 0;
}
