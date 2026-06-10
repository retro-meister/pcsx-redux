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

#include "core/movie.h"

#include <cstring>

#include "core/psxemulator.h"
#include "core/r3000a.h"
#include "core/sstate.h"
#include "lua/luawrapper.h"
#include "support/file.h"

namespace PCSX {

namespace {

constexpr char MOVIE_MAGIC[8] = {'P', 'C', 'S', 'X', 'M', 'v', '2', '\0'};

void writeFrame(IO<File>& file, const PadInputState& port) {
    file->write<uint16_t>(port.buttonStatus);
    file->write<uint8_t>(port.leftJoyX);
    file->write<uint8_t>(port.leftJoyY);
    file->write<uint8_t>(port.rightJoyX);
    file->write<uint8_t>(port.rightJoyY);
}

bool readFrame(IO<File>& file, PadInputState& port) {
    if (file->failed()) return false;
    port.buttonStatus = file->read<uint16_t>();
    port.leftJoyX = file->read<uint8_t>();
    port.leftJoyY = file->read<uint8_t>();
    port.rightJoyX = file->read<uint8_t>();
    port.rightJoyY = file->read<uint8_t>();
    return !file->failed();
}

}  // namespace

MovieManager::MovieManager() : m_listener(g_system->m_eventBus) {
    m_listener.listen<Events::GPU::VSync>([this](const auto&) { onVsync(); });
}

void MovieManager::ensureDeterminismWarning() {
    if (g_emulator->m_cpu->isDynarec()) {
        g_system->message(_("Rerecording works best with the interpreter CPU (disable Dynarec in settings)."));
    }
}

bool MovieManager::loadStartingSaveState() {
    if (m_savestate.empty()) return false;
    return SaveStates::load(m_savestate);
}

void MovieManager::applyFrame(uint64_t index) {
    if (index >= m_frames.size()) return;
    const Frame& frame = m_frames[index];
    g_emulator->m_pads->setPortInjection(Pads::Port::Port1, frame.port1);
    g_emulator->m_pads->setPortInjection(Pads::Port::Port2, frame.port2);
    g_emulator->m_pads->setInjectionActive(true);
}

void MovieManager::captureAndAppendFrame() {
    Frame frame;
    frame.port1 = g_emulator->m_pads->getPortState(Pads::Port::Port1);
    frame.port2 = g_emulator->m_pads->getPortState(Pads::Port::Port2);
    m_frames.push_back(frame);
    m_frameIndex = m_frames.size();
}

void MovieManager::onVsync() {
    if (m_mode == Mode::Recording) {
        captureAndAppendFrame();
    } else if (m_mode == Mode::Playing) {
        m_frameIndex++;
        if (m_frameIndex < m_frames.size()) {
            applyFrame(m_frameIndex);
        } else {
            stop(true);
        }
    }

    if (m_runUntilVblank) {
        m_runUntilVblank = false;
        g_system->pause();
    }
}

bool MovieManager::startRecording() {
    if (m_mode != Mode::Idle) return false;
    ensureDeterminismWarning();
    m_savestate = SaveStates::save();
    if (m_savestate.empty()) return false;
    m_frames.clear();
    m_frameIndex = 0;
    m_mode = Mode::Recording;
    g_emulator->m_pads->clearInjection();
    return true;
}

void MovieManager::stop(bool pauseAfter) {
    m_mode = Mode::Idle;
    m_runUntilVblank = false;
    g_emulator->m_pads->clearInjection();
    if (pauseAfter) {
        g_system->pause();
    }
}

bool MovieManager::startPlaying() {
    if (m_mode != Mode::Idle || m_frames.empty() || m_savestate.empty()) return false;
    ensureDeterminismWarning();
    if (!loadStartingSaveState()) return false;
    m_frameIndex = 0;
    m_mode = Mode::Playing;
    applyFrame(0);
    g_system->resume();
    return true;
}

bool MovieManager::load(const std::filesystem::path& path) {
    if (m_mode != Mode::Idle) stop(false);

    IO<File> file = new PosixFile(path);
    if (file->failed()) return false;

    char magic[8];
    if (file->read(magic, sizeof(magic)) != sizeof(magic)) return false;
    if (std::memcmp(magic, MOVIE_MAGIC, sizeof(MOVIE_MAGIC)) != 0) return false;

    uint64_t frameCount = file->read<uint64_t>();
    uint64_t savestateSize = file->read<uint64_t>();
    if (savestateSize == 0) return false;

    std::string savestate;
    savestate.resize(savestateSize);
    if (file->read(savestate.data(), savestateSize) != static_cast<ssize_t>(savestateSize)) return false;

    std::vector<Frame> frames;
    frames.reserve(frameCount);

    for (uint64_t i = 0; i < frameCount; i++) {
        Frame frame;
        if (!readFrame(file, frame.port1) || !readFrame(file, frame.port2)) return false;
        frames.push_back(frame);
    }

    if (file->failed()) return false;

    m_savestate = std::move(savestate);
    m_frames = std::move(frames);
    m_frameIndex = 0;
    m_path = path;
    return true;
}

bool MovieManager::save(const std::filesystem::path& path) {
    if (m_frames.empty() || m_savestate.empty()) return false;

    IO<File> file = new PosixFile(path, FileOps::TRUNCATE);
    if (file->failed()) return false;

    file->write(MOVIE_MAGIC, sizeof(MOVIE_MAGIC));
    file->write<uint64_t>(m_frames.size());
    file->write<uint64_t>(m_savestate.size());
    file->write(m_savestate.data(), m_savestate.size());

    for (const Frame& frame : m_frames) {
        writeFrame(file, frame.port1);
        writeFrame(file, frame.port2);
    }

    if (file->failed()) return false;

    m_path = path;
    return true;
}

void MovieManager::frameAdvance() { runUntilVblank(); }

void MovieManager::runUntilVblank() {
    if (!g_system->running()) {
        m_runUntilVblank = true;
        g_system->resume();
    }
}

const MovieManager::Frame* MovieManager::getFrameInput(uint64_t index) const {
    if (index >= m_frames.size()) return nullptr;
    return &m_frames[index];
}

namespace {

struct PadButtonName {
    const char* name;
    unsigned bit;
};

constexpr PadButtonName s_padButtons[] = {
    {"SELECT", 0},   {"START", 3},    {"UP", 4},        {"RIGHT", 5},   {"DOWN", 6},
    {"LEFT", 7},     {"L2", 8},       {"R2", 9},        {"L1", 10},     {"R1", 11},
    {"TRIANGLE", 12}, {"CIRCLE", 13}, {"CROSS", 14},    {"SQUARE", 15},
};

void pushPortInput(Lua& L, const PadInputState& port) {
    L.newtable();
    for (const auto& button : s_padButtons) {
        L.push(button.name);
        L.push(((port.buttonStatus & (1 << button.bit)) == 0));
        L.settable();
    }
    L.push("leftX");
    L.push(lua_Number(port.leftJoyX));
    L.settable();
    L.push("leftY");
    L.push(lua_Number(port.leftJoyY));
    L.settable();
    L.push("rightX");
    L.push(lua_Number(port.rightJoyX));
    L.settable();
    L.push("rightY");
    L.push(lua_Number(port.rightJoyY));
    L.settable();
}

}  // namespace

void MovieManager::setLua(Lua L) {
    L.getfieldtable("PCSX", LUA_GLOBALSINDEX);
    L.push("Movie");
    L.newtable();

    L.declareFunc(
        "startRecording",
        [](lua_State* L_) -> int {
            Lua L(L_);
            bool ok = g_emulator->m_movie->startRecording();
            L.push(ok);
            return 1;
        },
        -1);

    L.declareFunc(
        "stop",
        [](lua_State* L_) -> int {
            Lua L(L_);
            g_emulator->m_movie->stop();
            return 0;
        },
        -1);

    L.declareFunc(
        "play",
        [](lua_State* L_) -> int {
            Lua L(L_);
            if (L.gettop() >= 1 && L.isstring(1)) {
                if (!g_emulator->m_movie->load(L.tostring(1))) {
                    return L.error("Failed to load movie file");
                }
            }
            bool ok = g_emulator->m_movie->startPlaying();
            L.push(ok);
            return 1;
        },
        -1);

    L.declareFunc(
        "load",
        [](lua_State* L_) -> int {
            Lua L(L_);
            if (L.gettop() < 1 || !L.isstring(1)) {
                return L.error("Movie.load needs a path");
            }
            bool ok = g_emulator->m_movie->load(L.tostring(1));
            L.push(ok);
            return 1;
        },
        -1);

    L.declareFunc(
        "save",
        [](lua_State* L_) -> int {
            Lua L(L_);
            if (L.gettop() < 1 || !L.isstring(1)) {
                return L.error("Movie.save needs a path");
            }
            bool ok = g_emulator->m_movie->save(L.tostring(1));
            L.push(ok);
            return 1;
        },
        -1);

    L.declareFunc(
        "frameAdvance",
        [](lua_State* L_) -> int {
            Lua L(L_);
            g_emulator->m_movie->frameAdvance();
            return 0;
        },
        -1);

    L.declareFunc(
        "getFrame",
        [](lua_State* L_) -> int {
            Lua L(L_);
            L.push(lua_Number(g_emulator->m_movie->getFrameIndex()));
            return 1;
        },
        -1);

    L.declareFunc(
        "getFrameCount",
        [](lua_State* L_) -> int {
            Lua L(L_);
            L.push(lua_Number(g_emulator->m_movie->getFrameCount()));
            return 1;
        },
        -1);

    L.declareFunc(
        "getMode",
        [](lua_State* L_) -> int {
            Lua L(L_);
            switch (g_emulator->m_movie->getMode()) {
                case MovieManager::Mode::Idle:
                    L.push("idle");
                    break;
                case MovieManager::Mode::Recording:
                    L.push("recording");
                    break;
                case MovieManager::Mode::Playing:
                    L.push("playing");
                    break;
            }
            return 1;
        },
        -1);

    L.declareFunc(
        "getInput",
        [](lua_State* L_) -> int {
            Lua L(L_);
            if (L.gettop() < 1 || !L.isnumber(1)) {
                return L.error("Movie.getInput needs a frame index");
            }
            const uint64_t frame = L.checknumber(1);
            const Frame* input = g_emulator->m_movie->getFrameInput(frame);
            if (!input) {
                L.push();
                return 1;
            }
            if (L.gettop() >= 2 && !L.isnil(2)) {
                if (!L.isnumber(2)) {
                    return L.error("Movie.getInput port must be 1 or 2");
                }
                const int port = static_cast<int>(L.checknumber(2));
                if (port == 1) {
                    pushPortInput(L, input->port1);
                    return 1;
                }
                if (port == 2) {
                    pushPortInput(L, input->port2);
                    return 1;
                }
                return L.error("Movie.getInput port must be 1 or 2");
            }
            L.newtable();
            L.push("port1");
            pushPortInput(L, input->port1);
            L.settable();
            L.push("port2");
            pushPortInput(L, input->port2);
            L.settable();
            return 1;
        },
        -1);

    L.settable();
    L.pop();
}

}  // namespace PCSX
