//=========================================================================
// Name:            TciAudioDevice.cpp
// Purpose:         Audio device for TCI protocol implementation
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

#include "TciAudioDevice.h"
#include <cstring>
#include <algorithm>
#include <iostream>
#include <cmath>

TciAudioDevice::TciAudioDevice(std::shared_ptr<tci::TciWebSocketClient> wsClient, int trx)
    : wsClient_(wsClient)
    , trx_(trx)
    , sampleRate_(48000)  // TCI uses 48 kHz for FreeDV
    , numChannels_(1)      // Mono
    , running_(false)
    , shouldStop_(false)
    , estimatedLatency_(100000)  // Default 100ms
{
    // Callback registration moved to initialize() to use weak_ptr safely
}

void TciAudioDevice::initialize()
{
    // Use weak_ptr to prevent use-after-free when device is destroyed
    std::weak_ptr<TciAudioDevice> weakThis = shared_from_this();
    
    wsClient_->setStreamCallback([weakThis](const tci::StreamHeader& header, const uint8_t* data, size_t dataSize) {
        auto strongThis = weakThis.lock();
        if (strongThis) {
            strongThis->handleStream_(header, data, dataSize);
        }
    });
}

TciAudioDevice::~TciAudioDevice()
{
    // Stop threads first
    stop();
    
    // Clear the stream callback to prevent future invocations
    if (wsClient_)
    {
        wsClient_->setStreamCallback(nullptr);
        
        // Small delay to ensure any in-flight callbacks complete
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

int TciAudioDevice::getNumChannels() FREEDV_NONBLOCKING
{
    return numChannels_;
}

int TciAudioDevice::getSampleRate() const FREEDV_NONBLOCKING
{
    return sampleRate_;
}

void TciAudioDevice::start()
{
    if (running_)
    {
        return;
    }
    
    shouldStop_ = false;
    running_ = true;
    
    // Start RX thread
    rxThread_ = std::make_unique<std::thread>(&TciAudioDevice::rxThreadFunc_, this);
    
    // Start TX thread
    txThread_ = std::make_unique<std::thread>(&TciAudioDevice::txThreadFunc_, this);
}

void TciAudioDevice::stop()
{
    if (!running_)
    {
        return;
    }
    
    shouldStop_ = true;
    running_ = false;
    
    // Wake up all threads
    rxCv_.notify_all();
    txCv_.notify_all();
    
    // Wait for threads to finish
    if (rxThread_ && rxThread_->joinable())
    {
        rxThread_->join();
    }
    
    if (txThread_ && txThread_->joinable())
    {
        txThread_->join();
    }
    
    // Clear buffers
    {
        std::lock_guard<std::mutex> lock(txMutex_);
        while (!txQueue_.empty())
        {
            txQueue_.pop();
        }
    }
    
    {
        std::lock_guard<std::mutex> lock(rxMutex_);
        rxBuffer_.clear();
    }
}

bool TciAudioDevice::isRunning()
{
    return running_;
}

int64_t TciAudioDevice::getLatencyInMicroseconds()
{
    return estimatedLatency_;
}

void TciAudioDevice::setTrx(int trx)
{
    trx_ = trx;
}

int TciAudioDevice::getTrx() const
{
    return trx_;
}

void TciAudioDevice::enqueueTxAudio(const short* samples, size_t numSamples)
{
    if (!samples || numSamples == 0)
        return;
    
    std::lock_guard<std::mutex> lock(txMutex_);
    txQueue_.push(std::vector<short>(samples, samples + numSamples));
    txCv_.notify_one();
}

void TciAudioDevice::handleStream_(const tci::StreamHeader& header, const uint8_t* data, size_t dataSize __attribute__((unused)))
{
    // Only process streams for our TRX
    if (static_cast<int>(header.receiver) != trx_)
    {
        return;
    }
    
    if (header.type == tci::RX_AUDIO_STREAM)
    {
        // Convert received audio to short format
        std::vector<short> samples;
        
        switch (header.format)
        {
            case tci::INT16:
                convertInt16ToShort_(data, header.length, samples);
                break;
                
            case tci::INT24:
                convertInt24ToShort_(data, header.length, samples);
                break;
                
            case tci::INT32:
                convertInt32ToShort_(data, header.length, samples);
                break;
                
            case tci::FLOAT32:
                convertFloat32ToShort_(data, header.length, samples);
                break;
                
            default:
                return;
        }
        
        // Add to RX buffer
        {
            std::lock_guard<std::mutex> lock(rxMutex_);
            rxBuffer_.insert(rxBuffer_.end(), samples.begin(), samples.end());
            lastRxTime_ = std::chrono::steady_clock::now();
        }
        
        // Wake up RX thread
        rxCv_.notify_one();
    }
    else if (header.type == tci::TX_CHRONO)
    {
        // TCI is requesting TX audio
        // The length field indicates how many samples are needed
        uint32_t samplesNeeded = header.length;
        
        // Get samples from TX queue
        std::vector<short> txSamples;
        
        {
            std::lock_guard<std::mutex> lock(txMutex_);
            
            // Collect requested number of samples from queue
            while (!txQueue_.empty() && txSamples.size() < samplesNeeded)
            {
                auto& chunk = txQueue_.front();
                size_t toCopy = std::min(samplesNeeded - txSamples.size(), chunk.size());
                
                txSamples.insert(txSamples.end(), chunk.begin(), chunk.begin() + toCopy);
                
                if (toCopy == chunk.size())
                {
                    txQueue_.pop();
                }
                else
                {
                    // Partial chunk consumed, keep remainder
                    chunk.erase(chunk.begin(), chunk.begin() + toCopy);
                    break;
                }
            }
            
            // If we don't have enough samples, pad with silence
            if (txSamples.size() < samplesNeeded)
            {
                txSamples.resize(samplesNeeded, 0);
            }
        }
        
        // Send TX audio
        sendTxAudio_(txSamples.data(), txSamples.size());
    }
}

void TciAudioDevice::rxThreadFunc_()
{
    const size_t chunkSize = 1024;  // Process 1024 samples at a time
    
    while (!shouldStop_)
    {
        std::vector<short> samples;
        
        {
            std::unique_lock<std::mutex> lock(rxMutex_);
            
            // Wait for data or stop signal
            rxCv_.wait_for(lock, std::chrono::milliseconds(100), [this, chunkSize]() {
                return shouldStop_ || rxBuffer_.size() >= chunkSize;
            });
            
            if (shouldStop_)
                break;
            
            if (rxBuffer_.size() >= chunkSize)
            {
                samples.assign(rxBuffer_.begin(), rxBuffer_.begin() + chunkSize);
                rxBuffer_.erase(rxBuffer_.begin(), rxBuffer_.begin() + chunkSize);
            }
        }
        
        if (!samples.empty() && onAudioDataFunction)
        {
            onAudioDataFunction(*this, samples.data(), samples.size() * sizeof(short), onAudioDataState);
        }
    }
}

void TciAudioDevice::txThreadFunc_()
{
    // This thread is for handling TX audio that comes from FreeDV
    // and needs to be queued for transmission via TCI
    
    while (!shouldStop_)
    {
        // Wait for TX audio from FreeDV
        // In this implementation, FreeDV will call onAudioDataFunction with TX data
        // which we'll queue up. The actual sending happens in response to TX_CHRONO.
        
        std::unique_lock<std::mutex> lock(txMutex_);
        txCv_.wait_for(lock, std::chrono::milliseconds(100));
    }
}

void TciAudioDevice::convertInt16ToShort_(const uint8_t* data, size_t numSamples, std::vector<short>& output)
{
    output.resize(numSamples);
    std::memcpy(output.data(), data, numSamples * sizeof(short));
}

void TciAudioDevice::convertInt24ToShort_(const uint8_t* data, size_t numSamples, std::vector<short>& output)
{
    output.resize(numSamples);
    
    for (size_t i = 0; i < numSamples; ++i)
    {
        // Convert 24-bit to 16-bit by taking the upper 16 bits
        int32_t sample24 = (static_cast<int32_t>(data[i * 3 + 2]) << 16) |
                           (static_cast<int32_t>(data[i * 3 + 1]) << 8) |
                           (static_cast<int32_t>(data[i * 3 + 0]));
        
        // Sign extend if negative
        if (sample24 & 0x00800000)
        {
            sample24 |= 0xFF000000;
        }
        
        // Shift down to 16-bit range
        output[i] = static_cast<short>(sample24 >> 8);
    }
}

void TciAudioDevice::convertInt32ToShort_(const uint8_t* data, size_t numSamples, std::vector<short>& output)
{
    output.resize(numSamples);
    const int32_t* samples32 = reinterpret_cast<const int32_t*>(data);
    
    for (size_t i = 0; i < numSamples; ++i)
    {
        // Shift down to 16-bit range
        output[i] = static_cast<short>(samples32[i] >> 16);
    }
}

void TciAudioDevice::convertFloat32ToShort_(const uint8_t* data, size_t numSamples, std::vector<short>& output)
{
    output.resize(numSamples);
    const float* samplesFloat = reinterpret_cast<const float*>(data);
    
    for (size_t i = 0; i < numSamples; ++i)
    {
        // Clamp and convert float [-1.0, 1.0] to short [-32768, 32767]
        float sample = std::max(-1.0f, std::min(1.0f, samplesFloat[i]));
        output[i] = static_cast<short>(sample * 32767.0f);
    }
}

void TciAudioDevice::sendTxAudio_(const short* samples, size_t numSamples)
{
    // Build TX_AUDIO_STREAM packet
    tci::StreamHeader header;
    std::memset(&header, 0, sizeof(header));
    
    header.receiver = trx_;
    header.sample_rate = sampleRate_;
    header.format = tci::INT16;
    header.codec = 0;
    header.crc = 0;
    header.length = numSamples;
    header.type = tci::TX_AUDIO_STREAM;
    header.channels = numChannels_;
    
    // Prepare packet: header + audio data
    size_t packetSize = sizeof(tci::StreamHeader) + (numSamples * sizeof(short));
    std::vector<uint8_t> packet(packetSize);
    
    // Copy header
    std::memcpy(packet.data(), &header, sizeof(header));
    
    // Copy audio data
    std::memcpy(packet.data() + sizeof(header), samples, numSamples * sizeof(short));
    
    // Send via WebSocket
    wsClient_->sendBinaryData(packet.data(), packet.size());
}
