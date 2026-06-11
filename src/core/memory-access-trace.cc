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

#include "core/memory-access-trace.h"

#include <cstring>

#include "core/debug.h"
#include "core/psxemulator.h"
#include "core/r3000a.h"
#include "core/system.h"
#include "fmt/format.h"

namespace PCSX {

MemoryAccessTrace::MemoryAccessTrace() : m_listener(g_system->m_eventBus) {
    m_listener.listen<Events::ExecutionFlow::Reset>([this](const auto&) { stopAll(); });
}

MemoryAccessTrace::~MemoryAccessTrace() { stopAll(); }

MemoryAccessTrace::AccessMode MemoryAccessTrace::parseAccessMode(const std::string& mode) {
    if (mode == "read") return AccessMode::Read;
    if (mode == "write") return AccessMode::Write;
    if (mode == "both") return AccessMode::Both;
    throw std::runtime_error("mode must be read, write, or both");
}

std::string MemoryAccessTrace::resolveFunctionName(uint32_t pc) const {
    auto* cpu = g_emulator->m_cpu.get();
    if (auto* exact = cpu->getSymbolAt(pc)) {
        return *exact;
    }
    if (auto* sym = cpu->findContainingSymbol(pc)) {
        return sym->second;
    }
    return {};
}

uint64_t MemoryAccessTrace::entryKey(uint32_t pc, uint32_t callSite, Debug::BreakpointType access) {
    return (static_cast<uint64_t>(pc) << 34) | (static_cast<uint64_t>(callSite) << 2) |
           static_cast<uint64_t>(access);
}

void MemoryAccessTrace::record(Trace* trace, const Debug::Breakpoint* bp, const char* cause) {
    const auto& regs = g_emulator->m_cpu->m_regs.GPR.n;
    const uint32_t pc = g_emulator->m_cpu->m_regs.pc;
    const uint32_t callSite = regs.ra;
    const Debug::BreakpointType access = bp->type();
    const char* causeText = cause ? cause : "";

    if (trace->dedupeByPc) {
        const uint64_t key = entryKey(pc, callSite, access);
        auto it = trace->entryIndex.find(key);
        if (it != trace->entryIndex.end()) {
            if (trace->countHits) {
                trace->entries[it->second].hits++;
            }
            return;
        }
    }

    LogEntry entry;
    entry.pc = pc;
    entry.callSite = callSite;
    entry.access = access;
    entry.function = resolveFunctionName(pc);
    entry.callSiteFunction = resolveFunctionName(callSite);
    entry.cause = causeText;
    entry.hits = 1;

    if (trace->dedupeByPc) {
        const uint64_t key = entryKey(pc, callSite, access);
        trace->entryIndex[key] = trace->entries.size();
    }
    trace->entries.push_back(std::move(entry));
}

void MemoryAccessTrace::removeBreakpoints(Trace* trace) {
    for (const auto* bp : trace->breakpoints) {
        g_emulator->m_debug->removeBreakpoint(bp);
    }
    trace->breakpoints.clear();
}

MemoryAccessTrace::Trace* MemoryAccessTrace::findTrace(const std::string& idOrLabel) {
    for (auto& trace : m_traces) {
        if (trace.id == idOrLabel || trace.label == idOrLabel) {
            return &trace;
        }
    }
    return nullptr;
}

const MemoryAccessTrace::Trace* MemoryAccessTrace::findTrace(const std::string& idOrLabel) const {
    for (const auto& trace : m_traces) {
        if (trace.id == idOrLabel || trace.label == idOrLabel) {
            return &trace;
        }
    }
    return nullptr;
}

nlohmann::json MemoryAccessTrace::traceToJson(const Trace& trace, bool includeEntries) const {
    nlohmann::json j;
    j["id"] = trace.id;
    j["label"] = trace.label;
    j["address"] = trace.address;
    j["width"] = trace.width;
    j["mode"] = trace.mode == AccessMode::Read    ? "read"
                : trace.mode == AccessMode::Write ? "write"
                                                  : "both";
    j["dedupe_by_pc"] = trace.dedupeByPc;
    j["count_hits"] = trace.countHits;
    j["entry_count"] = trace.entries.size();
    if (includeEntries) {
        nlohmann::json entries = nlohmann::json::array();
        for (const auto& entry : trace.entries) {
            nlohmann::json e;
            e["pc"] = entry.pc;
            e["call_site"] = entry.callSite;
            e["access"] = entry.access == Debug::BreakpointType::Read ? "read" : "write";
            if (!entry.function.empty()) {
                e["function"] = entry.function;
            }
            if (!entry.callSiteFunction.empty()) {
                e["call_site_function"] = entry.callSiteFunction;
            }
            if (!entry.cause.empty()) {
                e["cause"] = entry.cause;
            }
            e["hits"] = entry.hits;
            entries.push_back(e);
        }
        j["entries"] = entries;
    }
    return j;
}

std::string MemoryAccessTrace::start(uint32_t address, unsigned width, AccessMode mode, const std::string& label,
                                     bool dedupeByPc, bool countHits) {
    if (width == 0) {
        throw std::runtime_error("width must be greater than zero");
    }
    if (!label.empty()) {
        if (auto* existing = findTrace(label)) {
            stopTrace(existing->id);
        }
    }

    Trace trace;
    trace.id = fmt::format("trace-{}", m_nextId++);
    trace.label = label.empty() ? trace.id : label;
    trace.address = address;
    trace.width = width;
    trace.mode = mode;
    trace.dedupeByPc = dedupeByPc;
    trace.countHits = countHits;

    m_traces.push_back(std::move(trace));
    Trace* active = &m_traces.back();

    auto invoker = [this, active](const Debug::Breakpoint* self, uint32_t, unsigned, const char* cause) {
        record(active, self, cause);
        return true;
    };

    const std::string source = "MemoryAccessTrace";
    if (mode == AccessMode::Read || mode == AccessMode::Both) {
        active->breakpoints.push_back(
            g_emulator->m_debug->addBreakpoint(address, Debug::BreakpointType::Read, width, source, "Read", invoker));
    }
    if (mode == AccessMode::Write || mode == AccessMode::Both) {
        active->breakpoints.push_back(
            g_emulator->m_debug->addBreakpoint(address, Debug::BreakpointType::Write, width, source, "Write", invoker));
    }

    return active->id;
}

nlohmann::json MemoryAccessTrace::listTraces() const {
    nlohmann::json traces = nlohmann::json::array();
    for (const auto& trace : m_traces) {
        traces.push_back(traceToJson(trace, false));
    }
    nlohmann::json j;
    j["count"] = traces.size();
    j["traces"] = traces;
    return j;
}

nlohmann::json MemoryAccessTrace::getTraceLog(const std::string& idOrLabel) const {
    if (idOrLabel.empty()) {
        nlohmann::json traces = nlohmann::json::array();
        for (const auto& trace : m_traces) {
            traces.push_back(traceToJson(trace, true));
        }
        nlohmann::json j;
        j["count"] = traces.size();
        j["traces"] = traces;
        return j;
    }
    const Trace* trace = findTrace(idOrLabel);
    if (!trace) {
        throw std::runtime_error(fmt::format("memory access trace not found: {}", idOrLabel));
    }
    return traceToJson(*trace, true);
}

bool MemoryAccessTrace::clearTraceLog(const std::string& idOrLabel) {
    Trace* trace = findTrace(idOrLabel);
    if (!trace) {
        return false;
    }
    trace->entries.clear();
    trace->entryIndex.clear();
    return true;
}

bool MemoryAccessTrace::stopTrace(const std::string& idOrLabel) {
    for (auto it = m_traces.begin(); it != m_traces.end(); ++it) {
        if (it->id == idOrLabel || it->label == idOrLabel) {
            removeBreakpoints(&*it);
            m_traces.erase(it);
            return true;
        }
    }
    return false;
}

void MemoryAccessTrace::stopAll() {
    for (auto& trace : m_traces) {
        removeBreakpoints(&trace);
    }
    m_traces.clear();
}

}  // namespace PCSX
