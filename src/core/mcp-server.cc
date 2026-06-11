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

#include "core/mcp-server.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "core/pad.h"

#include "core/debug.h"
#include "core/memory-access-trace.h"
#include "core/movie.h"
#include "core/psxemulator.h"
#include "core/psxmem.h"
#include "core/r3000a.h"
#include "core/system.h"
#include "fmt/format.h"
#include "json.hpp"
#include "support/hashtable.h"
#include "support/slice.h"
#include "support/strings-helpers.h"

namespace {

using PCSX::g_emulator;
using PCSX::g_system;

constexpr size_t kBufferSize = 65536;
constexpr size_t kMaxMemoryIO = 4096;

nlohmann::json mcpTextResult(const std::string& text) {
    return nlohmann::json{{"content", nlohmann::json::array({{{"type", "text"}, {"text", text}}})}};
}

nlohmann::json mcpTextResult(const nlohmann::json& j) { return mcpTextResult(j.dump(2)); }

bool debuggerEnabled() {
    return g_emulator->settings.get<PCSX::Emulator::SettingDebugSettings>()
        .get<PCSX::Emulator::DebugSettings::Debug>()
        .value;
}

nlohmann::json buildEmulationStatusJson() {
    auto& debugSettings = g_emulator->settings.get<PCSX::Emulator::SettingDebugSettings>();
    auto* movie = g_emulator->m_movie.get();
    nlohmann::json j;
    j["running"] = g_system->running();
    j["pc"] = g_emulator->m_cpu->m_regs.pc;
    j["debugger"] = debugSettings.get<PCSX::Emulator::DebugSettings::Debug>().value;
    j["dynarec"] = g_emulator->m_cpu->isDynarec();
    j["ram8mb"] = g_emulator->settings.get<PCSX::Emulator::Setting8MB>().value;
    j["pending_frame_advances"] = movie->getPendingFrameAdvances();
    switch (movie->getMode()) {
        case PCSX::MovieManager::Mode::Idle:
            j["movie_mode"] = "idle";
            break;
        case PCSX::MovieManager::Mode::Recording:
            j["movie_mode"] = "recording";
            break;
        case PCSX::MovieManager::Mode::Playing:
            j["movie_mode"] = "playing";
            break;
    }
    j["movie_frame"] = movie->getFrameIndex();
    j["movie_frame_count"] = movie->getFrameCount();
    if (!movie->getPath().empty()) {
        j["movie_path"] = movie->getPath().string();
    }
    auto* cpu = g_emulator->m_cpu.get();
    if (auto* sym = cpu->findContainingSymbol(cpu->m_regs.pc)) {
        j["pc_symbol"] = sym->second;
        j["pc_symbol_address"] = sym->first;
        j["pc_symbol_offset"] = cpu->m_regs.pc - sym->first;
    }
    return j;
}

nlohmann::json buildRegistersJson() {
    const auto& regs = g_emulator->m_cpu->m_regs;
    static const char* gprNames[] = {"zero", "at",  "v0",  "v1",  "a0",  "a1",  "a2",  "a3",  "t0",  "t1",  "t2",
                                     "t3",   "t4",  "t5",  "t6",  "t7",  "s0",  "s1",  "s2",  "s3",  "s4",  "s5",
                                     "s6",   "s7",  "t8",  "t9",  "k0",  "k1",  "gp",  "sp",  "s8",  "ra"};
    nlohmann::json j;
    j["pc"] = regs.pc;
    j["hi"] = regs.GPR.n.hi;
    j["lo"] = regs.GPR.n.lo;
    nlohmann::json gpr = nlohmann::json::object();
    for (int i = 0; i < 32; ++i) {
        gpr[gprNames[i]] = regs.GPR.r[i];
    }
    j["gpr"] = gpr;
    return j;
}

nlohmann::json toolGetEmulationStatus() { return mcpTextResult(buildEmulationStatusJson()); }

nlohmann::json toolGetRegisters() { return mcpTextResult(buildRegistersJson()); }

uint32_t parseAddress(const nlohmann::json& args, const char* key) {
    if (!args.contains(key)) {
        throw std::runtime_error(fmt::format("missing required argument '{}'", key));
    }
    const auto& v = args.at(key);
    if (v.is_number_unsigned()) return v.get<uint32_t>();
    if (v.is_number_integer()) return static_cast<uint32_t>(v.get<int64_t>());
    if (v.is_string()) {
        std::string s = v.get<std::string>();
        if (PCSX::StringsHelpers::startsWith(s, "0x") || PCSX::StringsHelpers::startsWith(s, "0X")) {
            return std::stoul(s, nullptr, 16);
        }
        return std::stoul(s, nullptr, 0);
    }
    throw std::runtime_error(fmt::format("invalid address argument '{}'", key));
}

nlohmann::json buildMemoryJson(uint32_t address, size_t size) {
    if (size == 0 || size > kMaxMemoryIO) {
        throw std::runtime_error(fmt::format("size must be 1..{}", kMaxMemoryIO));
    }
    auto* mem = g_emulator->m_mem.get();
    nlohmann::json bytes = nlohmann::json::array();
    for (size_t i = 0; i < size; ++i) {
        bytes.push_back(mem->read8(address + static_cast<uint32_t>(i)));
    }
    nlohmann::json j;
    j["address"] = address;
    j["size"] = size;
    j["bytes"] = bytes;
    std::string hex;
    for (size_t i = 0; i < size; ++i) {
        hex += fmt::format("{:02x}", bytes[i].get<uint8_t>());
    }
    j["hex"] = hex;
    if (size >= 4) {
        j["u32"] = mem->read32(address);
    }
    return j;
}

nlohmann::json toolReadMemory(const nlohmann::json& args) {
    uint32_t address = parseAddress(args, "address");
    size_t size = 4;
    if (args.contains("size")) {
        size = args.at("size").get<size_t>();
    }
    return mcpTextResult(buildMemoryJson(address, size));
}

nlohmann::json toolWriteMemory(const nlohmann::json& args) {
    uint32_t address = parseAddress(args, "address");
    if (!args.contains("bytes") && !args.contains("hex")) {
        throw std::runtime_error("write_memory requires 'bytes' array or 'hex' string");
    }
    std::vector<uint8_t> data;
    if (args.contains("bytes")) {
        for (const auto& b : args.at("bytes")) {
            data.push_back(static_cast<uint8_t>(b.get<uint32_t>() & 0xff));
        }
    } else {
        std::string hex = args.at("hex").get<std::string>();
        hex.erase(std::remove_if(hex.begin(), hex.end(), [](unsigned char c) { return std::isspace(c); }), hex.end());
        if (hex.size() % 2 != 0) throw std::runtime_error("hex string must have even length");
        for (size_t i = 0; i < hex.size(); i += 2) {
            data.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
        }
    }
    if (data.empty() || data.size() > kMaxMemoryIO) {
        throw std::runtime_error(fmt::format("write size must be 1..{}", kMaxMemoryIO));
    }
    auto* mem = g_emulator->m_mem.get();
    for (size_t i = 0; i < data.size(); ++i) {
        mem->write8(address + static_cast<uint32_t>(i), data[i]);
    }
    nlohmann::json j;
    j["address"] = address;
    j["size"] = data.size();
    j["ok"] = true;
    return mcpTextResult(j);
}

nlohmann::json toolAdvanceFrames(const nlohmann::json& args) {
    unsigned count = 1;
    if (args.contains("count")) {
        count = args.at("count").get<unsigned>();
    }
    if (count == 0) {
        throw std::runtime_error("count must be at least 1");
    }
    g_emulator->m_movie->advanceFrames(count);
    nlohmann::json j;
    j["frames_requested"] = count;
    j["pending"] = g_emulator->m_movie->getPendingFrameAdvances();
    j["running"] = g_system->running();
    if (g_emulator->m_movie->getMode() == PCSX::MovieManager::Mode::Playing) {
        j["movie_frame"] = g_emulator->m_movie->getFrameIndex();
    }
    return mcpTextResult(j);
}

nlohmann::json toolEmulationControl(const nlohmann::json& args) {
    if (!args.contains("action")) throw std::runtime_error("missing required argument 'action'");
    std::string action = args.at("action").get<std::string>();
    if (action == "pause") {
        g_system->pause();
    } else if (action == "resume" || action == "start") {
        g_system->resume();
    } else if (action == "soft_reset") {
        g_system->softReset();
    } else if (action == "hard_reset") {
        g_system->hardReset();
    } else {
        throw std::runtime_error("action must be pause, resume, start, soft_reset, or hard_reset");
    }
    return mcpTextResult(fmt::format(R"({{"action":"{}","running":{}}})", action, g_system->running()));
}

PCSX::Debug::BreakpointType parseBreakpointType(const std::string& type) {
    if (type == "Exec") return PCSX::Debug::BreakpointType::Exec;
    if (type == "Read") return PCSX::Debug::BreakpointType::Read;
    if (type == "Write") return PCSX::Debug::BreakpointType::Write;
    throw std::runtime_error("type must be Exec, Read, or Write");
}

nlohmann::json breakpointToJson(const PCSX::Debug::Breakpoint& bp, int index) {
    nlohmann::json j;
    j["id"] = fmt::format("{:p}", static_cast<const void*>(&bp));
    j["index"] = index;
    j["address"] = bp.address() | bp.base();
    j["type"] = bp.type() == PCSX::Debug::BreakpointType::Exec   ? "Exec"
                : bp.type() == PCSX::Debug::BreakpointType::Read ? "Read"
                                                                 : "Write";
    j["width"] = bp.width();
    j["enabled"] = bp.enabled();
    j["label"] = bp.label();
    j["source"] = bp.source();
    return j;
}

nlohmann::json toolListBreakpoints() {
    if (!debuggerEnabled()) throw std::runtime_error("Enable the debugger in PCSX settings first");
    nlohmann::json list = nlohmann::json::array();
    auto& tree = g_emulator->m_debug->getTree();
    int index = 0;
    for (auto bp = tree.begin(); bp != tree.end(); ++bp, ++index) {
        list.push_back(breakpointToJson(*bp, index));
    }
    return mcpTextResult(list);
}

nlohmann::json toolAddBreakpoint(const nlohmann::json& args) {
    if (!debuggerEnabled()) throw std::runtime_error("Enable the debugger in PCSX settings first");
    uint32_t address = parseAddress(args, "address");
    std::string type = args.contains("type") ? args.at("type").get<std::string>() : "Exec";
    unsigned width = args.contains("width") ? args.at("width").get<unsigned>() : 4;
    std::string label = args.contains("label") ? args.at("label").get<std::string>() : "MCP";
    std::string source = "MCP";
    auto* bp = g_emulator->m_debug->addBreakpoint(address, parseBreakpointType(type), width, source, label);
    return mcpTextResult(breakpointToJson(*bp, -1));
}

nlohmann::json toolRemoveBreakpoint(const nlohmann::json& args) {
    if (!debuggerEnabled()) throw std::runtime_error("Enable the debugger in PCSX settings first");
    if (!args.contains("id")) throw std::runtime_error("missing required argument 'id'");
    std::string id = args.at("id").get<std::string>();
    auto* ptr = reinterpret_cast<PCSX::Debug::Breakpoint*>(std::stoull(id, nullptr, 0));
    bool found = false;
    auto& tree = g_emulator->m_debug->getTree();
    for (auto bp = tree.begin(); bp != tree.end(); ++bp) {
        if (&*bp == ptr) {
            found = true;
            break;
        }
    }
    if (!found) throw std::runtime_error("breakpoint id not found");
    g_emulator->m_debug->removeBreakpoint(ptr);
    nlohmann::json j;
    j["removed"] = id;
    j["ok"] = true;
    return mcpTextResult(j);
}

nlohmann::json portInputToJson(const PCSX::PadInputState& port) {
    static const char* buttonNames[] = {"SELECT", "START",  "UP",       "RIGHT",    "DOWN",      "LEFT",
                                        "L2",     "R2",     "L1",       "R1",       "TRIANGLE",  "CIRCLE",
                                        "CROSS",  "SQUARE"};
    static const unsigned buttonBits[] = {0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    nlohmann::json j = nlohmann::json::object();
    for (size_t i = 0; i < sizeof(buttonNames) / sizeof(buttonNames[0]); ++i) {
        j[buttonNames[i]] = (port.buttonStatus & (1 << buttonBits[i])) == 0;
    }
    j["leftX"] = port.leftJoyX;
    j["leftY"] = port.leftJoyY;
    j["rightX"] = port.rightJoyX;
    j["rightY"] = port.rightJoyY;
    return j;
}

nlohmann::json buildMovieStatusJson() {
    auto* movie = g_emulator->m_movie.get();
    nlohmann::json j;
    switch (movie->getMode()) {
        case PCSX::MovieManager::Mode::Idle:
            j["mode"] = "idle";
            break;
        case PCSX::MovieManager::Mode::Recording:
            j["mode"] = "recording";
            break;
        case PCSX::MovieManager::Mode::Playing:
            j["mode"] = "playing";
            break;
    }
    j["frame"] = movie->getFrameIndex();
    j["frame_count"] = movie->getFrameCount();
    j["has_starting_savestate"] = movie->hasStartingSaveState();
    if (!movie->getPath().empty()) {
        j["path"] = movie->getPath().string();
    }
    return j;
}

nlohmann::json toolMovieStatus() { return mcpTextResult(buildMovieStatusJson()); }

nlohmann::json toolMoviePlay(const nlohmann::json& args) {
    auto* movie = g_emulator->m_movie.get();
    if (args.contains("path")) {
        if (!movie->load(args.at("path").get<std::string>())) {
            throw std::runtime_error("failed to load movie file");
        }
    }
    if (!movie->startPlaying()) {
        throw std::runtime_error("failed to start movie playback");
    }
    nlohmann::json j = buildMovieStatusJson();
    j["ok"] = true;
    return mcpTextResult(j);
}

nlohmann::json toolMovieStop(const nlohmann::json& args) {
    bool pauseAfter = !args.contains("pause") || args.at("pause").get<bool>();
    g_emulator->m_movie->stop(pauseAfter);
    nlohmann::json j = buildMovieStatusJson();
    j["ok"] = true;
    return mcpTextResult(j);
}

nlohmann::json toolMovieLoad(const nlohmann::json& args) {
    if (!args.contains("path")) throw std::runtime_error("missing required argument 'path'");
    if (!g_emulator->m_movie->load(args.at("path").get<std::string>())) {
        throw std::runtime_error("failed to load movie file");
    }
    nlohmann::json j = buildMovieStatusJson();
    j["ok"] = true;
    return mcpTextResult(j);
}

nlohmann::json toolMovieGetInput(const nlohmann::json& args) {
    if (!args.contains("frame")) throw std::runtime_error("missing required argument 'frame'");
    const uint64_t frame = args.at("frame").get<uint64_t>();
    const PCSX::MovieManager::Frame* input = g_emulator->m_movie->getFrameInput(frame);
    if (!input) {
        throw std::runtime_error("frame index out of range");
    }
    if (args.contains("port")) {
        const int port = args.at("port").get<int>();
        if (port == 1) return mcpTextResult(portInputToJson(input->port1));
        if (port == 2) return mcpTextResult(portInputToJson(input->port2));
        throw std::runtime_error("port must be 1 or 2");
    }
    nlohmann::json j;
    j["frame"] = frame;
    j["port1"] = portInputToJson(input->port1);
    j["port2"] = portInputToJson(input->port2);
    return mcpTextResult(j);
}

nlohmann::json buildPcContextJson(uint32_t address) {
    auto* cpu = g_emulator->m_cpu.get();
    nlohmann::json j;
    j["address"] = address;
    if (auto* exact = cpu->getSymbolAt(address)) {
        j["exact_symbol"] = *exact;
    }
    if (auto* sym = cpu->findContainingSymbol(address)) {
        j["symbol"] = sym->second;
        j["symbol_address"] = sym->first;
        j["offset"] = address - sym->first;
    }
    return j;
}

nlohmann::json toolGetPcContext(const nlohmann::json& args) {
    uint32_t address = g_emulator->m_cpu->m_regs.pc;
    if (args.contains("address")) {
        address = parseAddress(args, "address");
    }
    return mcpTextResult(buildPcContextJson(address));
}

nlohmann::json toolResolveSymbol(const nlohmann::json& args) {
    if (!args.contains("name")) throw std::runtime_error("missing required argument 'name'");
    std::string name = args.at("name").get<std::string>();
    bool exactOnly = args.contains("exact") && args.at("exact").get<bool>();
    auto& symbols = g_emulator->m_cpu->m_symbols;
    nlohmann::json matches = nlohmann::json::array();
    for (const auto& [addr, symName] : symbols) {
        if (symName == name) {
            nlohmann::json m;
            m["name"] = symName;
            m["address"] = addr;
            matches.push_back(m);
        } else if (!exactOnly && symName.find(name) != std::string::npos) {
            nlohmann::json m;
            m["name"] = symName;
            m["address"] = addr;
            matches.push_back(m);
        }
    }
    if (matches.empty()) {
        throw std::runtime_error(fmt::format("no symbol match for '{}'", name));
    }
    nlohmann::json j;
    j["query"] = name;
    j["matches"] = matches;
    return mcpTextResult(j);
}

nlohmann::json toolListSymbols(const nlohmann::json& args) {
    size_t limit = 200;
    if (args.contains("limit")) {
        limit = args.at("limit").get<size_t>();
    }
    std::string prefix;
    if (args.contains("prefix")) {
        prefix = args.at("prefix").get<std::string>();
    }
    nlohmann::json symbols = nlohmann::json::array();
    for (const auto& [addr, name] : g_emulator->m_cpu->m_symbols) {
        if (!prefix.empty() && name.rfind(prefix, 0) != 0) continue;
        symbols.push_back({{"name", name}, {"address", addr}});
        if (symbols.size() >= limit) break;
    }
    nlohmann::json j;
    j["count"] = symbols.size();
    j["symbols"] = symbols;
    return mcpTextResult(j);
}

nlohmann::json toolStartMemoryAccessTrace(const nlohmann::json& args) {
    if (!debuggerEnabled()) throw std::runtime_error("Enable the debugger in PCSX settings first");
    uint32_t address = parseAddress(args, "address");
    unsigned width = args.contains("width") ? args.at("width").get<unsigned>() : 4;
    std::string mode = args.contains("mode") ? args.at("mode").get<std::string>() : "write";
    std::string label = args.contains("label") ? args.at("label").get<std::string>() : "";
    bool dedupeByPc = !args.contains("dedupe_by_pc") || args.at("dedupe_by_pc").get<bool>();
    bool countHits = !args.contains("count_hits") || args.at("count_hits").get<bool>();
    const auto accessMode = PCSX::MemoryAccessTrace::parseAccessMode(mode);
    const std::string id =
        g_emulator->m_memoryAccessTrace->start(address, width, accessMode, label, dedupeByPc, countHits);
    nlohmann::json j = g_emulator->m_memoryAccessTrace->getTraceLog(id);
    j["ok"] = true;
    return mcpTextResult(j);
}

nlohmann::json toolListMemoryAccessTraces() {
    if (!debuggerEnabled()) throw std::runtime_error("Enable the debugger in PCSX settings first");
    return mcpTextResult(g_emulator->m_memoryAccessTrace->listTraces());
}

nlohmann::json toolGetMemoryAccessTrace(const nlohmann::json& args) {
    if (!debuggerEnabled()) throw std::runtime_error("Enable the debugger in PCSX settings first");
    std::string idOrLabel;
    if (args.contains("id")) {
        idOrLabel = args.at("id").get<std::string>();
    } else if (args.contains("label")) {
        idOrLabel = args.at("label").get<std::string>();
    }
    return mcpTextResult(g_emulator->m_memoryAccessTrace->getTraceLog(idOrLabel));
}

nlohmann::json toolClearMemoryAccessTrace(const nlohmann::json& args) {
    if (!debuggerEnabled()) throw std::runtime_error("Enable the debugger in PCSX settings first");
    if (!args.contains("id") && !args.contains("label")) {
        throw std::runtime_error("missing required argument 'id' or 'label'");
    }
    const std::string idOrLabel =
        args.contains("id") ? args.at("id").get<std::string>() : args.at("label").get<std::string>();
    if (!g_emulator->m_memoryAccessTrace->clearTraceLog(idOrLabel)) {
        throw std::runtime_error(fmt::format("memory access trace not found: {}", idOrLabel));
    }
    nlohmann::json j;
    j["ok"] = true;
    j["cleared"] = idOrLabel;
    return mcpTextResult(j);
}

nlohmann::json toolStopMemoryAccessTrace(const nlohmann::json& args) {
    if (!debuggerEnabled()) throw std::runtime_error("Enable the debugger in PCSX settings first");
    if (!args.contains("id") && !args.contains("label")) {
        throw std::runtime_error("missing required argument 'id' or 'label'");
    }
    const std::string idOrLabel =
        args.contains("id") ? args.at("id").get<std::string>() : args.at("label").get<std::string>();
    if (!g_emulator->m_memoryAccessTrace->stopTrace(idOrLabel)) {
        throw std::runtime_error(fmt::format("memory access trace not found: {}", idOrLabel));
    }
    nlohmann::json j;
    j["ok"] = true;
    j["stopped"] = idOrLabel;
    return mcpTextResult(j);
}

nlohmann::json listResources() {
    nlohmann::json resources = nlohmann::json::array();
    resources.push_back({{"uri", "pcsx://status"},
                         {"name", "emulation_status"},
                         {"description", "Current emulator run state, PC, movie state, and nearest symbol."},
                         {"mimeType", "application/json"}});
    resources.push_back({{"uri", "pcsx://registers"},
                         {"name", "cpu_registers"},
                         {"description", "MIPS GPRs, PC, HI, and LO."},
                         {"mimeType", "application/json"}});
    resources.push_back({{"uriTemplate", "pcsx://memory/{address}"},
                         {"name", "memory"},
                         {"description",
                          "Read emulated memory at a PSX virtual address. Optional query: ?size=N (default 4, max "
                          "4096). Address is hex with or without 0x prefix."},
                         {"mimeType", "application/json"}});
    return resources;
}

nlohmann::json readResource(const std::string& uri) {
    if (uri == "pcsx://status") {
        return {{"contents", {{{"uri", uri}, {"mimeType", "application/json"}, {"text", buildEmulationStatusJson().dump(2)}}}}};
    }
    if (uri == "pcsx://registers") {
        return {{"contents", {{{"uri", uri}, {"mimeType", "application/json"}, {"text", buildRegistersJson().dump(2)}}}}};
    }
    constexpr std::string_view kMemoryPrefix = "pcsx://memory/";
    if (uri.rfind(kMemoryPrefix, 0) == 0) {
        std::string rest = uri.substr(kMemoryPrefix.size());
        size_t size = 4;
        auto qpos = rest.find('?');
        std::string addrPart = qpos == std::string::npos ? rest : rest.substr(0, qpos);
        if (qpos != std::string::npos) {
            std::string query = rest.substr(qpos + 1);
            if (query.rfind("size=", 0) == 0) {
                size = std::stoul(query.substr(5));
            }
        }
        uint32_t address = std::stoul(addrPart, nullptr, 0);
        std::string text = buildMemoryJson(address, size).dump(2);
        return {{"contents", {{{"uri", uri}, {"mimeType", "application/json"}, {"text", text}}}}};
    }
    throw std::runtime_error(fmt::format("unknown resource uri '{}'", uri));
}

nlohmann::json listTools() {
    auto schema = [](const nlohmann::json& properties, const nlohmann::json& required = nlohmann::json::array()) {
        nlohmann::json s;
        s["type"] = "object";
        s["properties"] = properties;
        if (!required.empty()) s["required"] = required;
        return s;
    };
    nlohmann::json tools = nlohmann::json::array();
    tools.push_back({{"name", "get_emulation_status"},
                     {"description", "Get emulator run state, PC, debugger and dynarec flags."},
                     {"inputSchema", schema(nlohmann::json::object())}});
    tools.push_back({{"name", "get_registers"},
                     {"description", "Read MIPS GPRs, PC, HI, and LO."},
                     {"inputSchema", schema(nlohmann::json::object())}});
    tools.push_back({{"name", "read_memory"},
                     {"description", "Read emulated memory at a PSX virtual address (e.g. 0x8006d144)."},
                     {"inputSchema",
                      schema({{"address", {{"description", "PSX virtual address"}, {"type", "integer"}}},
                              {"size", {{"description", "Byte count (default 4, max 4096)"}, {"type", "integer"}}}},
                             nlohmann::json::array({"address"}))}});
    tools.push_back({{"name", "write_memory"},
                     {"description", "Write bytes to emulated memory at a PSX virtual address."},
                     {"inputSchema",
                      schema({{"address", {{"type", "integer"}}},
                              {"bytes", {{"type", "array"}, {"items", {{"type", "integer"}}}}},
                              {"hex", {{"type", "string"}, {"description", "Hex string alternative to bytes"}}}},
                             nlohmann::json::array({"address"}))}});
    tools.push_back({{"name", "advance_frames"},
                     {"description",
                      "Run emulation until the next GPU vsync, repeated count times, then pause. Works during normal "
                      "play and movie playback."},
                     {"inputSchema",
                      schema({{"count", {{"type", "integer"}, {"description", "Frames to advance (default 1)"},
                                          {"minimum", 1}}}})}});
    tools.push_back(
        {{"name", "emulation_control"},
         {"description", "Pause, resume, or reset the emulator."},
         {"inputSchema",
          schema({{"action",
                   {{"type", "string"},
                    {"enum", nlohmann::json::array({"pause", "resume", "start", "soft_reset", "hard_reset"})}}}},
                 nlohmann::json::array({"action"}))}});
    tools.push_back({{"name", "list_breakpoints"},
                     {"description", "List software breakpoints. Requires debugger enabled (interpreter)."},
                     {"inputSchema", schema(nlohmann::json::object())}});
    tools.push_back({{"name", "add_breakpoint"},
                     {"description", "Add a software breakpoint. Requires debugger enabled (interpreter)."},
                     {"inputSchema",
                      schema({{"address", {{"type", "integer"}}},
                              {"type", {{"type", "string"}, {"enum", nlohmann::json::array({"Exec", "Read", "Write"})}}},
                              {"width", {{"type", "integer"}}},
                              {"label", {{"type", "string"}}}},
                             nlohmann::json::array({"address"}))}});
    tools.push_back({{"name", "remove_breakpoint"},
                     {"description", "Remove a breakpoint by id from list_breakpoints."},
                     {"inputSchema",
                      schema({{"id", {{"type", "string"}, {"description", "Breakpoint id from list_breakpoints"}}}},
                             nlohmann::json::array({"id"}))}});
    tools.push_back({{"name", "movie_status"},
                     {"description", "Get movie mode, frame index, frame count, and loaded path."},
                     {"inputSchema", schema(nlohmann::json::object())}});
    tools.push_back({{"name", "movie_play"},
                     {"description", "Load optional path then start movie playback from embedded savestate."},
                     {"inputSchema", schema({{"path", {{"type", "string"}, {"description", "Optional .pcsxmv path"}}}})}});
    tools.push_back({{"name", "movie_stop"},
                     {"description", "Stop movie recording/playback."},
                     {"inputSchema", schema({{"pause", {{"type", "boolean"}, {"description", "Pause after stop (default true)"}}}})}});
    tools.push_back({{"name", "movie_load"},
                     {"description", "Load a .pcsxmv file without starting playback."},
                     {"inputSchema", schema({{"path", {{"type", "string"}}}}, nlohmann::json::array({"path"}))}});
    tools.push_back({{"name", "movie_get_input"},
                     {"description", "Get recorded input for a movie frame."},
                     {"inputSchema",
                      schema({{"frame", {{"type", "integer"}}},
                              {"port", {{"type", "integer"}, {"description", "Optional port 1 or 2"}}}},
                             nlohmann::json::array({"frame"}))}});
    tools.push_back({{"name", "get_pc_context"},
                     {"description", "Resolve Ghidra/Redux symbol info for an address (default: current PC)."},
                     {"inputSchema", schema({{"address", {{"type", "integer"}, {"description", "PSX virtual address"}}}})}});
    tools.push_back({{"name", "resolve_symbol"},
                     {"description", "Find symbol address(es) by name (Ghidra/Redux symbol table)."},
                     {"inputSchema",
                      schema({{"name", {{"type", "string"}}},
                              {"exact", {{"type", "boolean"}, {"description", "Exact match only (default false)"}}}},
                             nlohmann::json::array({"name"}))}});
    tools.push_back({{"name", "list_symbols"},
                     {"description", "List loaded symbols, optionally filtered by name prefix."},
                     {"inputSchema",
                      schema({{"prefix", {{"type", "string"}}}, {"limit", {{"type", "integer"}, {"maximum", 1000}}}})}});
    tools.push_back({{"name", "start_memory_access_trace"},
                     {"description",
                      "Arm a non-pausing read/write memory access trace (like Typed Debugger log mode). Requires "
                      "debugger enabled (interpreter)."},
                     {"inputSchema",
                      schema({{"address", {{"type", "integer"}}},
                              {"width", {{"type", "integer"}, {"description", "Watch width in bytes (default 4)"}}},
                              {"mode",
                               {{"type", "string"},
                                {"enum", nlohmann::json::array({"read", "write", "both"})},
                                {"description", "Access type to log (default write)"}}},
                              {"label", {{"type", "string"}, {"description", "Optional stable name; replaces same label"}}},
                              {"dedupe_by_pc",
                               {{"type", "boolean"}, {"description", "Log each store PC once (default true)"}}},
                              {"count_hits",
                               {{"type", "boolean"},
                                {"description", "Increment hit count for duplicate PCs (default true)"}}}},
                             nlohmann::json::array({"address"}))}});
    tools.push_back({{"name", "list_memory_access_traces"},
                     {"description", "List active memory access traces without log entries."},
                     {"inputSchema", schema(nlohmann::json::object())}});
    tools.push_back(
        {{"name", "get_memory_access_trace"},
         {"description", "Get trace metadata and logged access entries. Omit id/label to return all traces."},
         {"inputSchema",
          schema({{"id", {{"type", "string"}, {"description", "Trace id from start_memory_access_trace"}}},
                  {"label", {{"type", "string"}, {"description", "Trace label if set"}}}})}});
    tools.push_back({{"name", "clear_memory_access_trace"},
                     {"description", "Clear logged entries for a trace but keep it armed."},
                     {"inputSchema",
                      schema({{"id", {{"type", "string"}}}, {"label", {{"type", "string"}}}},
                             nlohmann::json::array())}});
    tools.push_back({{"name", "stop_memory_access_trace"},
                     {"description", "Remove a trace and its watch breakpoints."},
                     {"inputSchema",
                      schema({{"id", {{"type", "string"}}}, {"label", {{"type", "string"}}}},
                             nlohmann::json::array())}});
    return tools;
}

nlohmann::json callTool(const std::string& name, const nlohmann::json& args) {
    if (name == "get_emulation_status") return toolGetEmulationStatus();
    if (name == "get_registers") return toolGetRegisters();
    if (name == "read_memory") return toolReadMemory(args);
    if (name == "write_memory") return toolWriteMemory(args);
    if (name == "advance_frames") return toolAdvanceFrames(args);
    if (name == "emulation_control") return toolEmulationControl(args);
    if (name == "list_breakpoints") return toolListBreakpoints();
    if (name == "add_breakpoint") return toolAddBreakpoint(args);
    if (name == "remove_breakpoint") return toolRemoveBreakpoint(args);
    if (name == "movie_status") return toolMovieStatus();
    if (name == "movie_play") return toolMoviePlay(args);
    if (name == "movie_stop") return toolMovieStop(args);
    if (name == "movie_load") return toolMovieLoad(args);
    if (name == "movie_get_input") return toolMovieGetInput(args);
    if (name == "get_pc_context") return toolGetPcContext(args);
    if (name == "resolve_symbol") return toolResolveSymbol(args);
    if (name == "list_symbols") return toolListSymbols(args);
    if (name == "start_memory_access_trace") return toolStartMemoryAccessTrace(args);
    if (name == "list_memory_access_traces") return toolListMemoryAccessTraces();
    if (name == "get_memory_access_trace") return toolGetMemoryAccessTrace(args);
    if (name == "clear_memory_access_trace") return toolClearMemoryAccessTrace(args);
    if (name == "stop_memory_access_trace") return toolStopMemoryAccessTrace(args);
    throw std::runtime_error(fmt::format("unknown tool '{}'", name));
}

std::optional<nlohmann::json> handleMcpRequest(const nlohmann::json& req) {
    if (!req.contains("method")) return std::nullopt;
    std::string method = req.at("method").get<std::string>();
    const bool hasId = req.contains("id") && !req.at("id").is_null();

    if (method == "notifications/initialized" || method == "initialized") {
        return std::nullopt;
    }

    nlohmann::json response;
    response["jsonrpc"] = "2.0";
    if (hasId) response["id"] = req.at("id");

    try {
        if (method == "initialize") {
            nlohmann::json result;
            result["protocolVersion"] = "2024-11-05";
            result["capabilities"] = {{"tools", nlohmann::json::object()}, {"resources", nlohmann::json::object()}};
            result["serverInfo"] = {{"name", "pcsx-redux"}, {"version", "1.0.0"}};
            response["result"] = result;
        } else if (method == "tools/list") {
            response["result"] = {{"tools", listTools()}};
        } else if (method == "resources/list") {
            response["result"] = {{"resources", listResources()}};
        } else if (method == "resources/read") {
            const auto& params = req.at("params");
            std::string uri = params.at("uri").get<std::string>();
            response["result"] = readResource(uri);
        } else if (method == "tools/call") {
            const auto& params = req.at("params");
            std::string toolName = params.at("name").get<std::string>();
            nlohmann::json toolArgs = params.contains("arguments") ? params.at("arguments") : nlohmann::json::object();
            nlohmann::json toolResult = callTool(toolName, toolArgs);
            response["result"] = toolResult;
        } else if (method == "ping") {
            response["result"] = nlohmann::json::object();
        } else {
            if (!hasId) return std::nullopt;
            response["error"] = {{"code", -32601}, {"message", fmt::format("Method not found: {}", method)}};
        }
    } catch (const std::exception& e) {
        if (!hasId) return std::nullopt;
        response["error"] = {{"code", -32000}, {"message", e.what()}};
    }

    if (!hasId) return std::nullopt;
    return response;
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

}  // namespace

namespace PCSX {

class McpServer::McpClient : public Intrusive::List<McpClient>::Node {
  public:
    void shutdown();

    explicit McpClient(McpServer* server) : m_server(server) {
        uv_tcp_init(server->m_loop, &m_tcp);
        m_tcp.data = this;
    }

    bool accept(uv_tcp_t* srv) {
        if (m_status != CLOSED) return false;
        if (uv_accept(reinterpret_cast<uv_stream_t*>(srv), reinterpret_cast<uv_stream_t*>(&m_tcp)) != 0) {
            return false;
        }
        m_status = OPEN;
        m_buffer.clear();
        uv_read_start(
            reinterpret_cast<uv_stream_t*>(&m_tcp),
            [](uv_handle_t* handle, size_t, uv_buf_t* buf) {
                auto* self = static_cast<McpClient*>(handle->data);
                buf->base = self->m_readBuf;
                buf->len = sizeof(self->m_readBuf);
            },
            [](uv_stream_t* stream, ssize_t nread, const uv_buf_t*) {
                auto* self = static_cast<McpClient*>(stream->data);
                self->onRead(nread);
            });
        return true;
    }

  private:
    struct WriteRequest : public Intrusive::HashTable<uintptr_t, WriteRequest>::Node {
        Slice m_slice;
        uv_write_t m_req;
        uv_buf_t m_buf;
        void enqueue(McpClient* client) {
            m_buf.base = static_cast<char*>(const_cast<void*>(m_slice.data()));
            m_buf.len = m_slice.size();
            client->m_writes.insert(reinterpret_cast<uintptr_t>(&m_req), this);
            uv_write(&m_req, reinterpret_cast<uv_stream_t*>(&client->m_tcp), &m_buf, 1,
                     [](uv_write_t* req, int) {
                         auto* client = static_cast<McpClient*>(req->handle->data);
                         auto it = client->m_writes.find(reinterpret_cast<uintptr_t>(req));
                         delete &*it;
                     });
        }
    };

    void write(std::string msg) {
        auto* req = new WriteRequest();
        req->m_slice.copy(std::move(msg));
        req->enqueue(this);
    }

    void onRead(ssize_t nread) {
        if (nread <= 0) {
            shutdown();
            return;
        }
        m_buffer.append(m_readBuf, m_readBuf + nread);
        if (tryHandleRequest()) {
            shutdown();
        } else if (m_buffer.size() > kBufferSize * 2) {
            shutdown();
        }
    }

    bool tryHandleRequest() {
        auto headerEnd = m_buffer.find("\r\n\r\n");
        if (headerEnd == std::string::npos) return false;

        std::string headers = m_buffer.substr(0, headerEnd);
        size_t bodyStart = headerEnd + 4;
        std::string method;
        std::string path;
        size_t contentLength = 0;

        std::istringstream headerStream(headers);
        std::string line;
        if (!std::getline(headerStream, line)) return true;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        {
            std::istringstream requestLine(line);
            requestLine >> method >> path;
        }
        while (std::getline(headerStream, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) break;
            auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string key = toLower(line.substr(0, colon));
            std::string value = line.substr(colon + 1);
            while (!value.empty() && value.front() == ' ') value.erase(value.begin());
            if (key == "content-length") contentLength = std::stoul(value);
        }

        if (m_buffer.size() < bodyStart + contentLength) return false;

        std::string body = m_buffer.substr(bodyStart, contentLength);
        handleHttp(method, path, body);
        return true;
    }

    void handleHttp(const std::string& method, const std::string& path, const std::string& body) {
        if (method == "OPTIONS") {
            write("HTTP/1.1 204 No Content\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: POST, "
                  "OPTIONS\r\nAccess-Control-Allow-Headers: Content-Type\r\nContent-Length: 0\r\n\r\n");
            return;
        }

        if (method != "POST" || (path != "/mcp" && path != "/mcp/")) {
            write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
            return;
        }

        nlohmann::json responses = nlohmann::json::array();
        try {
            auto parsed = nlohmann::json::parse(body);
            if (parsed.is_array()) {
                for (const auto& item : parsed) {
                    if (auto resp = handleMcpRequest(item)) responses.push_back(*resp);
                }
            } else {
                if (auto resp = handleMcpRequest(parsed)) responses.push_back(*resp);
            }
        } catch (const std::exception& e) {
            nlohmann::json err;
            err["jsonrpc"] = "2.0";
            err["id"] = nullptr;
            err["error"] = {{"code", -32700}, {"message", e.what()}};
            responses.push_back(err);
        }

        std::string payload;
        if (responses.size() == 1) {
            payload = responses[0].dump();
        } else {
            payload = responses.dump();
        }

        std::string http = fmt::format(
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Content-Length: {}\r\n"
            "Connection: close\r\n\r\n",
            payload.size());
        http += payload;
        write(std::move(http));
        g_system->log(LogClass::SYSTEM, "MCP request handled (%zu bytes response)\n", payload.size());
    }

    enum { CLOSED, OPEN, CLOSING } m_status = CLOSED;
    McpServer* m_server;
    uv_tcp_t m_tcp;
    char m_readBuf[8192];
    std::string m_buffer;
    Intrusive::HashTable<uintptr_t, WriteRequest> m_writes;
};

void McpServer::McpClient::shutdown() {
    if (m_status == CLOSED || m_status == CLOSING) return;
    m_status = CLOSING;
    uv_close(reinterpret_cast<uv_handle_t*>(&m_tcp), [](uv_handle_t* handle) {
        auto* self = static_cast<McpClient*>(handle->data);
        self->m_status = CLOSED;
        self->unlink();
        delete self;
    });
}

McpServer::McpServer() : m_listener(g_system->m_eventBus) {
    m_listener.listen<Events::SettingsLoaded>([this](const auto&) {
        auto& settings = g_emulator->settings.get<Emulator::SettingDebugSettings>();
        if (settings.get<Emulator::DebugSettings::McpServer>() && (m_serverStatus != SERVER_STARTED)) {
            startServer(g_system->getLoop(), settings.get<Emulator::DebugSettings::McpServerPort>());
        }
    });
    m_listener.listen<Events::Quitting>([this](const auto&) {
        if (m_serverStatus == SERVER_STARTED) stopServer();
    });
}

void McpServer::stopServer() {
    if (m_serverStatus != SERVER_STARTED) return;
    m_serverStatus = SERVER_STOPPING;
    while (!m_clients.empty()) {
        m_clients.begin()->shutdown();
    }
    uv_close(reinterpret_cast<uv_handle_t*>(&m_server), closeCB);
}

void McpServer::startServer(uv_loop_t* loop, int port) {
    if (m_serverStatus != SERVER_STOPPED) return;
    m_loop = loop;
    uv_tcp_init(loop, &m_server);
    m_server.data = this;

    struct sockaddr_in bindAddr;
    if (uv_ip4_addr("127.0.0.1", port, &bindAddr) != 0) {
        uv_close(reinterpret_cast<uv_handle_t*>(&m_server), closeCB);
        return;
    }
    if (uv_tcp_bind(&m_server, reinterpret_cast<const sockaddr*>(&bindAddr), 0) != 0) {
        uv_close(reinterpret_cast<uv_handle_t*>(&m_server), closeCB);
        return;
    }
    if (uv_listen(reinterpret_cast<uv_stream_t*>(&m_server), 8, onNewConnectionTrampoline) != 0) {
        uv_close(reinterpret_cast<uv_handle_t*>(&m_server), closeCB);
        return;
    }
    m_serverStatus = SERVER_STARTED;
    g_system->log(LogClass::SYSTEM, "MCP server listening on http://127.0.0.1:%d/mcp\n", port);
}

void McpServer::onNewConnectionTrampoline(uv_stream_t* server, int status) {
    if (status < 0) return;
    static_cast<McpServer*>(server->data)->onNewConnection(status);
}

void McpServer::onNewConnection(int) {
    auto* client = new McpClient(this);
    m_clients.push_back(client);
    if (!client->accept(&m_server)) {
        client->unlink();
        delete client;
    }
}

void McpServer::closeCB(uv_handle_t* handle) {
    static_cast<McpServer*>(handle->data)->m_serverStatus = SERVER_STOPPED;
}

}  // namespace PCSX
