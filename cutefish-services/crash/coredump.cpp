// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "coredump.h"

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cxxabi.h>
#include <filesystem>
#include <format>
#include <map>
#include <regex>

#include "base/strings.h"

using namespace cutefish::base;
using namespace cutefish::base::strings;

static const std::string s_scopePrefix = "app-cutefish-";
static const std::string s_scopeSuffix = ".scope";

// The scope name without its prefix and suffix: <escaped id>-<pid>.
static std::string scopeInstance(const std::string &unit)
{
    if (!startsWith(unit, s_scopePrefix) || !endsWith(unit, s_scopeSuffix)
        || unit.size() < s_scopePrefix.size() + s_scopeSuffix.size())
        return {};
    return unit.substr(s_scopePrefix.size(), unit.size() - s_scopePrefix.size() - s_scopeSuffix.size());
}

// Reverses systemd's unit name escaping, \xNN for each byte of UTF-8.
static std::string unescape(const std::string &text)
{
    std::string result;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 3 < text.size() && text[i + 1] == 'x') {
            const auto byte = toNumber<int>(std::string_view(text).substr(i + 2, 2), 16);
            if (byte) {
                result += char(*byte);
                i += 3;
                continue;
            }
        }
        result += text[i];
    }
    return result;
}

std::string Coredump::applicationId() const
{
    const std::string instance = scopeInstance(userUnit);
    const size_t dash = instance.rfind('-');
    if (dash == std::string::npos || dash == 0)
        return {};
    return unescape(instance.substr(0, dash));
}

uint32_t Coredump::applicationPid() const
{
    const std::string instance = scopeInstance(userUnit);
    const size_t dash = instance.rfind('-');
    if (dash == std::string::npos || dash == 0)
        return 0;
    return toNumber<uint32_t>(std::string_view(instance).substr(dash + 1)).value_or(0);
}

std::string Coredump::signalDescription() const
{
    const std::string description = signal > 0 ? std::string(::strsignal(signal)) : std::string();
    if (signalName.empty())
        return description;
    if (description.empty())
        return signalName;
    return std::format("{} ({})", description, signalName);
}

// The local time as ISO 8601 writes it without a zone: 2026-10-06T12:34:56.
static std::string isoTime(int64_t milliseconds)
{
    const time_t seconds = time_t(milliseconds / 1000);
    struct tm local {};
    localtime_r(&seconds, &local);
    char text[64];
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%S", &local);
    return text;
}

std::string Coredump::report(const std::string &name) const
{
    std::string text;
    text += std::format("{} quit unexpectedly\n", name);
    text += std::format("Executable: {}\n", executable);
    if (!commandLine.empty())
        text += std::format("Command line: {}\n", commandLine);
    text += std::format("Signal: {}\n", signalDescription());
    if (time > 0)
        text += std::format("Time: {}\n", isoTime(time));
    for (const CoredumpThread &thread : threads) {
        text += std::format("\nThread {}:\n", thread.id);
        for (size_t i = 0; i < thread.frames.size(); ++i) {
            const CoredumpFrame &frame = thread.frames[i];
            const std::string function = frame.function.empty() ? std::string("??") : frame.function;
            if (frame.module.empty())
                text += std::format("#{} 0x{:016x} {}\n", i, frame.address, function);
            else
                text += std::format("#{} 0x{:016x} {} ({} + 0x{:x})\n", i, frame.address, function, frame.module,
                                    frame.offset);
        }
    }
    return text;
}

std::vector<CoredumpThread> Coredump::parseStackTrace(const std::string &message)
{
    static const std::regex threadLine("^Stack trace of thread (\\d+):$");
    // #3  0x00007fe7198dbede name (module + 0xdbede); the module part is
    // missing for an address no mapping covers.
    static const std::regex frameLine("^#\\d+\\s+0x([0-9a-fA-F]+)\\s+(\\S+)(?:\\s+\\((.*?)(?: \\+ 0x([0-9a-fA-F]+))?\\))?$");

    std::vector<CoredumpThread> threads;
    for (const std::string_view rawLine : splitViews(message, '\n')) {
        const std::string line(trimmed(rawLine));
        std::smatch match;
        if (std::regex_match(line, match, threadLine)) {
            threads.push_back({match[1].str(), {}});
            continue;
        }
        if (threads.empty())
            continue;
        if (!std::regex_match(line, match, frameLine))
            continue;
        CoredumpFrame frame;
        frame.address = toNumber<uint64_t>(match[1].str(), 16).value_or(0);
        const std::string symbol = match[2].str();
        if (symbol != "n/a")
            frame.function = demangle(symbol);
        if (match[3].matched && match[3].str() != "n/a") {
            frame.module = match[3].str();
            if (match[4].matched)
                frame.offset = toNumber<uint64_t>(match[4].str(), 16).value_or(0);
        }
        threads.back().frames.push_back(std::move(frame));
    }
    return threads;
}

namespace {

struct ProgramModule {
    uint64_t start = 0;
    uint64_t end = 0;
    std::string name;
};

} // namespace

void Coredump::addProgramSymbols(const std::string &symbols, const std::string &modules)
{
    std::map<uint64_t, std::string> names;
    for (const std::string_view line : splitViews(symbols, '\n')) {
        const size_t space = line.find(' ');
        if (space == std::string_view::npos)
            continue;
        if (const auto address = toNumber<uint64_t>(line.substr(0, space), 16))
            names[*address] = std::string(line.substr(space + 1));
    }

    std::vector<ProgramModule> ranges;
    for (const std::string_view line : splitViews(modules, '\n')) {
        const size_t first = line.find(' ');
        const size_t second = first == std::string_view::npos ? first : line.find(' ', first + 1);
        if (second == std::string_view::npos)
            continue;
        const auto start = toNumber<uint64_t>(line.substr(0, first), 16);
        const auto end = toNumber<uint64_t>(line.substr(first + 1, second - first - 1), 16);
        if (!start || !end || *end <= *start)
            continue;
        const std::string path(line.substr(second + 1));
        ranges.push_back({*start, *end, std::filesystem::path(path).filename().string()});
    }

    for (CoredumpThread &thread : threads) {
        for (CoredumpFrame &frame : thread.frames) {
            if (!frame.module.empty())
                continue;
            for (const ProgramModule &range : ranges) {
                if (frame.address < range.start || frame.address >= range.end)
                    continue;
                frame.module = range.name;
                frame.offset = frame.address - range.start;
                break;
            }
            if (frame.function.empty()) {
                if (const auto it = names.find(frame.address); it != names.end())
                    frame.function = demangle(it->second);
            }
        }
    }
}

std::string Coredump::demangle(const std::string &symbol)
{
    if (!startsWith(symbol, "_Z"))
        return symbol;
    int status = 0;
    char *name = abi::__cxa_demangle(symbol.c_str(), nullptr, nullptr, &status);
    if (status != 0 || !name)
        return symbol;
    std::string result = name;
    std::free(name);
    return result;
}
