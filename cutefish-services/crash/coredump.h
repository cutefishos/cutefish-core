// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct CoredumpFrame {
    std::string function;
    std::string module;
    uint64_t address = 0;
    // From the start of the module; only with a module.
    uint64_t offset = 0;
};

struct CoredumpThread {
    std::string id;
    std::vector<CoredumpFrame> frames;
};

// One systemd-coredump journal entry. The stack trace is what systemd-coredump
// unwound when it took the dump: exported symbols only, no source lines.
struct Coredump {
    uint32_t pid = 0;
    int signal = 0;
    std::string signalName;
    std::string executable;
    std::string commandLine;
    std::string userUnit;
    std::string cgroup;
    // Milliseconds since the epoch; 0 when unknown.
    int64_t time = 0;
    // The crashing thread comes first.
    std::vector<CoredumpThread> threads;

    // The desktop entry id of an application the runtime started, from the
    // app-cutefish-<id>-<pid>.scope it runs in; empty for anything else.
    std::string applicationId() const;
    // The pid the runtime started the application with.
    uint32_t applicationPid() const;

    std::string signalDescription() const;
    // Plain text for the clipboard.
    std::string report(const std::string &name) const;

    // Names the frames systemd-coredump left without a module, in code the
    // program loaded other than as ELF, from the journal entry the program
    // logged before it died: the symbols of its frames as "<address> <name>"
    // lines and its modules as "<start> <end> <path>" lines, addresses in hex.
    void addProgramSymbols(const std::string &symbols, const std::string &modules);

    static std::vector<CoredumpThread> parseStackTrace(const std::string &message);
    static std::string demangle(const std::string &symbol);
};
