//=========================================================================
// Name:            TciRigController.cpp
// Purpose:         Controls radios using TCI protocol implementation
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

#include "TciRigController.h"
#include <sstream>
#include <iostream>

TciRigController::TciRigController(std::string hostname, int port, int trx, int channel)
    : ThreadedObject("TciRigController")
    , hostname_(std::move(hostname))
    , port_(port)
    , trx_(trx)
    , channel_(channel)
    , connected_(false)
    , pttState_(false)
    , currentFrequency_(0)
    , currentModulation_(tci::DIGU)
    , trxCount_(1)
    , channelCount_(1)
    , minFrequency_(0)
    , maxFrequency_(60000000)
{
    fprintf(stderr, "TciRigController constructor: hostname='%s' port=%d\n", hostname_.c_str(), port_);
    
    wsClient_ = std::make_shared<tci::TciWebSocketClient>();
    
    // Set up callbacks
    wsClient_->setOnConnected([this]() { onConnected_(); });
    wsClient_->setOnDisconnected([this]() { onDisconnected_(); });
    wsClient_->setOnError([this](const std::string& error) { onError_(error); });
    
    wsClient_->setCommandCallback([this](const std::string& cmdName, const std::vector<std::string>& args) {
        enqueue_([this, cmdName, args]() {
            handleCommand_(cmdName, args);
        });
    });
}

TciRigController::~TciRigController()
{
    disconnect();
}

void TciRigController::connect()
{
    if (connected_)
    {
        return;
    }
    
    enqueue_([this]() {
        if (wsClient_->connect(hostname_, port_))
        {
            // Connection successful, wait for initialization commands
            // The onConnected_ callback will handle the rest
        }
        else
        {
            onRigError(this, "Failed to connect to TCI server at " + hostname_ + ":" + std::to_string(port_));
        }
    });
}

void TciRigController::disconnect()
{
    if (!connected_)
    {
        return;
    }
    
    enqueue_([this]() {
        // Turn off PTT if it's on
        if (pttState_)
        {
            ptt(false);
        }
        
        wsClient_->disconnect();
        connected_ = false;
    });
    
    waitForAllTasksComplete_();
}

bool TciRigController::isConnected()
{
    return connected_;
}

void TciRigController::ptt(bool state)
{
    fprintf(stderr, "\n=== TCI PTT REQUEST ===\n");
    fprintf(stderr, "TCI PTT: Requested state = %s\n", state ? "ON (TRANSMIT)" : "OFF (RECEIVE)");
    fprintf(stderr, "TCI PTT: Connected = %s\n", connected_ ? "YES" : "NO");
    fprintf(stderr, "TCI PTT: TRX number = %d\n", trx_);
    
    if (!connected_)
    {
        fprintf(stderr, "TCI PTT: ERROR - Not connected, cannot set PTT\n");
        fprintf(stderr, "======================\n\n");
        return;
    }
    
    fprintf(stderr, "TCI PTT: Enqueueing PTT command...\n");
    
    enqueue_([this, state]() {
        fprintf(stderr, "TCI PTT: [QUEUE] Processing PTT command\n");
        fprintf(stderr, "TCI PTT: [QUEUE] Previous PTT state = %s\n", pttState_ ? "ON" : "OFF");
        
        pttState_ = state;
        
        fprintf(stderr, "TCI PTT: [QUEUE] New PTT state = %s\n", pttState_ ? "ON" : "OFF");
        
        // TRX:trx_num,state;
        std::vector<std::string> args;
        args.push_back(std::to_string(trx_));
        args.push_back(state ? "true" : "false");
        
        fprintf(stderr, "TCI PTT: [QUEUE] Calling sendCommand_(\"trx\", [%d, %s])\n", 
                trx_, state ? "true" : "false");
        
        sendCommand_("trx", args);
        
        fprintf(stderr, "TCI PTT: [QUEUE] sendCommand_() returned\n");
        fprintf(stderr, "TCI PTT: [QUEUE] Notifying listeners...\n");
        
        // Notify listeners
        onPttChange(this, state);
        
        fprintf(stderr, "TCI PTT: [QUEUE] PTT command complete\n");
        fprintf(stderr, "======================\n\n");
    });
}

void TciRigController::setFrequency(uint64_t frequency)
{
    if (!connected_)
    {
        return;
    }
    
    enqueue_([this, frequency]() {
        std::lock_guard<std::mutex> lock(stateMutex_);
        
        // VFO:trx,vfo,frequency;
        std::vector<std::string> args;
        args.push_back(std::to_string(trx_));
        args.push_back(std::to_string(channel_));
        args.push_back(std::to_string(frequency));
        
        sendCommand_("vfo", args);
        
        currentFrequency_ = frequency;
    });
}

void TciRigController::setMode(Mode mode)
{
    if (!connected_)
    {
        return;
    }
    
    enqueue_([this, mode]() {
        std::lock_guard<std::mutex> lock(stateMutex_);
        
        tci::Modulation tciMod = freeDvModeToTci_(mode);
        
        // MODULATION:trx,modulation;
        std::vector<std::string> args;
        args.push_back(std::to_string(trx_));
        args.push_back(tci::CommandParser::modulationToString(tciMod));
        
        sendCommand_("modulation", args);
        
        currentModulation_ = tciMod;
    });
}

void TciRigController::requestCurrentFrequencyMode()
{
    // TCI protocol sends frequency and mode updates automatically when they change.
    // No need to poll - the server pushes updates via VFO: and MODULATION: commands
    // which are handled by handleVfo_() and handleModulation_() callbacks.
    // Polling would just create unnecessary command spam on the network.
}

int TciRigController::getRigResponseTimeMicroseconds()
{
    return rigResponseTime_;
}

