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
    
    // Enable RX audio streaming from TCI server
    std::string cmd = "audio_start:" + std::to_string(trx_) + ";";
    wsClient_->sendCommand(cmd);
    fprintf(stderr, "TCI Audio: Sent %s to enable RX audio streaming\n", cmd.c_str());
    fflush(stderr);
    
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
    
    // Disable RX audio streaming from TCI server (only if still connected)
    if (wsClient_ && wsClient_->isConnected())
    {
        std::string cmd = "audio_stop:" + std::to_string(trx_) + ";";
        wsClient_->sendCommand(cmd);
        fprintf(stderr, "TCI Audio: Sent %s to disable RX audio streaming\n", cmd.c_str());
        fflush(stderr);
    }
    
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

void TciAudioDevice::stopRealTimeWork(bool fastMode)
{
    // For TCI, we want to process data quickly to keep up with the stream.
    // When fastMode is true (output FIFO needs data OR input is backing up),
    // use a very short sleep. When false, use a slightly longer sleep.
    // Note: Even in fast mode we need SOME sleep to avoid spinning the CPU
    // when waiting for output FIFO to drain.
    if (fastMode)
    {
        // Very short sleep - quick response but not CPU-spinning
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    else
    {
        // Short sleep when everything is stable
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
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
    
    // Limit TX queue size to prevent memory exhaustion (max 10 seconds worth)
    const size_t MAX_QUEUE_CHUNKS = 500;  // ~10s at 20ms chunks
    
    if (txQueue_.size() >= MAX_QUEUE_CHUNKS)
    {
        // TX queue overflow - drop oldest chunk
        txQueue_.pop();
        fprintf(stderr, "TCI Audio: TX queue overflow, dropped oldest chunk\n");
        fflush(stderr);
    }
    
    txQueue_.push(std::vector<short>(samples, samples + numSamples));
    txCv_.notify_one();
}

void TciAudioDevice::handleStream_(const tci::StreamHeader& header, const uint8_t* data, size_t dataSize __attribute__((unused)))
{
    // Debug: log all incoming streams
    static int streamCount = 0;
    if (++streamCount <= 5 || streamCount % 100 == 0)
    {
        fprintf(stderr, "TCI Audio: handleStream_ called - type=%u, receiver=%u, length=%u, format=%u, rate=%u (our trx=%d)\n",
                header.type, header.receiver, header.length, header.format, header.sample_rate, trx_);
        fflush(stderr);
    }
    
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
        
        // Debug: log sample conversion
        static int convCount = 0;
        if (++convCount <= 5)
        {
            fprintf(stderr, "TCI Audio: Converted %zu samples, first few: %d %d %d %d\n",
                    samples.size(),
                    samples.size() > 0 ? samples[0] : 0,
                    samples.size() > 1 ? samples[1] : 0,
                    samples.size() > 2 ? samples[2] : 0,
                    samples.size() > 3 ? samples[3] : 0);
            fflush(stderr);
        }
        
        // Add to RX buffer with overflow protection
        {
            std::lock_guard<std::mutex> lock(rxMutex_);
            
            // Limit buffer size to prevent memory exhaustion (max 5 seconds at 48kHz)
            const size_t MAX_BUFFER_SIZE = 48000 * 5;
            
            if (rxBuffer_.size() + samples.size() > MAX_BUFFER_SIZE)
            {
                // Buffer overflow - drop oldest samples to make room
                size_t overflow = (rxBuffer_.size() + samples.size()) - MAX_BUFFER_SIZE;
                if (overflow >= rxBuffer_.size())
                {
                    rxBuffer_.clear();
                }
                else
                {
                    rxBuffer_.erase(rxBuffer_.begin(), rxBuffer_.begin() + overflow);
                }
                fprintf(stderr, "TCI Audio: RX buffer overflow, dropped %zu samples\n", overflow);
                fflush(stderr);
            }
            
            rxBuffer_.insert(rxBuffer_.end(), samples.begin(), samples.end());
            
            // Debug: log buffer status
            static int bufCount = 0;
            if (++bufCount <= 5 || bufCount % 50 == 0)
            {
                fprintf(stderr, "TCI Audio: RX buffer now has %zu samples (added %zu)\n",
                        rxBuffer_.size(), samples.size());
                fflush(stderr);
            }
            
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
    
    fprintf(stderr, "TCI Audio: RX thread started, onAudioDataFunction=%p\n", (void*)onAudioDataFunction);
    fflush(stderr);
    
    while (!shouldStop_)
    {
        std::vector<short> samples;
        
        {
            std::unique_lock<std::mutex> lock(rxMutex_);
            
            // Wait for data or stop signal (with timeout to prevent indefinite blocking)
            bool hasData = rxCv_.wait_for(lock, std::chrono::milliseconds(100), [this, chunkSize]() {
                return shouldStop_ || rxBuffer_.size() >= chunkSize;
            });
            
            if (shouldStop_)
            {
                fprintf(stderr, "TCI Audio: RX thread stopping\n");
                fflush(stderr);
                break;
            }
            
            if (!hasData || rxBuffer_.size() < chunkSize)
            {
                // Timeout or not enough data yet
                continue;
            }
            
            // Debug: show buffer status periodically
            static int rxCount = 0;
            if (++rxCount <= 5 || rxCount % 50 == 0)
            {
                fprintf(stderr, "TCI Audio: RX thread processing, buffer size=%zu, extracting %zu samples\n", 
                        rxBuffer_.size(), chunkSize);
                fflush(stderr);
            }
            
            if (rxBuffer_.size() >= chunkSize)
            {
                samples.assign(rxBuffer_.begin(), rxBuffer_.begin() + chunkSize);
                rxBuffer_.erase(rxBuffer_.begin(), rxBuffer_.begin() + chunkSize);
            }
        }
        
        if (!samples.empty() && onAudioDataFunction)
        {
            static int callbackCount = 0;
            if (++callbackCount <= 5 || callbackCount % 50 == 0)
            {
                fprintf(stderr, "TCI Audio: Calling onAudioDataFunction with %zu samples\n", samples.size());
                fflush(stderr);
            }
            // Pass sample count, NOT byte count!
            onAudioDataFunction(*this, samples.data(), samples.size(), onAudioDataState);
        }
        else if (!samples.empty() && !onAudioDataFunction)
        {
            static bool warned = false;
            if (!warned)
            {
                fprintf(stderr, "TCI Audio: WARNING - onAudioDataFunction is NULL, audio will be lost!\n");
                fflush(stderr);
                warned = true;
            }
        }
    }
}

void TciAudioDevice::txThreadFunc_()
{
    // This thread is for handling TX audio that comes from FreeDV
    // and needs to be queued for transmission via TCI
    
    fprintf(stderr, "TCI Audio: TX thread started\n");
    fflush(stderr);
    
    while (!shouldStop_)
    {
        // Wait for TX audio from FreeDV or stop signal
        // In this implementation, FreeDV will call onAudioDataFunction with TX data
        // which we'll queue up. The actual sending happens in response to TX_CHRONO.
        
        std::unique_lock<std::mutex> lock(txMutex_);
        txCv_.wait_for(lock, std::chrono::milliseconds(100), [this]() {
            return shouldStop_ || !txQueue_.empty();
        });
        
        if (shouldStop_)
        {
            fprintf(stderr, "TCI Audio: TX thread stopping\n");
            fflush(stderr);
            break;
        }
    }
    
    fprintf(stderr, "TCI Audio: TX thread exited\n");
    fflush(stderr);
}

void TciAudioDevice::convertInt16ToShort_(const uint8_t* data, size_t numSamples, std::vector<short>& output)
{
    const short* samples = reinterpret_cast<const short*>(data);
    
    // Note: numSamples is total samples including all channels
    // For stereo (2 channels), we convert LRLRLR... pairs to mono by averaging
    // Assuming stereo for TCI audio (channels = 2)
    const int channels = 2;
    size_t monoSamples = numSamples / channels;
    
    output.resize(monoSamples);
    
    for (size_t i = 0; i < monoSamples; ++i)
    {
        // Average left and right channels
        int32_t left = samples[i * 2];
        int32_t right = samples[i * 2 + 1];
        output[i] = static_cast<short>((left + right) / 2);
    }
}

void TciAudioDevice::convertInt24ToShort_(const uint8_t* data, size_t numSamples, std::vector<short>& output)
{
    // Note: numSamples is total samples including all channels
    // For stereo (2 channels), we convert LRLRLR... pairs to mono by averaging
    const int channels = 2;
    size_t monoSamples = numSamples / channels;
    
    output.resize(monoSamples);
    
    for (size_t i = 0; i < monoSamples; ++i)
    {
        // Convert left channel 24-bit to 16-bit
        int32_t left24 = (static_cast<int32_t>(data[i * 6 + 2]) << 16) |
                         (static_cast<int32_t>(data[i * 6 + 1]) << 8) |
                         (static_cast<int32_t>(data[i * 6 + 0]));
        if (left24 & 0x00800000) left24 |= 0xFF000000;
        
        // Convert right channel 24-bit to 16-bit
        int32_t right24 = (static_cast<int32_t>(data[i * 6 + 5]) << 16) |
                          (static_cast<int32_t>(data[i * 6 + 4]) << 8) |
                          (static_cast<int32_t>(data[i * 6 + 3]));
        if (right24 & 0x00800000) right24 |= 0xFF000000;
        
        // Average and shift down to 16-bit range
        int32_t avg = (left24 + right24) / 2;
        output[i] = static_cast<short>(avg >> 8);
    }
}

void TciAudioDevice::convertInt32ToShort_(const uint8_t* data, size_t numSamples, std::vector<short>& output)
{
    const int32_t* samples32 = reinterpret_cast<const int32_t*>(data);
    
    // Note: numSamples is total samples including all channels
    // For stereo (2 channels), we convert LRLRLR... pairs to mono by averaging
    const int channels = 2;
    size_t monoSamples = numSamples / channels;
    
    output.resize(monoSamples);
    
    for (size_t i = 0; i < monoSamples; ++i)
    {
        // Average left and right channels, then shift down from 32-bit to 16-bit range
        int64_t left = samples32[i * 2];
        int64_t right = samples32[i * 2 + 1];
        output[i] = static_cast<short>((left + right) / 2 >> 16);
    }
}

void TciAudioDevice::convertFloat32ToShort_(const uint8_t* data, size_t numSamples, std::vector<short>& output)
{
    const float* samplesFloat = reinterpret_cast<const float*>(data);
    
    // Note: numSamples is total samples including all channels
    // For stereo (2 channels), we convert LRLRLR... pairs to mono by averaging
    const int channels = 2;
    size_t monoSamples = numSamples / channels;
    
    output.resize(monoSamples);
    
    for (size_t i = 0; i < monoSamples; ++i)
    {
        // Average left and right channels
        float left = samplesFloat[i * 2];
        float right = samplesFloat[i * 2 + 1];
        float avg = (left + right) / 2.0f;
        
        // Convert float [-1.0, 1.0] to short [-32768, 32767]
        avg = std::max(-1.0f, std::min(1.0f, avg));
        output[i] = static_cast<short>(avg * 32767.0f);
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
