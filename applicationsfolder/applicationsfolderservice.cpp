// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "applicationsfolderservice.h"

#include "base/logging.h"
#include "base/uuid.h"
#include "common/detached_call.h"

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <map>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <tuple>
#include <unistd.h>
#include <vector>

#include <linux/fs.h>

namespace dbus = cutefish::base::dbus;

static const cutefish::base::LogCategory kLog("cutefish.applicationsfolder");

static constexpr char kAction[] = "org.cutefish.applications-folder.change";
static constexpr char kStagingPrefix[] = ".cutefish-staging-";
static constexpr char kPublishingPrefix[] = ".cutefish-publishing-";
static constexpr char kFailed[] = "org.cutefish.ApplicationsFolder.Error.Failed";

// polkit's Subject: a kind and its details.
using PolkitSubject = std::tuple<std::string, dbus::VariantMap>;

class Fd
{
public:
    explicit Fd(int value)
        : value(value)
    {
    }
    ~Fd()
    {
        if (value >= 0)
            ::close(value);
    }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;

    int value;
};

static std::string uniqueName(const char *prefix)
{
    std::string id = cutefish::base::Uuid::generate().toString();
    std::erase(id, '-');
    return prefix + id;
}

static bool isPlainName(const std::string &name)
{
    return !name.empty() && name != "." && name != ".." && name.find('/') == std::string::npos
            && !name.starts_with(kStagingPrefix) && !name.starts_with(kPublishingPrefix);
}

static bool groupId(const std::string &name, gid_t *gid)
{
    struct group entry;
    struct group *found = nullptr;
    std::vector<char> buffer(16384);
    if (getgrnam_r(name.c_str(), &entry, buffer.data(), buffer.size(), &found) != 0 || !found)
        return false;
    *gid = found->gr_gid;
    return true;
}

static std::vector<std::string> childrenOf(int directory)
{
    std::vector<std::string> names;
    const int copy = ::dup(directory);
    DIR *listing = copy >= 0 ? ::fdopendir(copy) : nullptr;
    if (!listing) {
        if (copy >= 0)
            ::close(copy);
        return names;
    }
    while (struct dirent *entry = ::readdir(listing)) {
        if (std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0)
            names.push_back(entry->d_name);
    }
    ::closedir(listing);
    return names;
}

