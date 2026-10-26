// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#pragma once

// The plugin's HTTPS, the way OBS does its own (frontend/utility/RemoteTextThread.cpp
// GetRemoteFile): libcurl, which OBS ships with Windows' own TLS. Not QtNetwork: stock OBS ships
// no Qt TLS backend, so a QNetworkAccessManager can't reach an https URL in a plain OBS install
// (2026-10-08). The calls are the node's (synqframe/http.hpp).

#include "client-settings.hpp"
#include <synqframe/http.hpp>

#include <QByteArray>
#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QThread>
#include <QTimer>

#include <chrono>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <vector>

struct HttpCall {
  QByteArray method = "GET"; // GET, POST, PUT, or another sent as is
  QString url;
  QString token;                  // the bearer; empty: none
  std::optional<QByteArray> body; // sent as JSON when given
  int timeoutMilliseconds = 10000;
};

struct HttpReply {
  bool completed = false; // false: the time ran out
  int status = 0;         // the HTTP status; 0 without a response
  QByteArray body;
  QString errorString; // status 0: why
  bool ok() const { return status >= 200 && status < 300; }
  QJsonObject json() const { return QJsonDocument::fromJson(body).object(); }
};

// Blocks the calling thread until the answer or the timeout. A call that failed before any
// answer is tried once more (common::httpRequest).
inline HttpReply performHttp(const HttpCall &call)
{
  common::HttpOptions options;
  options.token = call.token.toStdString();
  options.timeoutMs = call.timeoutMilliseconds;
  options.remote = true;
  const std::string body = call.body ? call.body->toStdString() : std::string();
  const common::HttpResult result =
    common::httpRequest(call.method.toStdString(), call.url.toStdString(), call.body ? &body : nullptr, options);
  HttpReply reply;
  reply.status = static_cast<int>(result.status);
  reply.body = QByteArray::fromStdString(result.body);
  reply.errorString = QString::fromStdString(result.error);
  reply.completed = !result.timedOut;
  return reply;
}

// OBS callbacks that require a synchronous answer must never wait forever, and the UI keeps
// painting meanwhile: on the UI thread the call runs on a thread of its own while a local event
// loop waits for it, as the QtNetwork version did. User input is held back while it waits: a
// click landing inside the wait (Stop Streaming during an output's start, a filter removed
// during its save) re-enters OBS in the middle of the callback that is waiting, which has
// crashed OBS before (bundle-filter.cpp, 2026-10-09).
inline HttpReply waitForHttp(const HttpCall &call)
{
  QCoreApplication *app = QCoreApplication::instance();
  if (!app || QThread::currentThread() != app->thread())
    return performHttp(call);
  std::future<HttpReply> answer = std::async(std::launch::async, performHttp, call);
  const auto ready = [&answer] { return answer.wait_for(std::chrono::seconds(0)) == std::future_status::ready; };
  if (!ready()) {
    QEventLoop wait;
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &wait, [&] {
      if (ready())
        wait.quit();
    });
    poll.start(10);
    wait.exec(QEventLoop::ExcludeUserInputEvents);
  }
  return answer.get();
}

// Calls in flight from asyncHttp, waited for when the plugin unloads (waitForHttpCalls), so no
// thread outlives its code.
inline std::mutex httpCallsMutex;
inline std::vector<std::future<void>> httpCalls;

// Runs `call` on a thread of its own and hands the answer to `done` on `context`'s thread,
// unless `context` is gone by then.
inline void asyncHttp(QObject *context, HttpCall call, std::function<void(const HttpReply &)> done)
{
  QPointer<QObject> target(context);
  std::lock_guard lock(httpCallsMutex);
  std::erase_if(httpCalls, [](const std::future<void> &call) {
    return call.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
  });
  httpCalls.push_back(std::async(std::launch::async, [call = std::move(call), target, done = std::move(done)] {
    const HttpReply reply = performHttp(call);
    if (target)
      QMetaObject::invokeMethod(target, [reply, done] { done(reply); }, Qt::QueuedConnection);
  }));
}

inline void waitForHttpCalls()
{
  std::lock_guard lock(httpCallsMutex);
  for (std::future<void> &call : httpCalls)
    call.wait();
  httpCalls.clear();
}

// A call to the Synqframe service at `path` as the signed-in account: its access token as the
// bearer (never in the URL), bounded like every call above.
inline HttpCall synqframeCall(const ClientSettings &settings, const QByteArray &method, const QString &path,
            std::optional<QJsonObject> payload = std::nullopt, int timeoutMilliseconds = 10000)
{
  HttpCall call;
  call.method = method;
  call.url = settings.serviceUrl + path;
  call.token = settings.accessToken;
  if (payload)
    call.body = QJsonDocument(*payload).toJson(QJsonDocument::Compact);
  call.timeoutMilliseconds = timeoutMilliseconds;
  return call;
}

// What a blocking call to the service came back with.
struct SynqframeReply {
  bool completed = false; // false: it timed out
  int status = 0;         // the HTTP status; 0 without a response
  QJsonObject body;       // the response as a JSON object, empty when it is not one
  QString errorString;    // status 0: why
  bool ok() const { return completed && status >= 200 && status < 300; }
  bool networkFailed() const { return status == 0; }
};

// Sends `method` ("GET", "PUT" or "POST", the latter two with `payload` as a JSON body) to the
// service as the signed-in account, and waits at most timeoutMilliseconds for the answer.
inline SynqframeReply callSynqframe(const ClientSettings &settings, const QByteArray &method, const QString &path,
            const QJsonObject &payload = {}, int timeoutMilliseconds = 10000)
{
  const HttpReply reply = waitForHttp(synqframeCall(
    settings, method, path, method == "GET" ? std::nullopt : std::optional<QJsonObject>(payload), timeoutMilliseconds));
  SynqframeReply result;
  result.completed = reply.completed;
  result.status = reply.status;
  result.body = reply.json();
  result.errorString = reply.errorString;
  return result;
}
