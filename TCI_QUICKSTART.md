# Quick Start Guide: Using TCI in FreeDV

## For Developers: GUI Integration Steps

The TCI backend is implemented and compiled. To make it available to users, follow these steps:

### Step 1: Add TciRigController to Radio Configuration

**File**: `src/config/RigControlConfiguration.h`

Add TCI enum value:
```cpp
enum class RigControlMethod {
    NONE = 0,
    HAMLIB,
    SERIAL_PORT,
    TCI,  // ADD THIS
};
```

### Step 2: Create TciRigController Instance

**File**: `src/ongui.cpp` or wherever rig control is initialized

```cpp
#include "rig_control/TciRigController.h"

// In initialization function:
if (rigControlMethod == RigControlMethod::TCI) {
    std::string hostname = "127.0.0.1";  // From config
    int port = 50001;                     // From config
    
    rigController = new TciRigController(hostname, port);
    rigController->connect();
}
```

### Step 3: Add TCI Audio Device

**File**: `src/audio/AudioEngineFactory.cpp`

```cpp
#include "audio/TciAudioDevice.h"
#include "rig_control/TciRigController.h"

// In factory method for creating audio devices:
if (useTciAudio && rigController) {
    auto tciRigController = dynamic_cast<TciRigController*>(rigController);
    if (tciRigController) {
        auto wsClient = tciRigController->getWebSocketClient();
        audioDevice = new TciAudioDevice(wsClient, 0);
    }
}
```

### Step 4: Add GUI Controls (wxWidgets)

**File**: Radio configuration dialog (e.g., `src/gui/dialogs/RadioConfigDialog.cpp`)

```cpp
// Add TCI fields
wxStaticText* tciHostnameLabel = new wxStaticText(panel, wxID_ANY, "TCI Hostname:");
wxTextCtrl* tciHostnameCtrl = new wxTextCtrl(panel, wxID_ANY, "127.0.0.1");

wxStaticText* tciPortLabel = new wxStaticText(panel, wxID_ANY, "TCI Port:");
wxSpinCtrl* tciPortCtrl = new wxSpinCtrl(panel, wxID_ANY, "40001", 
                                         wxDefaultPosition, wxDefaultSize,
                                         wxSP_ARROW_KEYS, 1, 65535, 50001);

// Add to sizer
sizer->Add(tciHostnameLabel, 0, wxALL, 5);
sizer->Add(tciHostnameCtrl, 0, wxALL | wxEXPAND, 5);
sizer->Add(tciPortLabel, 0, wxALL, 5);
sizer->Add(tciPortCtrl, 0, wxALL, 5);

// Show/hide based on radio type selection
void OnRadioTypeChanged(wxCommandEvent& event) {
    bool isTci = (radioTypeCombo->GetStringSelection() == "TCI Protocol");
    tciHostnameLabel->Show(isTci);
    tciHostnameCtrl->Show(isTci);
    tciPortLabel->Show(isTci);
    tciPortCtrl->Show(isTci);
    Layout();
}
```

## For Users: Quick Test (Programmatic)

Until GUI integration is complete, you can test by modifying `src/main.cpp`:

```cpp
#include "rig_control/TciRigController.h"
#include "audio/TciAudioDevice.h"

// In main() or initialization:
auto tciRig = new TciRigController("127.0.0.1", 50001);
tciRig->connect();

// Set up audio
auto wsClient = tciRig->getWebSocketClient();
auto tciAudio = new TciAudioDevice(wsClient, 0);

// Use tciRig and tciAudio as normal FreeDV rig/audio devices
```

## Testing with ExpertSDR3

### 1. Start ExpertSDR3
- Launch ExpertSDR3 software
- Go to Settings → TCI
- Enable TCI server
- Note the port (default: 50001)

### 2. Launch FreeDV with TCI
- Start FreeDV with TCI configured
- Should auto-connect to 127.0.0.1:50001

