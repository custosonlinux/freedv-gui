//=========================================================================
// Name:            TciRigController.h
// Purpose:         Controls radios using TCI protocol
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

#ifndef TCI_RIG_CONTROLLER_H
#define TCI_RIG_CONTROLLER_H

#include <string>
#include <memory>
#include <mutex>
#include <atomic>
#include <map>

#include "util/ThreadedObject.h"
#include "IRigFrequencyController.h"
#include "IRigPttController.h"
#include "TciWebSocketClient.h"
#include "TciProtocol.h"

class TciRigController : public ThreadedObject, 
                         public IRigFrequencyController, 
                         public IRigPttController
{
public:
    TciRigController(std::string hostname, int port = 40001, int trx = 0, int channel = 0);
    virtual ~TciRigController();
    
    // IRigController interface
    virtual void connect() override;
    virtual void disconnect() override;
    virtual bool isConnected() override;
    
    // IRigPttController interface
    virtual void ptt(bool state) override;
    virtual int getRigResponseTimeMicroseconds() override;
    
    // IRigFrequencyController interface
    virtual void setFrequency(uint64_t frequency) override;
    virtual void setMode(Mode mode) override;
    virtual void requestCurrentFrequencyMode() override;
    
    // TCI-specific methods
    void setTrxChannel(int trx, int channel);
    int getTrx() const { return trx_; }
    int getChannel() const { return channel_; }
    
    // Get TCI WebSocket client for audio device
    std::shared_ptr<tci::TciWebSocketClient> getWebSocketClient();
    
private:
    std::string hostname_;
    int port_;
    int trx_;           // TCI transceiver number (usually 0)
    int channel_;       // TCI channel number (usually 0)
    
    std::shared_ptr<tci::TciWebSocketClient> wsClient_;
    std::atomic<bool> connected_;
    std::atomic<bool> pttState_;
    
    std::mutex stateMutex_;
    uint64_t currentFrequency_;
    tci::Modulation currentModulation_;
    
    // TCI device capabilities
    std::string deviceName_;
    int trxCount_;
    int channelCount_;
    std::vector<std::string> supportedModulations_;
    uint64_t minFrequency_;
    uint64_t maxFrequency_;
    
    // Response time for PTT
    const int rigResponseTime_ = 100000; // 100ms in microseconds
    
    // Command handlers
    void handleCommand_(const std::string& cmdName, const std::vector<std::string>& args);
    void handleProtocol_(const std::vector<std::string>& args);
    void handleDevice_(const std::vector<std::string>& args);
    void handleTrxCount_(const std::vector<std::string>& args);
    void handleChannelCount_(const std::vector<std::string>& args);
    void handleModulationsList_(const std::vector<std::string>& args);
    void handleVfoLimits_(const std::vector<std::string>& args);
    void handleVfo_(const std::vector<std::string>& args);
    void handleModulation_(const std::vector<std::string>& args);
    void handleTrx_(const std::vector<std::string>& args);
    
    // Helper methods
    void sendCommand_(const std::string& cmdName, const std::vector<std::string>& args);
    tci::Modulation freeDvModeToTci_(Mode mode);
    Mode tciModeToFreeDv_(tci::Modulation mod);
    
    // Connection event handlers
    void onConnected_();
    void onDisconnected_();
    void onError_(const std::string& error);
};

#endif // TCI_RIG_CONTROLLER_H
