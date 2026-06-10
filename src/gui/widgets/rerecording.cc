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

#include "gui/widgets/rerecording.h"

#include "core/movie.h"
#include "core/psxemulator.h"
#include "core/r3000a.h"
#include "core/system.h"
#include "imgui.h"

void PCSX::Widgets::Rerecording::draw(const char* title) {
    ImGui::SetNextWindowPos(ImVec2(520, 30), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(420, 280), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title, &m_show)) {
        ImGui::End();
        return;
    }

    MovieManager* movie = g_emulator->m_movie.get();
    const bool idle = movie->getMode() == MovieManager::Mode::Idle;
    const bool recording = movie->getMode() == MovieManager::Mode::Recording;
    const bool playing = movie->getMode() == MovieManager::Mode::Playing;

    if (g_emulator->m_cpu->isDynarec()) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "%s", _("Disable Dynarec for deterministic rerecording"));
    }

    const char* status = recording ? _("Recording") : (playing ? _("Playing") : _("Idle"));
    ImGui::Text("%s: %s", _("Status"), status);
    if (g_emulator->m_pads->getInjectionActive()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", _("Movie input injection active"));
    }
    if (!g_system->running()) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "%s", _("Emulator paused — press Run"));
    }
    ImGui::Text("%s: %llu / %llu", _("Frame"), movie->getFrameIndex(), movie->getFrameCount());

    if (!movie->getPath().empty()) {
        ImGui::TextWrapped("%s: %s", _("File"), movie->getPath().string().c_str());
    }
    if (movie->hasStartingSaveState()) {
        ImGui::TextUnformatted(_("Starting savestate: present"));
    }

    ImGui::Separator();

    if (idle) {
        if (ImGui::Button(_("Record"))) {
            movie->startRecording();
        }
        ImGui::SameLine();
        if (ImGui::Button(_("Play"))) {
            movie->startPlaying();
        }
    } else {
        if (ImGui::Button(_("Stop"))) {
            movie->stop();
        }
    }

    ImGui::SameLine();
    if (ImGui::Button(_("Frame Advance"))) {
        movie->frameAdvance();
    }

    ImGui::Separator();

    if (ImGui::Button(_("Load Movie..."))) {
        m_loadDialog.openDialog();
    }
    ImGui::SameLine();
    if (ImGui::Button(_("Save Movie..."))) {
        m_saveDialog.openDialog();
    }

    if (m_loadDialog.draw()) {
        const auto& selected = m_loadDialog.selected();
        if (!selected.empty()) {
            if (!movie->load(selected[0])) {
                g_system->message(_("Failed to load movie file (expected .pcsxmv with embedded savestate)"));
            }
        }
    }

    if (m_saveDialog.draw()) {
        const auto& selected = m_saveDialog.selected();
        if (!selected.empty()) {
            if (!movie->save(selected[0])) {
                g_system->message(_("Failed to save movie file (record a movie first)"));
            }
        }
    }

    ImGui::End();
}
