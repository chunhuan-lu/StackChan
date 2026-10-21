/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_launcher.h"
#include <hal/hal.h>
#include <mooncake.h>
#include <mooncake_log.h>
#include <stackchan/stackchan.h>
#include <cstdint>
#include <sdkconfig.h>
#if CONFIG_SC_USB_LINK
#include <hal/sc/usb_link.h>  // [StackChan fork]
#endif

using namespace mooncake;

void AppLauncher::onLauncherCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");

    // 打开自己
    open();
}

void AppLauncher::onLauncherOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    LvglLockGuard lock;

    if (!_startup_checked && GetHAL().consumeAppSetupRequest()) {
        mclog::tagInfo(getAppInfo().name, "resume App setup after network/binding failure");
        _startup_worker = std::make_unique<setup_workers::WifiSetupWorker>();
    } else if (!_startup_checked && !GetHAL().isAppConfiged()) {
        mclog::tagInfo(getAppInfo().name, "app not configured, start startup worker");
        _startup_worker = std::make_unique<setup_workers::StartupWorker>();
    } else {
        create_launcher_view();
    }
}

void AppLauncher::onLauncherRunning()
{
    LvglLockGuard lock;

    if (_startup_worker) {
        _startup_worker->update();
        if (_startup_worker->isDone()) {
            _startup_worker.reset();
            _startup_checked = true;
            create_launcher_view();
        }
    } else {
#if CONFIG_SC_USB_LINK
        // [StackChan fork] "agent" over USB (sc-flash): open the AI Agent like a tap on its icon. The
        // screensaver is not closed by onLauncherClose, so drop it here, under the lock.
        if (sc::usb_link::consume_agent_request()) {
            for (const auto& app : getAppProps()) {
                if (app.info.name == "AI.AGENT") {
                    mclog::tagInfo(getAppInfo().name, "open AI Agent (USB request), app id: {}", app.appID);
                    _screensaver.reset();
                    openApp(app.appID);
                    return;
                }
            }
        }
#endif
        _view->update();
        screensaver_update();
    }

    GetStackChan().update();
}

void AppLauncher::onLauncherClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    LvglLockGuard lock;

    _view.reset();
}

void AppLauncher::onLauncherDestroy()
{
    mclog::tagInfo(getAppInfo().name, "on close");
}

void AppLauncher::create_launcher_view()
{
    _view = std::make_unique<view::LauncherView>();
    _view->init(getAppProps());
    _view->onAppClicked = [&](int appID) {
        mclog::tagInfo(getAppInfo().name, "handle open app, app id: {}", appID);
        openApp(appID);
    };
}

void AppLauncher::screensaver_update()
{
    const uint32_t SCREENSAVER_TIMEOUT_MS = 30000;

    uint32_t idle_time = lv_display_get_inactive_time(NULL);
    if (idle_time >= SCREENSAVER_TIMEOUT_MS) {
        if (!_screensaver) {
            _screensaver = std::make_unique<view::Screensaver>();
            _screensaver->init();
        }
    } else if (_screensaver) {
        _screensaver.reset();
    }

    // Update in 30ms interval
    if (_screensaver && GetHAL().millis() - _screensaver_timecount > 30) {
        _screensaver_timecount = GetHAL().millis();
        _screensaver->update();
    }
}
