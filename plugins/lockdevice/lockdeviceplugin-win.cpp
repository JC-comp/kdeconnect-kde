/**
 * SPDX-FileCopyrightText: 2021 Piyush Aggarwal <piyushaggarwal002@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "lockdeviceplugin-win.h"

#include "plugin_lockdevice_debug.h"
#include <KLocalizedString>
#include <KPluginFactory>
#include <QDebug>

#include <core/daemon.h>
#include <core/device.h>
#include <dbushelper.h>

K_PLUGIN_CLASS_WITH_JSON(LockDevicePlugin, "kdeconnect_lockdevice.json")

const wchar_t LockDevicePlugin::CLASS_NAME[] = L"WTSSessionListenerClass";
const wchar_t LockDevicePlugin::WINDOW_TITLE[] = L"WTS Session Listener";

LockDevicePlugin::LockDevicePlugin(QObject *parent, const QVariantList &args)
    : KdeConnectPlugin(parent, args)
    , m_remoteLocked(false)
    , hWnd(nullptr)
{
    registerSessionListener();
}

void LockDevicePlugin::registerSessionListener()
{
    auto hInstance = GetModuleHandle(nullptr);

    WNDCLASSW wc = {};
    if (!GetClassInfoW(hInstance, CLASS_NAME, &wc)) {
        wc.lpfnWndProc = &StaticWndProc;
        wc.hInstance = hInstance;
        wc.lpszClassName = CLASS_NAME;

        RegisterClassW(&wc);
    }

    hWnd = CreateWindowExW(0, // Optional window styles.
                           CLASS_NAME, // Window class
                           WINDOW_TITLE, // Window text
                           WS_OVERLAPPEDWINDOW, // Window style

                           // Size and position
                           CW_USEDEFAULT,
                           CW_USEDEFAULT,
                           CW_USEDEFAULT,
                           CW_USEDEFAULT,

                           nullptr, // Parent window
                           nullptr, // Menu
                           GetModuleHandle(nullptr), // Instance handle
                           nullptr // Pointer to window-creation data
    );

    if (hWnd == nullptr) {
        qWarning(KDECONNECT_PLUGIN_LOCKDEVICE) << "CreateWindowExW failed: " << GetLastError();
        return;
    }

    // Subscribe to WTS session change notifications
    if (!WTSRegisterSessionNotification(hWnd, NOTIFY_FOR_ALL_SESSIONS)) {
        qWarning(KDECONNECT_PLUGIN_LOCKDEVICE) << "WTSRegisterSessionNotification failed: " << GetLastError();
        DestroyWindow(hWnd);
        return;
    }

    SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    ShowWindow(hWnd, SW_HIDE); // Hide the window as we only need to listen for messages
}

LockDevicePlugin::~LockDevicePlugin()
{
    qWarning(KDECONNECT_PLUGIN_LOCKDEVICE) << "UnregisterSessionListener";
    if (hWnd != nullptr) {
        // Unsubscribe from WTS session change notifications
        WTSUnRegisterSessionNotification(hWnd);
        hWnd = nullptr;
    }

    auto hInstance = GetModuleHandle(nullptr);
    WNDCLASSW wc = {};
    if (GetClassInfoW(hInstance, CLASS_NAME, &wc)) {
        qWarning(KDECONNECT_PLUGIN_LOCKDEVICE) << "Unregistering Class";
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
    }
}

bool LockDevicePlugin::isLocked() const
{
    return m_localLocked;
}

void LockDevicePlugin::setLocked(bool locked)
{
    NetworkPacket np(PACKET_TYPE_LOCK_REQUEST, {{QStringLiteral("setLocked"), locked}});
    sendPacket(np);
}

void LockDevicePlugin::receivePacket(const NetworkPacket &np)
{
    if (np.has(QStringLiteral("isLocked"))) {
        bool locked = np.get<bool>(QStringLiteral("isLocked"));
        if (m_remoteLocked != locked) {
            m_remoteLocked = locked;
            Q_EMIT lockedChanged(locked);
        }
    }

    if (np.has(QStringLiteral("requestLocked"))) {
        sendState();
    }

    // Receiving result of setLocked
    if (np.has(QStringLiteral("lockResult"))) {
        bool lockSuccess = np.get<bool>(QStringLiteral("lockResult"));
        if (lockSuccess) {
            Daemon::instance()->sendSimpleNotification(QStringLiteral("remoteLockSuccess"),
                                                       device()->name(),
                                                       i18n("Remote lock successful"),
                                                       QStringLiteral("error"));
        } else {
            Daemon::instance()->sendSimpleNotification(QStringLiteral("remoteLockFail"), device()->name(), i18n("Remote lock failed"), QStringLiteral("error"));
            Daemon::instance()->reportError(device()->name(), i18n("Remote lock failed"));
        }
    }

    if (np.has(QStringLiteral("setLocked"))) {
        const bool lock = np.get<bool>(QStringLiteral("setLocked"));
        bool success = false;
        if (lock) {
            success = LockWorkStation();
            if (success)
                m_localLocked = true;
        }
        NetworkPacket np(PACKET_TYPE_LOCK, {{QStringLiteral("lockResult"), success}});
        sendPacket(np);

        sendState();
    }
}

void LockDevicePlugin::sendState()
{
    NetworkPacket np(PACKET_TYPE_LOCK, {{QStringLiteral("isLocked"), m_localLocked}});
    sendPacket(np);
}

void LockDevicePlugin::connected()
{
    NetworkPacket np(PACKET_TYPE_LOCK_REQUEST, {{QStringLiteral("requestLocked"), QVariant()}});
    sendPacket(np);
}

QString LockDevicePlugin::dbusPath() const
{
    return QLatin1String("/modules/kdeconnect/devices/%1/lockdevice").arg(device()->id());
}

LRESULT CALLBACK LockDevicePlugin::StaticWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    // Retrieve the 'this' pointer
    LockDevicePlugin *pWnd = reinterpret_cast<LockDevicePlugin *>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    if (pWnd) {
        // Call the non-static WndProc
        return pWnd->WndProc(hWnd, message, wParam, lParam);
    } else {
        // Handle messages before WM_NCCREATE or after the window is destroyed
        return DefWindowProcW(hWnd, message, wParam, lParam);
    }
}

LRESULT CALLBACK LockDevicePlugin::WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_WTSSESSION_CHANGE:
        switch (wParam) {
        case WTS_SESSION_LOCK:
            m_localLocked = true;
            sendState();
            break;
        case WTS_SESSION_UNLOCK:
            m_localLocked = false;
            sendState();
            break;
        default:
            break;
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hWnd, message, wParam, lParam);
    }
}

#include "lockdeviceplugin-win.moc"
#include "moc_lockdeviceplugin-win.cpp"
