//=========================================================================
// Name:            TciWebSocketClient.cpp
// Purpose:         WebSocket client for TCI protocol implementation
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

#include "TciWebSocketClient.h"
#include <iostream>
#include <cstring>
#include <chrono>
#include <sstream>
#include <random>
#include <iomanip>

// POSIX socket includes
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <fcntl.h>

namespace tci
{
    // WebSocket helper functions
    static std::string base64_encode(const unsigned char* data, size_t len) {
        static const char* base64_chars = 
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "abcdefghijklmnopqrstuvwxyz"
            "0123456789+/";
        
        std::string ret;
        int i = 0;
        unsigned char char_array_3[3];
        unsigned char char_array_4[4];
        
        while (len--) {
            char_array_3[i++] = *(data++);
            if (i == 3) {
                char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
                char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
                char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
                char_array_4[3] = char_array_3[2] & 0x3f;
                
                for(i = 0; i < 4; i++)
                    ret += base64_chars[char_array_4[i]];
                i = 0;
            }
        }
        
        if (i) {
            for(int j = i; j < 3; j++)
                char_array_3[j] = '\0';
            
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            
            for (int j = 0; j < i + 1; j++)
                ret += base64_chars[char_array_4[j]];
            
            while((i++ < 3))
                ret += '=';
        }
        
        return ret;
    }
    
