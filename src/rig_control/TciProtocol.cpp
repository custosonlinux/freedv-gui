//=========================================================================
// Name:            TciProtocol.cpp
// Purpose:         TCI Protocol parser implementation
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

#include "TciProtocol.h"
#include <algorithm>
#include <sstream>
#include <cctype>

namespace tci
{
    CommandParser::CommandParser()
    {
    }
    
    bool CommandParser::parseCommand(const std::string& command, std::string& cmdName, std::vector<std::string>& args)
    {
        // TCI command format: COMMAND:arg1,arg2,arg3;
        // or just: COMMAND;
        
        if (command.empty() || command.back() != ';')
        {
            return false; // Commands must end with semicolon
        }
        
        // Remove the trailing semicolon
        std::string cmd = command.substr(0, command.length() - 1);
        
        // Find the colon separator
        size_t colonPos = cmd.find(':');
        
        if (colonPos == std::string::npos)
        {
            // No arguments, just command name
            cmdName = cmd;
            trimString_(cmdName);
            
            // Convert to uppercase for case-insensitive comparison
            std::transform(cmdName.begin(), cmdName.end(), cmdName.begin(), ::toupper);
            
            args.clear();
            return !cmdName.empty();
        }
        
        // Extract command name
        cmdName = cmd.substr(0, colonPos);
        trimString_(cmdName);
        std::transform(cmdName.begin(), cmdName.end(), cmdName.begin(), ::toupper);
        
        // Extract and split arguments
        std::string argString = cmd.substr(colonPos + 1);
        args = splitArgs(argString);
        
        return !cmdName.empty();
    }
    
    std::string CommandParser::buildCommand(const std::string& cmdName, const std::vector<std::string>& args)
    {
        std::ostringstream oss;
        oss << cmdName;
        
        if (!args.empty())
        {
            oss << ":";
            for (size_t i = 0; i < args.size(); ++i)
            {
                if (i > 0) oss << ",";
                oss << args[i];
            }
        }
        
        oss << ";";
        return oss.str();
    }
    
    std::vector<std::string> CommandParser::splitArgs(const std::string& argString)
    {
        std::vector<std::string> result;
        std::string current;
        
        for (char ch : argString)
        {
            if (ch == ',')
            {
                result.push_back(current);
                current.clear();
            }
            else
            {
                current += ch;
            }
        }
        
        if (!current.empty())
        {
            result.push_back(current);
        }
        
        return result;
    }
    
    Modulation CommandParser::stringToModulation(const std::string& modStr)
    {
        std::string upper = modStr;
        std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
        
        if (upper == "AM") return AM;
        if (upper == "SAM") return SAM;
        if (upper == "DSB") return DSB;
        if (upper == "LSB") return LSB;
        if (upper == "USB") return USB;
        if (upper == "CW-L" || upper == "CW_L") return CW_L;
        if (upper == "CW-U" || upper == "CW_U") return CW_U;
        if (upper == "NFM") return NFM;
        if (upper == "WFM") return WFM;
        if (upper == "SPEC") return SPEC;
        if (upper == "DIGL") return DIGL;
        if (upper == "DIGU") return DIGU;
        if (upper == "AM-SYNC" || upper == "AM_SYNC") return AM_SYNC;
        if (upper == "DRM") return DRM;
        
        return AM; // default
    }
    
    std::string CommandParser::modulationToString(Modulation mod)
    {
        switch (mod)
        {
            case AM: return "am";
            case SAM: return "sam";
            case DSB: return "dsb";
            case LSB: return "lsb";
            case USB: return "usb";
            case CW_L: return "cw-l";
            case CW_U: return "cw-u";
            case NFM: return "nfm";
            case WFM: return "wfm";
            case SPEC: return "spec";
            case DIGL: return "digl";
            case DIGU: return "digu";
            case AM_SYNC: return "am-sync";
            case DRM: return "drm";
            default: return "am";
        }
    }
    
    void CommandParser::trimString_(std::string& str)
    {
        // Trim leading whitespace
        str.erase(str.begin(), std::find_if(str.begin(), str.end(), [](unsigned char ch) {
            return !std::isspace(ch);
        }));
        
        // Trim trailing whitespace
        str.erase(std::find_if(str.rbegin(), str.rend(), [](unsigned char ch) {
            return !std::isspace(ch);
        }).base(), str.end());
    }
}
