//=========================================================================
// Name:            TciAudioDevice.h
// Purpose:         Audio device for TCI protocol (RX and TX audio streams)
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

#ifndef TCI_AUDIO_DEVICE_H
#define TCI_AUDIO_DEVICE_H

#include "IAudioDevice.h"
#include "../rig_control/TciWebSocketClient.h"
#include "../rig_control/TciProtocol.h"
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <atomic>
#include <queue>

class TciAudioDevice : public IAudioDevice, public std::enable_shared_from_this<TciAudioDevice>
{
public:
    TciAudioDevice(std::shared_ptr<tci::TciWebSocketClient> wsClient, int trx = 0);
    virtual ~TciAudioDevice();
    
    // Must be called after construction to register callbacks
    void initialize();
    
    // IAudioDevice interface
    virtual int getNumChannels() FREEDV_NONBLOCKING override;
    virtual int getSampleRate() const FREEDV_NONBLOCKING override;
    virtual void start() override;
    virtual void stop() override;
    virtual bool isRunning() override;
    virtual int64_t getLatencyInMicroseconds() override;
    
    // Real-time helper - use shorter sleep when there's data waiting
    virtual void stopRealTimeWork(bool fastMode = false) override;
    
    // TCI-specific methods
    void setTrx(int trx);
    int getTrx() const;
    
    // Enqueue TX audio from FreeDV for transmission via TCI
    void enqueueTxAudio(const short* samples, size_t numSamples);
    
private:
    std::shared_ptr<tci::TciWebSocketClient> wsClient_;
    int trx_;
    int sampleRate_;
    int numChannels_;
    std::atomic<bool> running_;
    std::atomic<bool> shouldStop_;
    
    // TX audio buffering
    std::mutex txMutex_;
    std::queue<std::vector<short>> txQueue_;
    std::condition_variable txCv_;
    std::unique_ptr<std::thread> txThread_;
    
    // RX audio buffering
    std::mutex rxMutex_;
    std::condition_variable rxCv_;
    std::vector<short> rxBuffer_;
    std::unique_ptr<std::thread> rxThread_;
    
    // Stream handler
    void handleStream_(const tci::StreamHeader& header, const uint8_t* data, size_t dataSize);
    
    // TX thread function
    void txThreadFunc_();
    
    // RX thread function
    void rxThreadFunc_();
    
    // Audio format conversion
    void convertInt16ToShort_(const uint8_t* data, size_t numSamples, std::vector<short>& output);
    void convertInt24ToShort_(const uint8_t* data, size_t numSamples, std::vector<short>& output);
    void convertInt32ToShort_(const uint8_t* data, size_t numSamples, std::vector<short>& output);
    void convertFloat32ToShort_(const uint8_t* data, size_t numSamples, std::vector<short>& output);
    
    // Send TX audio stream
    void sendTxAudio_(const short* samples, size_t numSamples);
    
    // Latency tracking
    std::chrono::steady_clock::time_point lastRxTime_;
    int64_t estimatedLatency_;
};

#endif // TCI_AUDIO_DEVICE_H
