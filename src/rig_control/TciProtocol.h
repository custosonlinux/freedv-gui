//=========================================================================
// Name:            TciProtocol.h
// Purpose:         TCI Protocol constants, enums and parser
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

#ifndef TCI_PROTOCOL_H
#define TCI_PROTOCOL_H

#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <map>

namespace tci
{
    // TCI Audio Stream types
    enum StreamType
    {
        IQ_STREAM = 0,
        RX_AUDIO_STREAM = 1,
        TX_AUDIO_STREAM = 2,
        TX_CHRONO = 3,
        LINEOUT_STREAM = 4
    };
    
    // TCI Audio Sample formats
    enum SampleFormat
    {
        INT16 = 0,
        INT24 = 1,
        INT32 = 2,
        FLOAT32 = 3
    };
    
    // TCI Modulation modes
    enum Modulation
    {
        AM = 0,
        SAM,
        DSB,
        LSB,
        USB,
        CW_L,
        CW_U,
        NFM,
        WFM,
        SPEC,
        DIGL,
        DIGU,
        AM_SYNC,
        DRM
    };
    
    // TCI Audio stream header structure (matches TCI spec)
    #pragma pack(push, 1)
    struct StreamHeader
    {
        uint32_t receiver;      // receiver number
        uint32_t sample_rate;   // sampling rate
        uint32_t format;        // sample type (SampleFormat enum)
        uint32_t codec;         // compression algorithm (not implemented), always 0
        uint32_t crc;           // checksum (not implemented), always 0
        uint32_t length;        // number of samples
        uint32_t type;          // stream type (StreamType enum)
        uint32_t channels;      // number of channels
        uint32_t reserv[8];     // reserved
        // uint8_t data[] follows immediately after header
    };
    #pragma pack(pop)
    
    // Command parser class
    class CommandParser
    {
    public:
        CommandParser();
        
        // Parse a TCI command string
        // Returns true if command is valid
        bool parseCommand(const std::string& command, std::string& cmdName, std::vector<std::string>& args);
        
        // Build a TCI command string
        static std::string buildCommand(const std::string& cmdName, const std::vector<std::string>& args);
        
        // Helper to split CSV arguments
        static std::vector<std::string> splitArgs(const std::string& argString);
        
        // Convert modulation string to enum
        static Modulation stringToModulation(const std::string& modStr);
        
        // Convert modulation enum to string
        static std::string modulationToString(Modulation mod);
        
    private:
        void trimString_(std::string& str);
    };
    
    // Callback type for handling TCI commands
    using CommandCallback = std::function<void(const std::string& cmdName, const std::vector<std::string>& args)>;
    
    // Callback type for handling TCI audio streams
    using StreamCallback = std::function<void(const StreamHeader& header, const uint8_t* data, size_t dataSize)>;
}

#endif // TCI_PROTOCOL_H
