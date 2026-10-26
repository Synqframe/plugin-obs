// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#include "go-live-dock.hpp"
#include "client-settings.hpp"
#include "obs-browser-panel.hpp"
#include "obs-connect.hpp"
#include "client-config.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QDesktopServices>
#include <QDockWidget>
#include <QMainWindow>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace {
// The vendored panel interface is version 3. A shorter vtable from an older
// obs-browser must never be called through it.
constexpr int RequiredPanelVersion = 3;
constexpr int BrowserPollMilliseconds = 500;
constexpr int BrowserPollAttempts = 40;
constexpr int MinimumPanelWidth = 240;
constexpr int MinimumPanelHeight = 160;
// A floating dock opens at least this large, like OBS's own browser docks.
constexpr int FloatingWidth = 460;
constexpr int FloatingHeight = 600;
// Its own cookie jar under obs-browser's plugin config, separate from the
// panels OBS manages, so signing in here never touches other sites' sessions.
constexpr const char *DockProfilePath = "synqframe_dock_cookies";

QPointer<QWidget> panel;
QPointer<QDockWidget> wrapper;
QPointer<QLabel> notice;
QPointer<QPushButton> openInBrowser;
QCef *cef = nullptr;
QCefCookieManager *cookies = nullptr;
QPointer<QCefWidget> browser;
QString loadedUrl;
int pollAttempts = 0;
bool polling = false;

QString dockUrl()
{
  const QUrl base(loadClientSettings().serviceUrl.trimmed());
  if (!base.isValid() || (base.scheme() != QStringLiteral("https") && base.scheme() != QStringLiteral("http")) ||
      base.host().isEmpty())
    return {};
  QString text = base.toString(QUrl::RemoveQuery | QUrl::RemoveFragment);
  while (text.endsWith(QLatin1Char('/')))
    text.chop(1);
  text += QStringLiteral("/dock");
  // Tells the page which OBS it is running in. A random, non-secret id; never a
  // token or grant.
  if (clientConfigChannelEnabled()) text += QStringLiteral("?clientId=") + ensureClientId();
  return text;
}

// Which window edge OBS has docked the panel to, for the page's tab rail:
// left, right, top or bottom. Empty when it is unknown or the dock is floating,
// and the page then picks its own edge.
QString dockArea()
{
  if (!wrapper || wrapper->isFloating()) return {};
  auto *mainWindow = qobject_cast<QMainWindow *>(static_cast<QWidget *>(obs_frontend_get_main_window()));
  if (!mainWindow) return {};
  switch (mainWindow->dockWidgetArea(wrapper)) {
  case Qt::LeftDockWidgetArea: return QStringLiteral("left");
  case Qt::RightDockWidgetArea: return QStringLiteral("right");
  case Qt::TopDockWidgetArea: return QStringLiteral("top");
  case Qt::BottomDockWidgetArea: return QStringLiteral("bottom");
  default: return {};
  }
}

// The URL to load. Only the first load carries the dock area: after that a move
// is pushed to the page as an event, so moving the dock never reloads it.
QString pageUrl()
{
  QString url = dockUrl();
  if (url.isEmpty()) return url;
  const QString area = dockArea();
  if (!area.isEmpty()) url += (url.contains(QLatin1Char('?')) ? QStringLiteral("&dock=") : QStringLiteral("?dock=")) + area;
  return url;
}

// Tells the loaded page where the dock is now: a `synqframe:dock` event on
// window, with detail.area set to left, right, top, bottom or floating.
void pushDockArea(const char *signal)
{
  const QString area = wrapper && wrapper->isFloating() ? QStringLiteral("floating") : dockArea();
  // A move fires topLevelChanged and dockLocationChanged together with the same
  // area, and a drag passes through "floating": tell the page once per change.
  static QString lastArea;
  if (area == lastArea) return;
  lastArea = area;
  blog(LOG_INFO, "[Synqframe] Dock moved (%s): %s", signal,
       area.isEmpty() ? "unknown" : area.toUtf8().constData());
  if (!browser) return;
  browser->executeJavaScript(
    QStringLiteral("window.dispatchEvent(new CustomEvent('synqframe:dock',{detail:{area:'%1'}}));")
      .arg(area.isEmpty() ? QStringLiteral("floating") : area).toStdString());
}

void showNotice(const QString &text, bool offerBrowser)
{
  if (notice) {
    notice->setText(text);
    notice->show();
  }
  if (openInBrowser)
    openInBrowser->setVisible(offerBrowser);
}

// Used when obs-browser is absent (some distributions omit it) or too old.
void showFallback()
{
  showNotice(QObject::tr("This OBS build has no browser dock support. Open Go Live in your browser instead."),
             !dockUrl().isEmpty());
}

void pollBrowser();

// One sign-in in the dock signs this OBS in too: while the plugin holds no
// token, the dock loads the plugin's own PKCE connect step (/connect/obs). The
// web page signs the user in if needed, then redirects to the plugin's loopback
// callback with a one-time code. Neither the connect URL nor the code is logged.
// Returns the URL to load first, or an empty string when nothing is needed.
QString startConnectInDock()
{
  if (!loadClientSettings().accessToken.isEmpty()) return {};
  QString connectUrl;
  const bool started = connectObs(
    [&connectUrl](const QUrl &url) { connectUrl = url.toString(QUrl::FullyEncoded); },
    [](bool connected) {
      blog(LOG_INFO, "[Synqframe] Sign-in from the dock %s", connected ? "completed" : "did not complete");
      if (browser) browser->setURL(pageUrl().toStdString());
    });
  if (started && !connectUrl.isEmpty())
    blog(LOG_INFO, "[Synqframe] The dock is signing this OBS in");
  return connectUrl;
}

