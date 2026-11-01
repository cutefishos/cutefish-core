#include "brightnessmanager.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <filesystem>
#include <sys/inotify.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "base/event_loop.h"
#include "base/file.h"
#include "base/strings.h"
#include "dbuscall.h"
#include "log.h"

using namespace std::chrono_literals;
using namespace cutefish::base;
using namespace cutefish::base::strings;
namespace fs = std::filesystem;

static const std::string BacklightDir = "/sys/class/backlight/";
static const std::string DrmDir = "/sys/class/drm/";

static constexpr std::chrono::milliseconds kFadeFrameInterval = 16ms;

// A value asked for while the last one is still fading in, as a dragged slider's
// or a held key's, follows this quickly so the panel keeps up.
static constexpr std::chrono::milliseconds kFollowFade = 64ms;

// Fades move evenly in perceived brightness rather than in backlight power.
static constexpr double kPerceptualGamma = 2.2;

// Writers that replace the file drop the watch; IN_IGNORED says so.
static constexpr uint32_t kWatchMask = IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB | IN_MOVE_SELF | IN_DELETE_SELF;

static std::string readSysfs(const std::string &path)
{
    return std::string(trimmed(file::read(path).value_or(std::string())));
}

static int toInt(const std::string &text)
{
    return toNumber<int>(text).value_or(0);
}

static std::string brightnessFile(const std::string &backlight)
{
    return BacklightDir + backlight + "/brightness";
}

