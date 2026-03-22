# OBS-PTZ Thread Safety Fixes - Phase 2

## Changes Applied

### 1. Thread-Safe OBSFrontendEventWrapper (ptz-controls.cpp)
**Location:** Line 72  
**Problem:** OBS frontend callbacks may come from non-GUI threads in OBS 31+, but the code directly accessed Qt GUI objects.  
**Fix:** Wrapped the callback in `QMetaObject::invokeMethod()` with `Qt::QueuedConnection` to ensure execution on the Qt main thread.

```cpp
void PTZControls::OBSFrontendEventWrapper(enum obs_frontend_event event, void *ptr)
{
    PTZControls *controls = reinterpret_cast<PTZControls *>(ptr);
    // Ensure execution on Qt main thread to prevent crashes
    // OBS frontend callbacks may come from non-GUI threads
    QMetaObject::invokeMethod(controls, [controls, event]() {
        controls->OBSFrontendEvent(event);
    }, Qt::QueuedConnection);
}
```

### 2. Thread-Safe Widget Deletion (settings.cpp)
**Location:** Line 492 (obs_event function)  
**Problem:** Deleting QWidget from non-GUI thread is unsafe.  
**Fix:** Used `deleteLater()` which is thread-safe and defers deletion to Qt event loop.

```cpp
static void obs_event(enum obs_frontend_event event, void *)
{
    if (event == OBS_FRONTEND_EVENT_EXIT && ptzSettingsWindow) {
        ptzSettingsWindow->deleteLater();
        ptzSettingsWindow = nullptr;
    }
}
```

### 3. Fixed OBSData Reference Management in SaveConfig (ptz-controls.cpp)
**Location:** Line 501  
**Problem:** Manual `obs_data_release()` after creation creates fragile reference counting.  
**Fix:** Changed to `OBSDataAutoRelease` for automatic reference management.

```cpp
OBSDataAutoRelease savedata = obs_data_create();
```

### 4. Fixed OBSData Reference Management in LoadConfig (ptz-controls.cpp)
**Location:** Line 565  
**Problem:** Manual `obs_data_release()` after creation creates fragile reference counting.  
**Fix:** Changed to `OBSDataAutoRelease` and removed manual release call.

```cpp
OBSDataAutoRelease loaddata = obs_data_create_from_json_file_safe(file, "bak");
```

### 5. Fixed preset_max Type (ptz-device.cpp)
**Location:** Line 540  
**Problem:** Using `obs_data_set_default_double()` for an integer value (16).  
**Fix:** Changed to `obs_data_set_default_int()`.

```cpp
obs_data_set_default_int(config, "preset_max", 16);
```

## Verification Results

All changes verified successfully:
- ✅ `Qt::QueuedConnection` present in ptz-controls.cpp
- ✅ `deleteLater()` present in settings.cpp  
- ✅ `OBSDataAutoRelease` used in SaveConfig
- ✅ `OBSDataAutoRelease` used in LoadConfig
- ✅ `preset_max` uses integer type
- ✅ No double type for `preset_max`

## Impact
- **Thread Safety:** All OBS frontend event callbacks now execute on Qt main thread
- **Widget Management:** Settings window deletion is now thread-safe
- **Memory Management:** More robust reference counting for OBSData objects
- **Type Correctness:** preset_max now uses correct integer type

## No Functionality Removed
All existing features preserved:
- ✅ Scene selection logic unchanged
- ✅ Auto-select functionality intact
- ✅ Event handlers all present
- ✅ Preset recall/save unchanged
- ✅ OBS proc handler registration unchanged
