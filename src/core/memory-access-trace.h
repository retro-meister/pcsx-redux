/***************************************************************************
 *   Copyright (C) 2026 PCSX-Redux authors                                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.           *
 ***************************************************************************/

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/debug.h"
#include "json.hpp"
#include "support/eventbus.h"

namespace PCSX {

class MemoryAccessTrace {
  public:
    enum class AccessMode { Read, Write, Both };

    MemoryAccessTrace();
    ~MemoryAccessTrace();

    std::string start(uint32_t address, unsigned width, AccessMode mode, const std::string& label, bool dedupeByPc,
                      bool countHits);
    nlohmann::json listTraces() const;
    nlohmann::json getTraceLog(const std::string& idOrLabel) const;
    bool clearTraceLog(const std::string& idOrLabel);
    bool stopTrace(const std::string& idOrLabel);
    void stopAll();

    static AccessMode parseAccessMode(const std::string& mode);

  private:
    struct LogEntry {
        uint32_t pc = 0;
        uint32_t callSite = 0;
        Debug::BreakpointType access = Debug::BreakpointType::Read;
        std::string function;
        std::string callSiteFunction;
        std::string cause;
        uint64_t hits = 0;
    };

    struct Trace {
        std::string id;
        std::string label;
        uint32_t address = 0;
        unsigned width = 0;
        AccessMode mode = AccessMode::Write;
        bool dedupeByPc = true;
        bool countHits = true;
        std::vector<LogEntry> entries;
        std::unordered_map<uint64_t, size_t> entryIndex;
        std::vector<const Debug::Breakpoint*> breakpoints;
    };

    void record(Trace* trace, const Debug::Breakpoint* bp, const char* cause);
    void removeBreakpoints(Trace* trace);
    Trace* findTrace(const std::string& idOrLabel);
    const Trace* findTrace(const std::string& idOrLabel) const;
    std::string resolveFunctionName(uint32_t pc) const;
    nlohmann::json traceToJson(const Trace& trace, bool includeEntries) const;
    static uint64_t entryKey(uint32_t pc, uint32_t callSite, Debug::BreakpointType access);

    std::vector<Trace> m_traces;
    uint64_t m_nextId = 1;
    EventBus::Listener m_listener;
};

}  // namespace PCSX