    static std::string generate_websocket_key() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 255);
        
        unsigned char random_bytes[16];
        for (int i = 0; i < 16; i++) {
            random_bytes[i] = dis(gen);
        }
        
        return base64_encode(random_bytes, 16);
    }
    
    TciWebSocketClient::TciWebSocketClient()
        : port_(0)
        , connected_(false)
        , shouldRun_(false)
        , socketFd_(-1)
    {
    }
    
    TciWebSocketClient::~TciWebSocketClient()
    {
        disconnect();
    }
    
    bool TciWebSocketClient::connect(const std::string& hostname, int port)
    {
        if (connected_)
        {
            return true;
        }
        
        hostname_ = hostname;
        port_ = port;
        
        if (!connectSocket_())
        {
            if (onErrorCallback_)
            {
                onErrorCallback_("Failed to connect to " + hostname + ":" + std::to_string(port));
            }
            return false;
        }
        
        connected_ = true;
        shouldRun_ = true;
        
        // Start receive thread
        receiveThread_ = std::make_unique<std::thread>(&TciWebSocketClient::receiveThreadFunc_, this);
        
        if (onConnectedCallback_)
        {
            onConnectedCallback_();
        }
        
        return true;
    }
    
    void TciWebSocketClient::disconnect()
    {
        if (!connected_)
        {
            return;
        }
        
        shouldRun_ = false;
        connected_ = false;
        
        closeSocket_();
        
        // Wait for threads to finish
        if (receiveThread_ && receiveThread_->joinable())
        {
            receiveThread_->join();
        }
        
        if (onDisconnectedCallback_)
        {
            onDisconnectedCallback_();
        }
    }
    
    bool TciWebSocketClient::isConnected() const
    {
        return connected_;
    }
    
    bool TciWebSocketClient::sendCommand(const std::string& command)
    {
        if (!connected_)
        {
            return false;
        }
        
        std::lock_guard<std::mutex> lock(sendMutex_);
        return sendData_(reinterpret_cast<const uint8_t*>(command.c_str()), command.length());
    }
    
    bool TciWebSocketClient::sendBinaryData(const uint8_t* data, size_t size)
    {
        if (!connected_)
        {
            return false;
        }
        
        std::lock_guard<std::mutex> lock(sendMutex_);
        return sendData_(data, size);
    }
    
    void TciWebSocketClient::setCommandCallback(CommandCallback callback)
    {
        commandCallback_ = callback;
    }
    
    void TciWebSocketClient::setStreamCallback(StreamCallback callback)
    {
        streamCallback_ = callback;
    }
    
    void TciWebSocketClient::setOnConnected(std::function<void()> callback)
    {
        onConnectedCallback_ = callback;
    }
    
    void TciWebSocketClient::setOnDisconnected(std::function<void()> callback)
    {
        onDisconnectedCallback_ = callback;
    }
    
    void TciWebSocketClient::setOnError(std::function<void(const std::string&)> callback)
    {
        onErrorCallback_ = callback;
    }
    
    bool TciWebSocketClient::connectSocket_()
    {
        // Create socket
        socketFd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (socketFd_ < 0)
        {
            return false;
        }
        
        // Resolve hostname
        struct hostent* server = gethostbyname(hostname_.c_str());
        if (server == nullptr)
        {
            closeSocket_();
            return false;
        }
        
        // Setup server address
        struct sockaddr_in serverAddr;
        std::memset(&serverAddr, 0, sizeof(serverAddr));
        serverAddr.sin_family = AF_INET;
        std::memcpy(&serverAddr.sin_addr.s_addr, server->h_addr, server->h_length);
        serverAddr.sin_port = htons(port_);
        
        // Connect
        if (::connect(socketFd_, reinterpret_cast<struct sockaddr*>(&serverAddr), sizeof(serverAddr)) < 0)
        {
            closeSocket_();
            return false;
        }
        
        // Perform WebSocket handshake
        if (!performWebSocketHandshake_())
        {
            closeSocket_();
            return false;
        }
        
        // Set non-blocking mode for receive
        int flags = fcntl(socketFd_, F_GETFL, 0);
        fcntl(socketFd_, F_SETFL, flags | O_NONBLOCK);
        
        return true;
    }
    
    bool TciWebSocketClient::performWebSocketHandshake_()
    {
        // Generate WebSocket key
        std::string key = generate_websocket_key();
        
        // Build handshake request
        std::ostringstream request;
        request << "GET / HTTP/1.1\r\n";
        request << "Host: " << hostname_ << ":" << port_ << "\r\n";
        request << "Upgrade: websocket\r\n";
        request << "Connection: Upgrade\r\n";
        request << "Sec-WebSocket-Key: " << key << "\r\n";
        request << "Sec-WebSocket-Version: 13\r\n";
        request << "\r\n";
        
        std::string req_str = request.str();
        
        // Send handshake
        ssize_t sent = send(socketFd_, req_str.c_str(), req_str.length(), 0);
        if (sent <= 0)
        {
            return false;
        }
        
        // Wait for response
        char buffer[2048];
        ssize_t received = recv(socketFd_, buffer, sizeof(buffer) - 1, 0);
        if (received <= 0)
        {
            return false;
        }
        
        buffer[received] = '\0';
        std::string response(buffer);
        
        // Check for successful handshake
        if (response.find("101") != std::string::npos &&
            response.find("Switching Protocols") != std::string::npos)
        {
            return true;
        }
        
        return false;
    }
    
    void TciWebSocketClient::closeSocket_()
    {
        if (socketFd_ >= 0)
        {
            ::close(socketFd_);
            socketFd_ = -1;
        }
    }
    
    bool TciWebSocketClient::sendData_(const uint8_t* data, size_t size)
    {
        if (socketFd_ < 0)
        {
            return false;
        }
        
        // Create WebSocket frame (text frame with masking)
        std::vector<uint8_t> frame;
        
        // FIN=1, opcode=1 (text frame) for ASCII commands
        // FIN=1, opcode=2 (binary frame) for audio data
        bool isBinary = (size >= sizeof(StreamHeader));
        frame.push_back(isBinary ? 0x82 : 0x81);
        
        // Mask=1, payload length
        if (size < 126) {
            frame.push_back(0x80 | size);
        } else if (size < 65536) {
            frame.push_back(0x80 | 126);
            frame.push_back((size >> 8) & 0xFF);
            frame.push_back(size & 0xFF);
        } else {
            frame.push_back(0x80 | 127);
            for (int i = 7; i >= 0; i--) {
                frame.push_back((size >> (i * 8)) & 0xFF);
            }
        }
        
        // Masking key (random)
        uint8_t mask[4];
        std::random_device rd;
        for (int i = 0; i < 4; i++) {
            mask[i] = rd() & 0xFF;
            frame.push_back(mask[i]);
        }
        
        // Masked payload
        for (size_t i = 0; i < size; i++) {
            frame.push_back(data[i] ^ mask[i % 4]);
        }
        
        // Send the frame
        ssize_t sent = send(socketFd_, frame.data(), frame.size(), 0);
        return (sent == static_cast<ssize_t>(frame.size()));
    }
    
    void TciWebSocketClient::receiveThreadFunc_()
    {
        const size_t bufferSize = 65536;
        std::vector<uint8_t> buffer(bufferSize);
        
        while (shouldRun_)
        {
            int received = recv(socketFd_, buffer.data(), bufferSize, 0);
            
            if (received > 0)
            {
                // Parse WebSocket frames
                size_t offset = 0;
                while (offset + 2 <= static_cast<size_t>(received))
                {
                    // uint8_t fin = buffer[offset] & 0x80;  // Not used currently
                    uint8_t opcode = buffer[offset] & 0x0F;
                    offset++;
                    
                    uint8_t masked = buffer[offset] & 0x80;
                    uint64_t payloadLen = buffer[offset] & 0x7F;
                    offset++;
                    
                    // Handle extended payload length
                    if (payloadLen == 126)
                    {
                        if (offset + 2 > static_cast<size_t>(received)) break;
                        payloadLen = (static_cast<uint16_t>(buffer[offset]) << 8) | 
                                    static_cast<uint16_t>(buffer[offset + 1]);
                        offset += 2;
                    }
                    else if (payloadLen == 127)
                    {
                        if (offset + 8 > static_cast<size_t>(received)) break;
                        payloadLen = 0;
                        for (int i = 0; i < 8; i++)
                        {
                            payloadLen = (payloadLen << 8) | static_cast<uint8_t>(buffer[offset + i]);
                        }
                        offset += 8;
                    }
                    
                    // Skip mask if present (server shouldn't mask)
                    if (masked)
                    {
                        offset += 4;
                    }
                    
                    // Check we have full payload
                    if (offset + payloadLen > static_cast<size_t>(received))
                    {
                        break;
                    }
                    
                    // Process based on opcode
                    if (opcode == 1)  // Text frame
                    {
                        // Accumulate text data
                        for (size_t i = 0; i < payloadLen; ++i)
                        {
                            char ch = static_cast<char>(buffer[offset + i]);
                            receiveBuffer_ += ch;
                            
                            if (ch == ';')
                            {
                                processReceivedData_(receiveBuffer_);
                                receiveBuffer_.clear();
                            }
                        }
                    }
                    else if (opcode == 2)  // Binary frame
                    {
                        processBinaryData_(buffer.data() + offset, payloadLen);
                    }
                    
                    offset += payloadLen;
                }
            }
            else if (received == 0)
            {
                // Connection closed
                connected_ = false;
                break;
            }
            else
            {
                // Error or would block
                if (errno != EAGAIN && errno != EWOULDBLOCK)
                {
                    // Real error
                    connected_ = false;
                    break;
                }
                
                // Would block, sleep a bit
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        
        // Connection lost
        if (connected_)
        {
            connected_ = false;
            if (onDisconnectedCallback_)
            {
                onDisconnectedCallback_();
            }
        }
    }
    
    void TciWebSocketClient::processReceivedData_(const std::string& data)
    {
        if (!commandCallback_)
        {
            return;
        }
        
        CommandParser parser;
        std::string cmdName;
        std::vector<std::string> args;
        
        if (parser.parseCommand(data, cmdName, args))
        {
            commandCallback_(cmdName, args);
        }
    }
    
    void TciWebSocketClient::processBinaryData_(const uint8_t* data, size_t size)
    {
        if (!streamCallback_ || size < sizeof(StreamHeader))
        {
            return;
        }
        
        const StreamHeader* header = reinterpret_cast<const StreamHeader*>(data);
        const uint8_t* audioData = data + sizeof(StreamHeader);
        size_t audioDataSize = size - sizeof(StreamHeader);
        
        streamCallback_(*header, audioData, audioDataSize);
    }
}