void TciRigController::setTrxChannel(int trx, int channel)
{
    trx_ = trx;
    channel_ = channel;
}

std::shared_ptr<tci::TciWebSocketClient> TciRigController::getWebSocketClient()
{
    return wsClient_;
}

void TciRigController::handleCommand_(const std::string& cmdName, const std::vector<std::string>& args)
{
    if (cmdName == "PROTOCOL")
    {
        handleProtocol_(args);
    }
    else if (cmdName == "DEVICE")
    {
        handleDevice_(args);
    }
    else if (cmdName == "TRX_COUNT")
    {
        handleTrxCount_(args);
    }
    else if (cmdName == "CHANNEL_COUNT")
    {
        handleChannelCount_(args);
    }
    else if (cmdName == "MODULATIONS_LIST")
    {
        handleModulationsList_(args);
    }
    else if (cmdName == "VFO_LIMITS")
    {
        handleVfoLimits_(args);
    }
    else if (cmdName == "VFO")
    {
        handleVfo_(args);
    }
    else if (cmdName == "MODULATION")
    {
        handleModulation_(args);
    }
    else if (cmdName == "TRX")
    {
        handleTrx_(args);
    }
}

void TciRigController::handleProtocol_(const std::vector<std::string>& args)
{
    if (args.size() >= 2)
    {
        std::string protocol = args[0];
        std::string version = args[1];
        std::cout << "TCI Protocol: " << protocol << " v" << version << std::endl;
    }
}

void TciRigController::handleDevice_(const std::vector<std::string>& args)
{
    if (args.size() >= 1)
    {
        deviceName_ = args[0];
        std::cout << "TCI Device: " << deviceName_ << std::endl;
    }
}

void TciRigController::handleTrxCount_(const std::vector<std::string>& args)
{
    if (args.size() >= 1)
    {
        trxCount_ = std::stoi(args[0]);
    }
}

void TciRigController::handleChannelCount_(const std::vector<std::string>& args)
{
    if (args.size() >= 1)
    {
        channelCount_ = std::stoi(args[0]);
    }
}

void TciRigController::handleModulationsList_(const std::vector<std::string>& args)
{
    supportedModulations_ = args;
}

void TciRigController::handleVfoLimits_(const std::vector<std::string>& args)
{
    if (args.size() >= 2)
    {
        minFrequency_ = std::stoull(args[0]);
        maxFrequency_ = std::stoull(args[1]);
    }
}

void TciRigController::handleVfo_(const std::vector<std::string>& args)
{
    if (args.size() >= 3)
    {
        int trx = std::stoi(args[0]);
        int vfo = std::stoi(args[1]);
        uint64_t frequency = std::stoull(args[2]);
        
        if (trx == trx_ && vfo == channel_)
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            currentFrequency_ = frequency;
            
            // Notify listeners
            onFreqModeChange(this, currentFrequency_, tciModeToFreeDv_(currentModulation_));
        }
    }
}

void TciRigController::handleModulation_(const std::vector<std::string>& args)
{
    if (args.size() >= 2)
    {
        int trx = std::stoi(args[0]);
        std::string modStr = args[1];
        
        if (trx == trx_)
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            currentModulation_ = tci::CommandParser::stringToModulation(modStr);
            
            // Notify listeners
            onFreqModeChange(this, currentFrequency_, tciModeToFreeDv_(currentModulation_));
        }
    }
}

void TciRigController::handleTrx_(const std::vector<std::string>& args)
{
    if (args.size() >= 2)
    {
        int trx = std::stoi(args[0]);
        bool state = (args[1] == "true");
        
        if (trx == trx_)
        {
            pttState_ = state;
            onPttChange(this, state);
        }
    }
}

void TciRigController::sendCommand_(const std::string& cmdName, const std::vector<std::string>& args)
{
    std::string command = tci::CommandParser::buildCommand(cmdName, args);
    wsClient_->sendCommand(command);
}

tci::Modulation TciRigController::freeDvModeToTci_(Mode mode)
{
    // Map FreeDV modes to TCI modes
    // FreeDV ALWAYS uses USB for proper audio passthrough
    fprintf(stderr, "TCI MODE: Mapping FreeDV mode %d to USB (forced)\n", static_cast<int>(mode));
    
    // Always return USB regardless of input mode
    return tci::USB;
}

IRigFrequencyController::Mode TciRigController::tciModeToFreeDv_(tci::Modulation mod)
{
    // Map TCI modes back to FreeDV modes
    switch (mod)
    {
        case tci::DIGU:
            return DIGU;
            
        case tci::USB:
            return USB;
            
        case tci::DIGL:
            return DIGL;
            
        case tci::LSB:
            return LSB;
            
        case tci::NFM:
        case tci::WFM:
            return FM;
            
        case tci::AM:
        case tci::SAM:
        case tci::DSB:
        case tci::AM_SYNC:
            return AM;
            
        default:
            return UNKNOWN;
    }
}

void TciRigController::onConnected_()
{
    connected_ = true;
    
    // Request initial settings
    sendCommand_("audio_sample_rate", {std::to_string(48000)});
    
    // Note: audio_start command is sent by TciAudioDevice::start() when the audio
    // device is ready to receive data. Sending it here would cause audio packets
    // to arrive before the audio device is initialized, causing crashes.
    
    // Set initial mode to DIGU for FreeDV
    setMode(DIGU);
    
    // Notify listeners
    onRigConnected(this);
}

void TciRigController::onDisconnected_()
{
    bool wasConnected = connected_.exchange(false);
    
    if (wasConnected)
    {
        onRigDisconnected(this);
    }
}

void TciRigController::onError_(const std::string& error)
{
    onRigError(this, error);
}