### 3. Verify Connection
- Check FreeDV status bar for "Connected"
- Change frequency in FreeDV → should change in ExpertSDR3
- Change frequency in ExpertSDR3 → should change in FreeDV

### 4. Test PTT
- Press PTT in FreeDV
- Should see TX indicator in ExpertSDR3
- Audio should flow from FreeDV to TCI

### 5. Test RX
- Tune to active frequency
- Should hear audio in FreeDV
- FreeDV should decode signals

## Configuration File Example

**File**: `~/.freedv/freedv.conf` or similar

```ini
[rig_control]
method=tci
tci_hostname=127.0.0.1
tci_port=40001
tci_trx=0
tci_channel=0

[audio]
input_device=tci
output_device=tci
```

## Minimal Integration Code

Here's a minimal example to get TCI working:

```cpp
// main.cpp or initialization
#include "rig_control/TciRigController.h"
#include "audio/TciAudioDevice.h"

// Create TCI rig controller
auto tciRig = std::make_unique<TciRigController>("127.0.0.1", 40001);

// Set up event handlers
tciRig->onRigConnected += [](IRigController* rig) {
    std::cout << "TCI connected!" << std::endl;
};

tciRig->onRigError += [](IRigController* rig, std::string const& error) {
    std::cerr << "TCI error: " << error << std::endl;
};

// Connect
tciRig->connect();

// Create audio device
auto wsClient = tciRig->getWebSocketClient();
auto tciAudio = std::make_unique<TciAudioDevice>(wsClient, 0);

// Set up audio callbacks (same as other audio devices)
tciAudio->setOnAudioData([](IAudioDevice& dev, void* data, size_t size, void* state) {
    // Process received audio
    short* samples = static_cast<short*>(data);
    size_t numSamples = size / sizeof(short);
    
    // Feed to FreeDV pipeline
    // ...
}, nullptr);

// Start audio
tciAudio->start();

// Now TCI is fully operational!
// Use tciRig->ptt(true/false) for PTT
// Use tciRig->setFrequency() for frequency changes
// Audio flows through tciAudio callbacks
```

## Command Line Testing

For quick testing without GUI:

```bash
# Build FreeDV with TCI
cd freedv-gui/build
cmake ..
make -j$(nproc)

# Test TCI connection (requires ExpertSDR3 running)
# Currently need to add test code to main.cpp

# Check TCI server is listening
nc -zv 127.0.0.1 40001

# Manual TCI command test
echo "vfo:0,0,14200000;" | nc 127.0.0.1 50001
```

## Debugging

Enable debug output:

```cpp
// In TciRigController.cpp, add:
std::cout << "TCI RX: " << cmdName << std::endl;

// In TciWebSocketClient.cpp, add:
std::cout << "TCI Command: " << command << std::endl;
```

Or use environment variable:
```bash
export FREEDV_TCI_DEBUG=1
./freedv
```

## Common Issues

**Connection refused**: ExpertSDR3 TCI server not running

**No audio**: Check AUDIO_START command sent, verify 48 kHz sample rate

**PTT not working**: Verify TRX command uses "tci" audio source parameter

**Mode wrong**: FreeDV auto-maps to DIGL/DIGU for digital modes

## Next Steps

1. **Add to GUI**: Follow Step 1-4 above
2. **Save Configuration**: Persist hostname/port settings
3. **Add Status Indicator**: Show TCI connection status in UI
4. **Add Reconnection**: Auto-reconnect on connection loss
5. **Add Device List**: Enumerate TCI devices on network

## Resources

- [TCI_INTEGRATION.md](TCI_INTEGRATION.md) - Full documentation
- [IMPLEMENTATION_SUMMARY.md](IMPLEMENTATION_SUMMARY.md) - Implementation details
- [TCI Protocol.txt](../TCI%20Protocol.txt) - Official TCI specification

## Support

For questions or issues:
- Check documentation in `TCI_INTEGRATION.md`
- Review code comments in source files
- Test with ExpertSDR3 first
- Check TCI server logs