// The entries of a directory that are directories or links to them, sorted by name.
static std::vector<std::string> subdirectories(const std::string &directory)
{
    std::vector<std::string> names;
    std::error_code error;
    for (const fs::directory_entry &entry : fs::directory_iterator(directory, error)) {
        std::error_code typeError;
        if (entry.is_directory(typeError))
            names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

// The name of what the link at the path resolves to; empty when it does not.
static std::string canonicalName(const std::string &path)
{
    std::error_code error;
    const fs::path target = fs::canonical(path, error);
    return error ? std::string() : target.filename().string();
}

static bool exists(const std::string &path)
{
    std::error_code error;
    return fs::exists(path, error);
}

// Connected DRM connectors, e.g. "card0-eDP-1".
static std::vector<std::string> connectedConnectors()
{
    std::vector<std::string> result;
    for (const std::string &entry : subdirectories(DrmDir)) {
        if (!startsWith(entry, "card") || entry.find('-', 4) == std::string::npos)
            continue;
        if (readSysfs(DrmDir + entry + "/status") == "connected")
            result.push_back(entry);
    }
    return result;
}

static bool isInternalConnector(const std::string &connector)
{
    const std::string_view type = section(connector, '-', 1, 1);
    return type == "eDP" || type == "LVDS" || type == "DSI";
}

static int typeRank(const std::string &type)
{
    if (type == "firmware")
        return 0;
    if (type == "platform")
        return 1;
    return 2;
}

static double easeOut(double progress)
{
    const double remaining = 1.0 - progress;
    return 1.0 - remaining * remaining * remaining;
}

static int fadeValue(int from, int to, int maximum, double progress)
{
    const double start = std::pow(double(from) / maximum, 1.0 / kPerceptualGamma);
    const double end = std::pow(double(to) / maximum, 1.0 / kPerceptualGamma);
    const double level = start + (end - start) * easeOut(progress);
    return int(std::lround(std::pow(level, kPerceptualGamma) * maximum));
}

static bool contains(const std::vector<std::string> &list, const std::string &value)
{
    return std::find(list.begin(), list.end(), value) != list.end();
}

BrightnessManager::BrightnessManager(dbus::Connection *session, dbus::Connection *system)
    : m_session(session)
    , m_system(system)
    , m_ddcWorker([this](uint64_t generation, const std::vector<DdcDisplay> &displays) {
        handleDdcProbed(generation, displays);
    })
{
    m_inotify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (m_inotify >= 0) {
        m_inotifyWatch = EventLoop::current()->watch(m_inotify, EPOLLIN, [this](uint32_t) {
            readInotify();
        });
    }

    // Monitors need a moment after hotplug before they answer DDC/CI.
    m_rescanTimer.setSingleShot(true);
    m_rescanTimer.setInterval(1500ms);
    m_rescanTimer.onTimeout = [this] {
        rescan();
    };
    m_fadeTimer.setInterval(kFadeFrameInterval);
    m_fadeTimer.onTimeout = [this] {
        stepFades();
    };
    if (m_session) {
        dbus::MatchRule rule;
        rule.path = "/org/cutefish/DisplayConfig1";
        rule.interface = "org.cutefish.DisplayConfig1";
        rule.member = "StateChanged";
        m_displayStateChanged = m_session->subscribe(rule, [this](const dbus::Message &) {
            m_rescanTimer.start();
        });
    }

    rescan();
}

BrightnessManager::~BrightnessManager()
{
    if (m_inotifyWatch)
        EventLoop::current()->unwatch(m_inotifyWatch);
    if (m_inotify >= 0)
        ::close(m_inotify);
}

bool BrightnessManager::publish()
{
    if (!m_session)
        return false;
    m_object = std::make_unique<dbus::Object>(m_session, "/org/cutefish/Services/Brightness",
                                              "org.cutefish.Services.Brightness");
    dbus::Object &object = *m_object;
    object.addMethod("setValue", [this](int32_t value) { setValue(value); }, {{"value"}, {}});
    object.addMethod("displays", [this] { return displays(); });
    object.addMethod("displayBrightness",
                     [this](const std::string &display) { return int32_t(displayBrightness(display)); },
                     {{"display"}, {"value"}});
    object.addMethod("setDisplayBrightness",
                     [this](const std::string &display, int32_t value) { setDisplayBrightness(display, value); },
                     {{"display", "value"}, {}});
    object.addProperty("maxBrightness", [this] { return int32_t(maxBrightness()); });
    object.addProperty("brightness", [this] { return int32_t(brightness()); });
    object.addProperty("brightnessEnabled", [this] { return brightnessEnabled(); });
    object.addSignal("brightnessChanged", "");
    object.addSignal("displaysChanged", "");
    object.addSignal("displayBrightnessChanged", "si", {"display", "value"});
    return object.publish();
}

int BrightnessManager::maxBrightness()
{
    return m_displays.empty() ? -1 : m_displays.front().maximum;
}

int BrightnessManager::brightness()
{
    return m_displays.empty() ? -1 : percent(m_displays.front());
}

bool BrightnessManager::brightnessEnabled()
{
    return !m_displays.empty();
}

void BrightnessManager::setValue(int value, std::chrono::milliseconds fade)
{
    if (!m_displays.empty())
        setDisplayBrightness(m_displays.front().connector, value, fade);
}

std::vector<std::string> BrightnessManager::displays() const
{
    std::vector<std::string> result;
    for (const Display &display : m_displays)
        result.push_back(display.connector);
    return result;
}

int BrightnessManager::displayBrightness(const std::string &display) const
{
    const Display *entry = find(display);
    return entry ? percent(*entry) : -1;
}

void BrightnessManager::setDisplayBrightness(const std::string &connector, int value,
                                             std::chrono::milliseconds fade)
{
    Display *display = find(connector);
    if (!display)
        return;

    int raw = int(std::lround(std::clamp(value, 0, 100) * display->maximum / 100.0));
    // Raw 0 switches some backlights off entirely.
    if (display->kind == Kind::Backlight)
        raw = std::max(1, raw);
    if (raw == display->raw)
        return;

    const int oldPercent = percent(*display);
    display->raw = raw;

    // DDC/CI writes are too slow to drive frame by frame.
    if (display->kind == Kind::Backlight)
        startFade(*display, fade);
    else
        m_ddcWorker.requestValue(display->device, raw);

    notifyIfChanged(*display, oldPercent);
}

void BrightnessManager::startFade(Display &display, std::chrono::milliseconds duration)
{
    if (duration <= 0ms || display.written <= 0 || display.maximum <= 0) {
        display.fading = false;
        writeBacklight(display, display.raw);
        return;
    }

    if (display.fading)
        duration = std::min(duration, kFollowFade);
    display.fading = true;
    display.fadeFrom = display.written;
    display.fadeStart = std::chrono::steady_clock::now();
    display.fadeDuration = duration;
    if (!m_fadeTimer.isActive())
        m_fadeTimer.start();
}

void BrightnessManager::stepFades()
{
    const auto now = std::chrono::steady_clock::now();
    bool fading = false;
    for (Display &display : m_displays) {
        if (!display.fading)
            continue;

        const double progress = std::chrono::duration<double>(now - display.fadeStart)
                / std::chrono::duration<double>(display.fadeDuration);
        if (progress >= 1.0) {
            display.fading = false;
            writeBacklight(display, display.raw);
            continue;
        }

        fading = true;
        writeBacklight(display, std::max(1, fadeValue(display.fadeFrom, display.raw, display.maximum, progress)));
    }
    if (!fading)
        m_fadeTimer.stop();
}

void BrightnessManager::writeBacklight(Display &display, int raw)
{
    if (raw == display.written || !m_system)
        return;

    display.written = raw;
    ++display.pendingWrites;

    const dbus::Proxy session(m_system, "org.freedesktop.login1",
                              "/org/freedesktop/login1/session/auto",
                              "org.freedesktop.login1.Session");
    services::callDetached(
            session, "SetBrightness",
            [lifetime = std::weak_ptr<char>(m_lifetime), this, device = display.device](const dbus::Reply &reply) {
                if (reply.isError())
                    CF_WARNING(services::log())
                            << "Setting backlight brightness failed:" << reply.error().message;
                if (!lifetime.expired())
                    handleBacklightWritten(device);
            },
            std::string("backlight"), display.device, uint32_t(raw));
}

void BrightnessManager::handleBacklightWritten(const std::string &device)
{
    for (Display &display : m_displays) {
        if (display.kind != Kind::Backlight || display.device != device)
            continue;

        display.pendingWrites = std::max(0, display.pendingWrites - 1);
        // Our own writes were not read back; catch anything else that wrote meanwhile.
        if (display.pendingWrites == 0 && !display.fading)
            handleFileChanged(brightnessFile(device));
        return;
    }
}

void BrightnessManager::rescan()
{
    ++m_generation;
    const std::vector<std::string> connected = connectedConnectors();
    const std::vector<Display> backlights = scanBacklights(connected);

    std::vector<DdcDisplay> candidates = ddcCandidates(connected, backlights);

    // Keep known DDC displays until the probe answers so sliders do not flicker.
    std::vector<Display> displays = backlights;
    for (const Display &display : m_displays) {
        if (display.kind != Kind::Ddc)
            continue;
        for (const DdcDisplay &candidate : candidates) {
            if (candidate.connector == display.connector && candidate.device == display.device)
                displays.push_back(display);
        }
    }
    publish(std::move(displays));

    if (!candidates.empty())
        m_ddcWorker.probe(m_generation, std::move(candidates));
}

std::vector<BrightnessManager::Display>
BrightnessManager::scanBacklights(const std::vector<std::string> &connected) const
{
    std::vector<std::string> internal;
    for (const std::string &connector : connected) {
        if (isInternalConnector(connector))
            internal.push_back(connector);
    }

    // Firmware and platform interfaces carry no connector link; they drive the internal panel.
    std::map<std::string, std::pair<int, Display>> best;
    for (const std::string &name : subdirectories(BacklightDir)) {
        const std::string path = BacklightDir + name;
        Display display;
        display.kind = Kind::Backlight;
        display.device = name;
        display.maximum = toInt(readSysfs(path + "/max_brightness"));
        if (display.maximum <= 0)
            continue;

        const std::string parent = canonicalName(path + "/device");
        const bool attached = !parent.empty() && exists(DrmDir + parent) && startsWith(parent, "card");
        if (attached)
            display.connector = parent;
        else if (!internal.empty())
            display.connector = internal.front();
        if (display.connector.empty() || !contains(connected, display.connector))
            continue;

        const int rank = typeRank(readSysfs(path + "/type")) * 2 + (attached ? 0 : 1);
        auto it = best.find(display.connector);
        if (it == best.end() || rank < it->second.first) {
            readBacklight(display);
            best.insert_or_assign(display.connector, std::make_pair(rank, display));
        }
    }

    std::vector<Display> result;
    for (const std::string &connector : connected) {
        if (const auto it = best.find(connector); it != best.end())
            result.push_back(it->second.second);
    }
    return result;
}

std::vector<DdcDisplay> BrightnessManager::ddcCandidates(const std::vector<std::string> &connected,
                                                         const std::vector<Display> &backlights) const
{
    std::vector<DdcDisplay> result;
    for (const std::string &connector : connected) {
        if (isInternalConnector(connector))
            continue;
        const bool hasBacklight = std::any_of(backlights.cbegin(), backlights.cend(),
                                              [&](const Display &display) {
                                                  return display.connector == connector;
                                              });
        if (hasBacklight)
            continue;

        // HDMI/DVI/VGA link their DDC adapter; DP exposes an AUX-backed i2c child.
        const std::string path = DrmDir + connector;
        std::string bus = canonicalName(path + "/ddc");
        if (!startsWith(bus, "i2c-")) {
            bus.clear();
            for (const std::string &child : subdirectories(path)) {
                if (startsWith(child, "i2c-")) {
                    bus = child;
                    break;
                }
            }
        }
        if (bus.empty() || !exists("/dev/" + bus))
            continue;

        DdcDisplay candidate;
        candidate.connector = connector;
        candidate.device = "/dev/" + bus;
        result.push_back(candidate);
    }
    return result;
}

void BrightnessManager::handleDdcProbed(uint64_t generation, const std::vector<DdcDisplay> &probed)
{
    if (generation != m_generation)
        return;

    // Backlight values may have changed since the scan; m_displays holds the latest.
    std::vector<Display> displays;
    for (const Display &display : m_displays) {
        if (display.kind == Kind::Backlight)
            displays.push_back(display);
    }
    for (const DdcDisplay &entry : probed) {
        Display display;
        display.connector = entry.connector;
        display.kind = Kind::Ddc;
        display.device = entry.device;
        display.maximum = entry.maximum;
        display.raw = entry.value;
        // A slider write may still be queued; its value is newer than the probe.
        if (const Display *known = find(entry.connector); known && known->device == entry.device)
            display.raw = known->raw;
        displays.push_back(display);
    }
    publish(std::move(displays));
}

void BrightnessManager::publish(std::vector<Display> displays)
{
    const std::vector<std::string> oldConnectors = this->displays();
    const int oldDefault = brightness();
    const std::vector<Display> old = m_displays;
    m_displays = std::move(displays);

    // A rescan reads a backlight mid-fade; keep fading to the value asked for.
    for (Display &display : m_displays) {
        if (display.kind != Kind::Backlight)
            continue;
        const auto it = std::find_if(old.cbegin(), old.cend(), [&](const Display &entry) {
            return entry.kind == Kind::Backlight && entry.device == display.device;
        });
        if (it != old.cend() && (it->fading || it->pendingWrites > 0)) {
            display.raw = it->raw;
            display.written = it->written;
            display.pendingWrites = it->pendingWrites;
            display.fading = it->fading;
            display.fadeFrom = it->fadeFrom;
            display.fadeStart = it->fadeStart;
            display.fadeDuration = it->fadeDuration;
        }
    }

    watchFiles();

    if (this->displays() != oldConnectors && m_object)
        m_object->emitSignal("displaysChanged");

    for (const Display &display : m_displays) {
        const auto it = std::find_if(old.cbegin(), old.cend(), [&](const Display &entry) {
            return entry.connector == display.connector;
        });
        if (it == old.cend() || percent(*it) != percent(display))
            emitDisplayBrightnessChanged(display.connector, percent(display));
    }

    if (brightness() != oldDefault)
        emitBrightnessChanged();
}

void BrightnessManager::watchFiles()
{
    if (m_inotify < 0)
        return;

    std::map<std::string, int> watches;
    for (const Display &display : m_displays) {
        if (display.kind != Kind::Backlight)
            continue;
        const std::string path = brightnessFile(display.device);
        if (const auto it = m_watches.find(path); it != m_watches.end()) {
            watches.insert(*it);
            m_watches.erase(it);
            continue;
        }
        const int watch = inotify_add_watch(m_inotify, path.c_str(), kWatchMask);
        if (watch >= 0)
            watches.emplace(path, watch);
    }
    for (const auto &[path, watch] : m_watches)
        inotify_rm_watch(m_inotify, watch);
    m_watches = std::move(watches);
}

void BrightnessManager::readInotify()
{
    alignas(inotify_event) char buffer[4096];
    std::vector<std::string> changed;
    for (;;) {
        const ssize_t length = ::read(m_inotify, buffer, sizeof(buffer));
        if (length <= 0)
            break;
        for (ssize_t offset = 0; offset < length;) {
            const auto *event = reinterpret_cast<const inotify_event *>(buffer + offset);
            offset += ssize_t(sizeof(inotify_event) + event->len);
            for (auto it = m_watches.begin(); it != m_watches.end(); ++it) {
                if (it->second != event->wd)
                    continue;
                // Some writers replace the file, which drops it from the watch.
                if (event->mask & IN_IGNORED) {
                    const int watch = inotify_add_watch(m_inotify, it->first.c_str(), kWatchMask);
                    if (watch >= 0)
                        it->second = watch;
                }
                if (std::find(changed.begin(), changed.end(), it->first) == changed.end())
                    changed.push_back(it->first);
                break;
            }
        }
    }
    for (const std::string &path : changed)
        handleFileChanged(path);
}

void BrightnessManager::readBacklight(Display &display) const
{
    display.raw = toInt(readSysfs(brightnessFile(display.device)));
    display.written = display.raw;
}

BrightnessManager::Display *BrightnessManager::find(const std::string &display)
{
    for (Display &entry : m_displays) {
        if (entry.connector == display)
            return &entry;
    }
    return nullptr;
}

const BrightnessManager::Display *BrightnessManager::find(const std::string &display) const
{
    return const_cast<BrightnessManager *>(this)->find(display);
}

int BrightnessManager::percent(const Display &display) const
{
    return display.maximum > 0 ? int(std::lround(display.raw * 100.0 / display.maximum)) : -1;
}

void BrightnessManager::notifyIfChanged(const Display &display, int oldPercent)
{
    const int newPercent = percent(display);
    if (newPercent == oldPercent)
        return;

    emitDisplayBrightnessChanged(display.connector, newPercent);
    if (&display == &m_displays.front())
        emitBrightnessChanged();
}

void BrightnessManager::emitBrightnessChanged()
{
    if (m_object)
        m_object->emitSignal("brightnessChanged");
}

void BrightnessManager::emitDisplayBrightnessChanged(const std::string &display, int value)
{
    if (m_object)
        m_object->emitSignal("displayBrightnessChanged", display, int32_t(value));
}

void BrightnessManager::handleFileChanged(const std::string &path)
{
    for (Display &display : m_displays) {
        if (display.kind != Kind::Backlight || brightnessFile(display.device) != path)
            continue;
        // The file is only partway through our own fade.
        if (display.fading || display.pendingWrites > 0)
            return;

        const int oldPercent = percent(display);
        readBacklight(display);
        notifyIfChanged(display, oldPercent);
        return;
    }
}
