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

#include <uv.h>

#include "support/eventbus.h"
#include "support/list.h"

namespace PCSX {

class McpServer {
  public:
    McpServer();
    enum McpServerStatus {
        SERVER_STOPPED,
        SERVER_STOPPING,
        SERVER_STARTED,
    };
    McpServerStatus getServerStatus() { return m_serverStatus; }

    void startServer(uv_loop_t* loop, int port = 8090);
    void stopServer();

  private:
    class McpClient;
    static void onNewConnectionTrampoline(uv_stream_t* server, int status);
    void onNewConnection(int status);
    static void closeCB(uv_handle_t* handle);
    McpServerStatus m_serverStatus = SERVER_STOPPED;
    uv_tcp_t m_server;
    uv_loop_t* m_loop = nullptr;
    Intrusive::List<McpClient> m_clients;
    EventBus::Listener m_listener;
};

}  // namespace PCSX
