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
#include <filesystem>
#include <string>
#include <vector>

#include "core/pad.h"
#include "core/system.h"

namespace PCSX {

class Lua;

class MovieManager {
  public:
    enum class Mode { Idle, Recording, Playing };

    struct Frame {
        PadInputState port1;
        PadInputState port2;
    };

    MovieManager();
    ~MovieManager() = default;

    Mode getMode() const { return m_mode; }
    uint64_t getFrameIndex() const { return m_frameIndex; }
    uint64_t getFrameCount() const { return m_frames.size(); }
    const std::filesystem::path& getPath() const { return m_path; }
    bool hasStartingSaveState() const { return !m_savestate.empty(); }
    const Frame* getFrameInput(uint64_t index) const;

    bool startRecording();
    void stop(bool pauseAfter = false);
    bool startPlaying();
    bool load(const std::filesystem::path& path);
    bool save(const std::filesystem::path& path);
    void frameAdvance();
    void runUntilVblank();

    void setLua(Lua L);

  private:
    void onVsync();
    void applyFrame(uint64_t index);
    void captureAndAppendFrame();
    void ensureDeterminismWarning();
    bool loadStartingSaveState();

    EventBus::Listener m_listener;
    Mode m_mode = Mode::Idle;
    std::vector<Frame> m_frames;
    std::string m_savestate;
    uint64_t m_frameIndex = 0;
    std::filesystem::path m_path;
    bool m_runUntilVblank = false;
};

}  // namespace PCSX