void createBrowser(const QString &url)
{
  cookies = cef->create_cookie_manager(DockProfilePath, true);
  const QString connectUrl = startConnectInDock();
  browser = cef->create_widget(panel, (connectUrl.isEmpty() ? pageUrl() : connectUrl).toStdString(), cookies);
  if (!browser) {
    showFallback();
    return;
  }
  loadedUrl = url;
  if (notice) notice->hide();
  if (openInBrowser) openInBrowser->hide();
  // Drop the spacer that kept the notice at the top, so the page gets all the room.
  auto *box = static_cast<QVBoxLayout *>(panel->layout());
  for (int index = box->count() - 1; index >= 0; --index)
    if (box->itemAt(index)->spacerItem()) delete box->takeAt(index);
  box->addWidget(browser, 1);
}

void pollBrowser()
{
  if (!panel || browser) {
    polling = false;
    return;
  }
  if (cef->initialized()) {
    polling = false;
    const QString url = dockUrl();
    if (!url.isEmpty()) createBrowser(url);
    return;
  }
  if (++pollAttempts > BrowserPollAttempts) {
    polling = false;
    blog(LOG_WARNING, "[Synqframe] The OBS browser did not finish initializing; Go Live opens externally");
    showFallback();
    return;
  }
  QTimer::singleShot(BrowserPollMilliseconds, panel, pollBrowser);
}
}

void registerGoLiveDock()
{
  panel = new QWidget();
  // Without a minimum size OBS lets a floating dock, or one on a top or bottom
  // edge, shrink until only its title bar shows.
  panel->setMinimumSize(MinimumPanelWidth, MinimumPanelHeight);
  panel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  auto *layout = new QVBoxLayout(panel);
  layout->setContentsMargins(0, 0, 0, 0);
  notice = new QLabel(panel);
  notice->setWordWrap(true);
  notice->setMargin(8);
  notice->hide();
  openInBrowser = new QPushButton(QObject::tr("Open Go Live in browser"), panel);
  openInBrowser->hide();
  layout->addWidget(notice);
  layout->addWidget(openInBrowser, 0, Qt::AlignHCenter);
  layout->addStretch(0);
  QObject::connect(openInBrowser, &QPushButton::clicked, panel, [] {
    const QString url = dockUrl();
    if (!url.isEmpty()) QDesktopServices::openUrl(QUrl(url));
  });

  if (!obs_frontend_add_dock_by_id("synqframe-go-live", "Synqframe Go Live", panel)) {
    blog(LOG_ERROR, "Failed to register the Synqframe Go Live dock");
    return;
  }
  // The service URL can change while OBS runs; follow it whenever the creator
  // brings the dock back.
  wrapper = qobject_cast<QDockWidget *>(panel->parentWidget());
  if (wrapper) {
    QObject::connect(wrapper, &QDockWidget::visibilityChanged, panel, [](bool visible) {
      if (visible) populateGoLiveDock();
    });
    // Moving the dock between window edges, or floating it, is pushed to the
    // page rather than reloading it.
    QObject::connect(wrapper, &QDockWidget::dockLocationChanged, panel, [](Qt::DockWidgetArea) { pushDockArea("dockLocationChanged"); });
    QObject::connect(wrapper, &QDockWidget::topLevelChanged, panel, [](bool floating) {
      // The size a dock had while docked on a thin edge is a poor size for a window.
      if (floating && wrapper && (wrapper->width() < FloatingWidth || wrapper->height() < FloatingHeight))
        wrapper->resize(qMax(wrapper->width(), FloatingWidth), qMax(wrapper->height(), FloatingHeight));
      pushDockArea("topLevelChanged");
    });
  }
}

void populateGoLiveDock()
{
  if (!panel) return;
  const QString url = dockUrl();
  if (url.isEmpty()) {
    showNotice(QObject::tr("Set the Synqframe service URL to open Go Live."), false);
    return;
  }
  if (browser) {
    if (url != loadedUrl) {
      loadedUrl = url;
      browser->setURL(url.toStdString());
    }
    // A token lost since the dock loaded (for example a failed refresh) restarts
    // the connect step; a sign-in already waiting for its callback is left alone.
    if (const QString connectUrl = startConnectInDock(); !connectUrl.isEmpty())
      browser->setURL(connectUrl.toStdString());
    return;
  }
  if (!cef) {
    if (obs_browser_qcef_version() < RequiredPanelVersion) {
      showFallback();
      return;
    }
    cef = obs_browser_init_panel();
    if (!cef) {
      showFallback();
      return;
    }
  }
  // Visibility changes fire while OBS restores its layout; one wait is enough.
  if (polling) return;
  polling = true;
  pollAttempts = 0;
  cef->init_browser();
  pollBrowser();
}

void unloadGoLiveDock()
{
  if (browser) browser->closeBrowser();
  if (cookies) {
    cookies->FlushStore();
    delete cookies;
    cookies = nullptr;
  }
  delete cef;
  cef = nullptr;
}
