// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#include "time-client.hpp"

#include <synqframe/capture-time-stamper.hpp>
#include <synqframe/ntp-sample.hpp>

#include <obs-module.h>

#include <QHostAddress>
#include <QHostInfo>
#include <QUdpSocket>
#include <QUrl>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace {
using synqframe::NtpSample;

constexpr int SamplesPerRound = 6;
constexpr auto RoundInterval = std::chrono::seconds(32);
constexpr auto RetryInterval = std::chrono::seconds(5);
// Keep the last good offset through a short outage; a PC clock drifts well under a
// millisecond a minute. After this long without an answer, stamp unsynced again.
constexpr auto StaleAfter = std::chrono::minutes(10);

std::mutex mutex;
std::condition_variable wake;
std::thread worker;
QString current;
bool stopping = false;

int64_t utcNowNs()
{
  // The stamper maps frames to this same clock (capture-time-stamper.cpp).
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
    .count();
}

// Sleeps, returning early (true) when asked to stop.
bool pause(std::chrono::milliseconds duration)
{
  std::unique_lock lock(mutex);
  return wake.wait_for(lock, duration, [] { return stopping; });
}

std::optional<NtpSample> queryRound(QUdpSocket &socket, const QHostAddress &address, quint16 port)
{
  std::vector<NtpSample> samples;
  for (int i = 0; i < SamplesPerRound; ++i) {
    const int64_t sent = utcNowNs();
    const auto request = synqframe::ntpRequest(sent);
    socket.writeDatagram(reinterpret_cast<const char *>(request.data()), request.size(), address, port);
    // Short waits so a stop never blocks for long.
    for (int wait = 0; wait < 4 && !socket.hasPendingDatagrams(); ++wait) socket.waitForReadyRead(250);
    while (socket.hasPendingDatagrams()) {
      QByteArray reply(static_cast<int>(socket.pendingDatagramSize()), Qt::Uninitialized);
      socket.readDatagram(reply.data(), reply.size());
      const int64_t received = utcNowNs();
      const auto bytes = std::span(reinterpret_cast<const uint8_t *>(reply.constData()), static_cast<size_t>(reply.size()));
      if (auto sample = synqframe::readNtpReply(bytes, sent, received)) samples.push_back(*sample);
    }
    if (pause(std::chrono::milliseconds(150))) break;
  }
  return synqframe::bestNtpSample(samples);
}

void run(QString url)
{
  const QUrl parsed(url);
  const QString host = parsed.host();
  const quint16 port = static_cast<quint16>(parsed.port(123));
  QUdpSocket socket;
  socket.bind();
  std::optional<std::chrono::steady_clock::time_point> lastGood;
  std::optional<NtpSample> reported;
  bool warned = false;

  while (true) {
    QHostAddress address;
    for (const QHostAddress &candidate : QHostInfo::fromName(host).addresses())
      if (candidate.protocol() == QAbstractSocket::IPv4Protocol) {
        address = candidate;
        break;
      }
    const auto best = address.isNull() ? std::nullopt : queryRound(socket, address, port);
    {
      std::lock_guard lock(mutex);
      if (stopping) return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (best) {
      synqframe::setTimeServerSync(synqframe::TimeServerSync{
        best->offsetNs, static_cast<uint32_t>(std::min<int64_t>(best->uncertaintyNs / 1000, 0xfffffffe))});
      // Log the first sync and any real change, not every round.
      if (!reported || std::llabs(best->offsetNs - reported->offsetNs) > 5'000'000) {
        blog(LOG_INFO, "[Synqframe] Time server %s: this clock is %+.1f ms off (+-%.1f ms, stratum %d); stamps synced",
             host.toUtf8().constData(), best->offsetNs / 1e6, best->uncertaintyNs / 1e6, best->stratum);
        reported = best;
      }
      lastGood = now;
      warned = false;
    } else if (lastGood && now - *lastGood > StaleAfter) {
      synqframe::setTimeServerSync(std::nullopt);
      blog(LOG_WARNING, "[Synqframe] Time server %s has not answered for 10 minutes; stamps are unsynced",
           host.toUtf8().constData());
      lastGood.reset();
      reported.reset();
    } else if (!lastGood && !warned) {
      // A just-restarted server answers "not synchronized" for a few seconds: not an error.
      blog(LOG_INFO, "[Synqframe] Time server %s gave no usable answer yet; stamps stay unsynced until it does",
           host.toUtf8().constData());
      warned = true;
    }
    if (pause(lastGood ? RoundInterval : RetryInterval)) return;
  }
}

void stopLocked(std::unique_lock<std::mutex> &lock)
{
  if (!worker.joinable()) return;
  stopping = true;
  wake.notify_all();
  std::thread finishing = std::move(worker);
  lock.unlock();
  finishing.join();
  lock.lock();
  stopping = false;
}
} // namespace

void setContributionTimeServer(const QString &url)
{
  std::unique_lock lock(mutex);
  if (url == current && (url.isEmpty() || worker.joinable())) return;
  stopLocked(lock);
  current = url;
  if (url.isEmpty() || QUrl(url).scheme() != QStringLiteral("ntp") || QUrl(url).host().isEmpty()) {
    synqframe::setTimeServerSync(std::nullopt);
    if (!url.isEmpty()) blog(LOG_WARNING, "[Synqframe] Ignoring time server with an unsupported address");
    return;
  }
  worker = std::thread(run, url);
}
