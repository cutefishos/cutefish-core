// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "notification.h"

#include <systemd/sd-bus.h>

using namespace cutefish::base;

static const char kService[] = "org.freedesktop.Notifications";
static const char kPath[] = "/org/freedesktop/Notifications";

Notification::Notification(dbus::Connection *bus)
    : m_bus(bus)
    , m_proxy(bus, kService, kPath, kService)
{
}

Notification::~Notification() = default;

void Notification::show()
{
    m_closeWhenShown = false;

    // The reply brings the id the new content has to replace.
    if (m_pending) {
        m_showAgain = true;
        return;
    }

    send();
}

void Notification::close()
{
    m_showAgain = false;

    if (m_pending) {
        m_closeWhenShown = true;
        return;
    }

    sendClose();
}

void Notification::send()
{
    if (!m_bus) {
        failed.emit();
        return;
    }

    if (m_closedMatch.isNull()) {
        m_closedMatch = m_proxy.onSignal("NotificationClosed", [this](const dbus::Message &message) {
            uint32_t id = 0;
            if (!message.read(id) || id == 0 || id != m_id)
                return;
            // A later show() is a new notification.
            m_id = 0;
            closed.emit();
        });
    }
    if (m_activatable && m_actionMatch.isNull()) {
        m_actionMatch = m_proxy.onSignal("ActionInvoked", [this](const dbus::Message &message) {
            uint32_t id = 0;
            std::string key;
            if (message.read(id, key) && id != 0 && id == m_id && key == "default")
                activated.emit();
        });
    }

    dbus::VariantMap hints {{"urgency", dbus::Variant(uint8_t(m_urgency))}};
    if (!m_desktopEntry.empty())
        hints.emplace("desktop-entry", dbus::Variant(m_desktopEntry));

    dbus::Message call = m_bus->newMethodCall(kService, kPath, kService, "Notify");
    // Only a running notification server shows it; none is started for it.
    sd_bus_message_set_auto_start(call.get(), 0);
    const std::vector<std::string> actions = m_activatable ? std::vector<std::string> {"default", ""}
                                                           : std::vector<std::string>();
    call.append(std::string(), m_id, m_iconName, m_summary, m_body, actions, hints, int32_t(m_timeout));

    m_pending = true;
    m_call = m_bus->callAsync(call, [this](const dbus::Reply &reply) {
        m_pending = false;

        if (reply.isError()) {
            m_id = 0;
            m_showAgain = false;
            m_closeWhenShown = false;
            failed.emit();
            return;
        }

        // 0: the server dropped it, as the shell does with a repeat within a second.
        m_id = reply.value<uint32_t>().value_or(0);

        if (m_closeWhenShown) {
            m_closeWhenShown = false;
            close();
        } else if (m_showAgain) {
            m_showAgain = false;
            send();
        }
    });
}

void Notification::sendClose()
{
    if (m_id == 0 || !m_bus)
        return;

    dbus::Message call = m_bus->newMethodCall(kService, kPath, kService, "CloseNotification");
    sd_bus_message_set_auto_start(call.get(), 0);
    call.append(m_id);
    m_bus->send(call);

    m_id = 0;
}