// Never follows a link, so nothing outside the tree goes.
static bool eraseAt(int parent, const std::string &name)
{
    struct stat st;
    if (::fstatat(parent, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0)
        return errno == ENOENT;
    if (S_ISDIR(st.st_mode)) {
        Fd directory(::openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (directory.value < 0)
            return false;
        bool erased = true;
        for (const std::string &child : childrenOf(directory.value))
            erased = eraseAt(directory.value, child) && erased;
        return erased && ::unlinkat(parent, name.c_str(), AT_REMOVEDIR) == 0;
    }
    return ::unlinkat(parent, name.c_str(), 0) == 0;
}

// Gives the tree to the owner and the group. Shared, it is the group's to
// change: folders 2775, programs 775, other files 664. Links keep their own
// target and only change hands; other kinds of file are refused.
static bool ownTree(int parent, const std::string &name, uid_t owner, gid_t group, bool shared)
{
    struct stat st;
    if (::fstatat(parent, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0)
        return false;
    if (S_ISLNK(st.st_mode))
        return ::fchownat(parent, name.c_str(), owner, group, AT_SYMLINK_NOFOLLOW) == 0;
    if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))
        return false;

    const int flags = S_ISDIR(st.st_mode) ? O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC
                                          : O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC;
    Fd file(::openat(parent, name.c_str(), flags));
    struct stat opened;
    if (file.value < 0 || ::fstat(file.value, &opened) != 0 || opened.st_ino != st.st_ino
        || opened.st_dev != st.st_dev)
        return false;
    if (::fchown(file.value, owner, group) != 0)
        return false;
    mode_t mode = opened.st_mode & 0777;
    if (shared)
        mode = S_ISDIR(opened.st_mode) ? 02775 : (opened.st_mode & 0111) ? 0775 : 0664;
    if (::fchmod(file.value, mode) != 0)
        return false;
    if (!S_ISDIR(opened.st_mode))
        return true;
    for (const std::string &child : childrenOf(file.value)) {
        if (!ownTree(file.value, child, owner, group, shared))
            return false;
    }
    return true;
}

static std::string failure(const char *what, const std::string &name)
{
    return std::string(what) + ' ' + name + ": " + std::strerror(errno);
}

ApplicationsFolderService::ApplicationsFolderService()
    : ApplicationsFolderService(Paths())
{
}

ApplicationsFolderService::ApplicationsFolderService(Paths paths, std::chrono::milliseconds idleTimeout)
    : m_paths(std::move(paths))
{
    m_idleTimer.setSingleShot(true);
    m_idleTimer.setInterval(idleTimeout);
    m_idleTimer.onTimeout = [this] {
        if (!m_pending && onIdle)
            onIdle();
    };
    m_idleTimer.start();
}

ApplicationsFolderService::~ApplicationsFolderService() = default;

bool ApplicationsFolderService::exportTo(dbus::Connection *bus)
{
    m_bus = bus;
    m_object = std::make_unique<dbus::Object>(bus, "/org/cutefish/ApplicationsFolder", "org.cutefish.ApplicationsFolder");

    m_object->addMethod(
            "prepare", "", "s",
            [this](dbus::MethodCall &call) {
                withCaller(call, true, [this](dbus::MethodCall &request, uint32_t user) {
                    std::string error;
                    const std::string path = prepare(user, &error);
                    if (path.empty())
                        request.replyError(kFailed, error);
                    else
                        request.reply(path);
                });
            },
            {{}, {"staging"}});
    m_object->addMethod(
            "publish", "ssb", "s",
            [this](dbus::MethodCall &call) {
                withCaller(call, false, [this](dbus::MethodCall &request, uint32_t user) {
                    std::string staging;
                    std::string name;
                    bool replace = false;
                    request.read(staging, name, replace);
                    std::string error;
                    const std::string path = publish(user, staging, name, replace, &error);
                    if (path.empty())
                        request.replyError(kFailed, error);
                    else
                        request.reply(path);
                });
            },
            {{"staging", "name", "replace"}, {"path"}});
    m_object->addMethod(
            "release", "s", "s",
            [this](dbus::MethodCall &call) {
                withCaller(call, true, [this](dbus::MethodCall &request, uint32_t user) {
                    std::string name;
                    request.read(name);
                    std::string error;
                    const std::string path = release(user, name, &error);
                    if (path.empty())
                        request.replyError(kFailed, error);
                    else
                        request.reply(path);
                });
            },
            {{"name"}, {"staging"}});
    m_object->addMethod(
            "discard", "s", "",
            [this](dbus::MethodCall &call) {
                withCaller(call, false, [this](dbus::MethodCall &request, uint32_t user) {
                    std::string staging;
                    request.read(staging);
                    std::string error;
                    if (discard(user, staging, &error))
                        request.reply();
                    else
                        request.replyError(kFailed, error);
                });
            },
            {{"staging"}, {}});
    return m_object->publish();
}

void ApplicationsFolderService::withCaller(dbus::MethodCall &call, bool ask,
                                           std::function<void(dbus::MethodCall &, uint32_t)> then)
{
    m_idleTimer.start();
    const auto answer = [this, then](dbus::MethodCall &request) {
        dbus::Message query = m_bus->newMethodCall("org.freedesktop.DBus", "/org/freedesktop/DBus",
                                                   "org.freedesktop.DBus", "GetConnectionUnixUser");
        query.append(request.message().sender());
        uint32_t user = 0;
        if (!m_bus->call(query).read(user)) {
            request.replyError("org.freedesktop.DBus.Error.AccessDenied", "The caller is not known");
            return;
        }
        then(request, user);
    };
    if (!ask) {
        answer(call);
        return;
    }

    dbus::Message check = m_bus->newMethodCall("org.freedesktop.PolicyKit1", "/org/freedesktop/PolicyKit1/Authority",
                                               "org.freedesktop.PolicyKit1.Authority", "CheckAuthorization");
    const PolkitSubject subject {"system-bus-name", {{"name", dbus::Variant(call.message().sender())}}};
    // 1: the user may be asked for an administrator's password.
    check.append(subject, std::string(kAction), std::map<std::string, std::string>(), uint32_t(1), std::string());

    ++m_pending;
    dbus::MethodCall request = call;
    cutefish::core::callDetached(
            m_bus, check,
            [this, request, answer](const dbus::Reply &reply) mutable {
                --m_pending;
                m_idleTimer.start();
                std::tuple<bool, bool, std::map<std::string, std::string>> result;
                if (!reply.read(result)) {
                    CF_WARNING(kLog) << "cutefish-applications-folder: polkit check failed:" << reply.error().message;
                    std::get<0>(result) = false;
                }
                if (!std::get<0>(result)) {
                    request.replyError("org.freedesktop.DBus.Error.AccessDenied",
                                       "Not authorized to change the Applications folder");
                    return;
                }
                answer(request);
            },
            std::chrono::minutes(10));
}

std::string ApplicationsFolderService::stagingName(uint32_t user, const std::string &path) const
{
    const std::string prefix = m_paths.folder + '/';
    if (!path.starts_with(prefix))
        return {};
    const std::string name = path.substr(prefix.size());
    if (!name.starts_with(kStagingPrefix) || name.find('/') != std::string::npos)
        return {};
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != user)
        return {};
    return name;
}

