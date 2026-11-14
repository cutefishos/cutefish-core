// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#ifndef NOTIFICATION_H
#define NOTIFICATION_H

#include <cstdint>
#include <string>

#include "base/dbus.h"
#include "base/signal.h"

// One message in the notification centre (org.freedesktop.Notifications).
// show() again replaces what it showed, until the user or close() removed it.
class Notification
{
public:
    enum Urgency : uint8_t {
        Low,
        Normal,
        Critical,
    };

    explicit Notification(cutefish::base::dbus::Connection *bus);
    ~Notification();

    Notification(const Notification &) = delete;
    Notification &operator=(const Notification &) = delete;

    // The application it is from.
    void setDesktopEntry(std::string id) { m_desktopEntry = std::move(id); }
    void setIconName(std::string name) { m_iconName = std::move(name); }
    void setSummary(std::string summary) { m_summary = std::move(summary); }
    void setBody(std::string body) { m_body = std::move(body); }
    void setUrgency(Urgency urgency) { m_urgency = urgency; }
    // Notify's expire_timeout; -1 leaves it to the server.
    void setTimeout(int timeout) { m_timeout = timeout; }
    // Clicking it then emits activated.
    void setActivatable(bool activatable) { m_activatable = activatable; }

    void show();
    void close();

    // The user or the server removed it.
    cutefish::base::Signal<> closed;
    // No server took it, as early in a session.
    cutefish::base::Signal<> failed;
    // The user clicked it.
    cutefish::base::Signal<> activated;

private:
    void send();
    void sendClose();

    cutefish::base::dbus::Connection *m_bus;
    cutefish::base::dbus::Proxy m_proxy;
    cutefish::base::dbus::Match m_closedMatch;
    cutefish::base::dbus::Match m_actionMatch;
    cutefish::base::dbus::PendingCall m_call;
    std::string m_desktopEntry;
    std::string m_iconName;
    std::string m_summary;
    std::string m_body;
    Urgency m_urgency = Normal;
    int m_timeout = -1;
    bool m_activatable = false;
    uint32_t m_id = 0;
    bool m_pending = false;
    bool m_showAgain = false;
    bool m_closeWhenShown = false;
};

#endif // NOTIFICATION_H
