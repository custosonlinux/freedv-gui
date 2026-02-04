# TCI Protocol Integration for FreeDV

This document describes the TCI (Transceiver Control Interface) integration added to FreeDV-GUI.

## Overview

TCI is a modern network-based protocol developed by Expert Electronics for controlling SDR radios and software like ExpertSDR3. This integration allows FreeDV to:

- Connect to TCI-compatible radios over TCP/IP network
- Control frequency, mode, and PTT via TCI commands
- Stream RX audio from the radio for FreeDV demodulation
- Stream TX audio to the radio for FreeDV modulation
- Operate at 48 kHz sample rate (FreeDV native)

## Architecture

### Components

1. **TciProtocol** (TciProtocol.h/cpp)
   - Protocol constants and enums
   - Command parser for ASCII TCI commands
   - Stream header structure for binary audio data

2. **TciWebSocketClient** (TciWebSocketClient.h/cpp)
   - TCP socket client (POSIX sockets)
   - Handles connection to TCI server
   - Sends/receives commands and audio streams
   - Callback-based architecture

3. **TciRigController** (TciRigController.h/cpp)
   - Implements `IRigFrequencyController` and `IRigPttController`
   - Handles TCI command protocol
   - Maps FreeDV modes to TCI modes (DIGL/DIGU)
   - Manages frequency and PTT control

4. **TciAudioDevice** (TciAudioDevice.h/cpp)
   - Implements `IAudioDevice` interface
   - Handles RX_AUDIO_STREAM from TCI
   - Sends TX_AUDIO_STREAM to TCI
   - Responds to TX_CHRONO timing requests
   - Audio format conversion (INT16/INT24/INT32/FLOAT32 to short)

## Data Flow

### Initialization
```
FreeDV connects to TCI server (hostname:port)
    ↓
TCI sends initialization commands (PROTOCOL, DEVICE, MODULATIONS_LIST, etc.)
    ↓
FreeDV requests 48 kHz audio: AUDIO_SAMPLE_RATE:48000;
    ↓
FreeDV starts audio streaming: AUDIO_START:0;
    ↓
FreeDV sets mode to DIGU: MODULATION:0,digu;
    ↓
Ready for RX/TX
```

### Receive (RX)
```
Radio receives RF signal
    ↓
TCI server demodulates audio in DIGU mode
    ↓
TCI sends RX_AUDIO_STREAM binary blocks
    ↓
TciAudioDevice converts samples to short[]
    ↓
FreeDV pipeline processes audio
    ↓
Decoded voice/data output
```

### Transmit (TX)
```
User presses PTT
    ↓
TciRigController sends: TRX:0,true,tci;
    ↓
TCI server requests audio: TX_CHRONO with sample count
    ↓
TciAudioDevice reads samples from TX buffer
    ↓
Sends TX_AUDIO_STREAM binary block
    ↓
TCI server modulates and transmits RF
    ↓
Cycle repeats until PTT released
```

## TCI Protocol Details

### Command Format
```
COMMAND:arg1,arg2,arg3;
```

Commands are ASCII strings ending with semicolon.

### Key Commands Used

- `VFO:trx,channel,frequency;` - Set/get frequency
- `MODULATION:trx,mode;` - Set/get mode (digl, digu, usb, lsb, etc.)
- `TRX:trx,state,audio_source;` - PTT control (state=true/false, audio_source=tci)
- `AUDIO_SAMPLE_RATE:rate;` - Set audio sample rate (48000)
- `AUDIO_START:trx;` - Start audio streaming
- `AUDIO_STOP:trx;` - Stop audio streaming

### Audio Stream Format

Binary blocks with header:

```c
struct StreamHeader {
    uint32_t receiver;      // TRX number
    uint32_t sample_rate;   // 48000 Hz
    uint32_t format;        // INT16=0, INT24=1, INT32=2, FLOAT32=3
    uint32_t codec;         // Always 0
    uint32_t crc;           // Always 0
    uint32_t length;        // Number of samples
    uint32_t type;          // RX_AUDIO_STREAM=1, TX_AUDIO_STREAM=2, TX_CHRONO=3
    uint32_t channels;      // 1 (mono)
    uint32_t reserv[8];     // Reserved
    // uint8_t data[] follows
};
```