std::string ApplicationsFolderService::prepare(uint32_t user, std::string *error) const
{
    gid_t group = 0;
    if (!groupId(m_paths.group, &group)) {
        *error = "No group " + m_paths.group;
        return {};
    }
    Fd folder(::open(m_paths.folder.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (folder.value < 0) {
        *error = failure("Cannot open", m_paths.folder);
        return {};
    }
    const std::string name = uniqueName(kStagingPrefix);
    if (::mkdirat(folder.value, name.c_str(), 0700) != 0) {
        *error = failure("Cannot make", name);
        return {};
    }
    Fd staging(::openat(folder.value, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (staging.value < 0 || ::fchown(staging.value, user, group) != 0 || ::fchmod(staging.value, 02775) != 0) {
        *error = failure("Cannot hand over", name);
        eraseAt(folder.value, name);
        return {};
    }
    return m_paths.folder + '/' + name;
}

std::string ApplicationsFolderService::publish(uint32_t user, const std::string &staging, const std::string &name,
                                               bool replace, std::string *error) const
{
    const std::string stagingFolder = stagingName(user, staging);
    gid_t group = 0;
    if (stagingFolder.empty() || !isPlainName(name) || !groupId(m_paths.group, &group)) {
        *error = "Not a staged application: " + staging + '/' + name;
        return {};
    }
    Fd folder(::open(m_paths.folder.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    Fd source(folder.value >= 0
                      ? ::openat(folder.value, stagingFolder.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
                      : -1);
    if (source.value < 0) {
        *error = failure("Cannot open", staging);
        return {};
    }

    // Out of the user's reach before it changes hands.
    const std::string publishing = uniqueName(kPublishingPrefix);
    if (::mkdirat(folder.value, publishing.c_str(), 0700) != 0) {
        *error = failure("Cannot make", publishing);
        return {};
    }
    Fd held(::openat(folder.value, publishing.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    const auto finish = [&](const std::string &result) {
        eraseAt(folder.value, publishing);
        eraseAt(folder.value, stagingFolder);
        return result;
    };
    if (held.value < 0 || ::renameat(source.value, name.c_str(), held.value, name.c_str()) != 0) {
        *error = failure("Cannot take", name);
        return finish({});
    }
    if (!ownTree(held.value, name, m_paths.owner, group, true)) {
        *error = failure("Cannot share", name);
        return finish({});
    }

    if (::syscall(SYS_renameat2, held.value, name.c_str(), folder.value, name.c_str(), RENAME_NOREPLACE) == 0)
        return finish(m_paths.folder + '/' + name);
    if (errno != EEXIST || !replace) {
        *error = failure("Cannot publish", name);
        return finish({});
    }
    // Replaced whole: none of the old one's files belong to the new one.
    if (::syscall(SYS_renameat2, held.value, name.c_str(), folder.value, name.c_str(), RENAME_EXCHANGE) != 0) {
        *error = failure("Cannot replace", name);
        return finish({});
    }
    return finish(m_paths.folder + '/' + name);
}

std::string ApplicationsFolderService::release(uint32_t user, const std::string &name, std::string *error) const
{
    if (!isPlainName(name)) {
        *error = "Not in the Applications folder: " + name;
        return {};
    }
    const std::string staging = prepare(user, error);
    if (staging.empty())
        return {};
    const std::string stagingFolder = staging.substr(m_paths.folder.size() + 1);
    Fd folder(::open(m_paths.folder.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    Fd target(folder.value >= 0
                      ? ::openat(folder.value, stagingFolder.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
                      : -1);
    if (target.value < 0 || ::renameat(folder.value, name.c_str(), target.value, name.c_str()) != 0) {
        *error = failure("Cannot take out", name);
        eraseAt(folder.value, stagingFolder);
        return {};
    }
    // The user's now, to move or delete as they like.
    const struct passwd *account = ::getpwuid(user);
    if (!ownTree(target.value, name, user, account ? account->pw_gid : gid_t(-1), false))
        CF_WARNING(kLog) << "cutefish-applications-folder: not all of" << name << "changed hands";
    return staging;
}

bool ApplicationsFolderService::discard(uint32_t user, const std::string &staging, std::string *error) const
{
    const std::string stagingFolder = stagingName(user, staging);
    if (stagingFolder.empty()) {
        *error = "Not a staging folder: " + staging;
        return false;
    }
    Fd folder(::open(m_paths.folder.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (folder.value < 0 || !eraseAt(folder.value, stagingFolder)) {
        *error = failure("Cannot discard", staging);
        return false;
    }
    return true;
}
