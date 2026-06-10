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

#include <string>
#include <vector>

#include "gui/widgets/filedialog.h"

namespace PCSX {

namespace Widgets {

class Rerecording {
  public:
    Rerecording(bool& show, std::vector<std::string>& favorites)
        : m_show(show),
          m_loadDialog(l_("Load Movie"), favorites, "PCSX-Redux Movie {.pcsxmv},.*"),
          m_saveDialog(l_("Save Movie"), favorites, "PCSX-Redux Movie {.pcsxmv}") {
        m_loadDialog.m_currentPath = "movies";
        m_saveDialog.m_currentPath = "movies";
    }
    void draw(const char* title);
    bool& m_show;

  private:
    Widgets::FileDialog<> m_loadDialog;
    Widgets::FileDialog<FileDialogMode::Save> m_saveDialog;
};

}  // namespace Widgets

}  // namespace PCSX