### Mode Mapping

FreeDV Mode → TCI Mode:
- USB/DIGU → digu (digital upper sideband)
- LSB/DIGL → digl (digital lower sideband)
- FM/DIGFM → nfm (narrow FM)
- AM → am (amplitude modulation)

## Configuration

### TCI Server Settings

- **Hostname**: IP address or hostname of TCI server (default: 127.0.0.1)
- **Port**: TCP port (default: 40001)
- **TRX**: Transceiver number (default: 0)
- **Channel**: VFO/channel number (default: 0)

### FreeDV Settings

1. Select "TCI Protocol" in Tools → Radio Control
2. Enter TCI server hostname and port
3. Select TCI audio device for RX/TX
4. Set mode to USB or LSB (will auto-map to DIGU/DIGL)

## Building

The TCI integration is automatically included when building FreeDV. No additional dependencies are required beyond standard POSIX sockets.

### Build Requirements

- C++11 compiler
- POSIX sockets (Linux/macOS/BSD)
- Standard FreeDV dependencies

### Build Commands

```bash
cd freedv-gui
mkdir build && cd build
cmake ..
make -j$(nproc)
```

## Testing

### With ExpertSDR3

1. Launch ExpertSDR3
2. Enable TCI server in Settings → TCI
3. Launch FreeDV
4. Select TCI Protocol in radio control
4. Connect to 127.0.0.1:40001
6. Select TCI audio device
7. Test PTT and audio

### With Other TCI Servers

Any TCI 1.9+ compatible server should work. Ensure:
- Server supports 48 kHz audio sample rate
- DIGL/DIGU modes are available
- TRX command accepts "tci" audio source

## Troubleshooting

### Connection Issues

- **Cannot connect**: Check TCI server is running and port is correct
- **Connection drops**: Check network stability, firewall rules
- **Wrong port**: Default is 50001, may vary by server

### Audio Issues

- **No RX audio**: Verify AUDIO_START sent, check sample rate (48000)
- **No TX audio**: Check PTT control, verify TRX command with "tci" source
- **Garbled audio**: Check audio format conversion, sample rate mismatch

### PTT Issues

- **PTT stuck on**: Check TRX:0,false,tci; command sent on disconnect
- **PTT not working**: Verify "tci" audio source in TRX command
- **Delayed PTT**: Normal, TCI has ~100ms response time

### Debug Logging

Enable FreeDV debug logging to see TCI commands:
```
export FREEDV_DEBUG=1
./freedv
```

## Limitations

- Currently supports single TRX/channel (configurable but not in GUI yet)
- Assumes mono audio (1 channel)
- No IQ stream support (uses demodulated audio only)
- POSIX sockets only (Linux/macOS/BSD, not Windows yet)

## Future Enhancements

1. **GUI Configuration**: Add TCI settings to Audio Config dialog
2. **Multi-Channel**: Support multiple receivers/channels
3. **Windows Support**: Port to Winsock or use cross-platform WebSocket library
4. **IQ Streams**: Direct IQ data handling for SDR modes
5. **Auto-Discovery**: Detect TCI servers on local network
6. **Reconnection**: Auto-reconnect on connection loss

## References

- [TCI Protocol Specification v2.0](https://eesdr.com/en/)
- [ExpertSDR3 Documentation](https://eesdr.com/en/)
- [FreeDV Website](https://freedv.org/)

## License

Same as FreeDV-GUI: GNU GPL v2.1

## Authors

- Tomas Ostojic - TCI integration implementation (February 2026)
- FreeDV Team - Original FreeDV-GUI architecture

## Support

For issues or questions:
- FreeDV mailing list
- GitHub issues: https://github.com/drowe67/freedv-gui/issues
- TCI protocol questions: info@sunsdr.com
