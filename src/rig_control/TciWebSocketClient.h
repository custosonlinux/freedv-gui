//=========================================================================
// Name:            TciWebSocketClient.h
// Purpose:         WebSocket client for TCI protocol
//
// Authors:         Tomas Ostojic
// License:
//
//  All rights reserved.
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License version 2.1,
//  as published by the Free Software Foundation.  This program is
//  distributed in the hope that it will be useful, but WITHOUT ANY
//  WARRANTY; without even the implied warranty of MERCHANTABILITY or
//  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
//  License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, see <http://www.gnu.org/licenses/>.
//
//=========================================================================

#ifndef TCI_WEBSOCKET_CLIENT_H
#define TCI_WEBSOCKET_CLIENT_H

#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <queue>
#include <functional>
#include <memory>

#include "TciProtocol.h"

// Forward declaration for WebSocket implementation details
namespace boost { namespace asio { class io_context; } }
namespace websocketpp { namespace client { template<typename T> class client; } }

namespace tci
{
    class TciWebSocketClient
    {
    public:
        TciWebSocketClient();
        virtual ~TciWebSocketClient();
        
        // Connect to TCI server
        bool connect(const std::string& hostname, int port);
        
        // Disconnect from TCI server
        void disconnect();
        
        // Check if connected
        bool isConnected() const;
        
        // Send a TCI command
        bool sendCommand(const std::string& command);
        
        // Send binary audio stream data
        bool sendBinaryData(const uint8_t* data, size_t size);
        
        // Set callback for received commands
        void setCommandCallback(CommandCallback callback);
        
        // Set callback for received audio streams
        void setStreamCallback(StreamCallback callback);
        
        // Set callback for connection events
        void setOnConnected(std::function<void()> callback);
        void setOnDisconnected(std::function<void()> callback);
        void setOnError(std::function<void(const std::string&)> callback);
        
    private:
        std::string hostname_;
        int port_;
        std::atomic<bool> connected_;
        std::atomic<bool> shouldRun_;
        
        std::unique_ptr<std::thread> ioThread_;
        std::unique_ptr<std::thread> receiveThread_;
        
        std::mutex sendMutex_;
        std::queue<std::string> sendQueue_;
        
        CommandCallback commandCallback_;
        StreamCallback streamCallback_;
        std::function<void()> onConnectedCallback_;
        std::function<void()> onDisconnectedCallback_;
        std::function<void(const std::string&)> onErrorCallback_;
        
        // Platform-specific socket handle
        int socketFd_;
        
        void ioThreadFunc_();
        void receiveThreadFunc_();
        
        bool connectSocket_();
        bool performWebSocketHandshake_();
        void closeSocket_();
        bool sendData_(const uint8_t* data, size_t size);
        int receiveData_(uint8_t* buffer, size_t bufferSize);
        
        void processReceivedData_(const std::string& data);
        void processBinaryData_(const uint8_t* data, size_t size);
        
        std::string receiveBuffer_;
        std::vector<uint8_t> binaryBuffer_;
    };
}

#endif // TCI_WEBSOCKET_CLIENT_H
