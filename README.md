# electron-native-screenshare

Cross-platform native audio capture for Electron screen sharing with **process-level audio isolation**.

Uses low-level OS APIs instead of web APIs for **lower latency, lower CPU usage, and true per-process audio control**.

| Platform | API | Min Version | Include Mode | Exclude Mode |
|----------|-----|-------------|:------------:|:------------:|
| Windows  | WASAPI Process Loopback | Windows 10 2004 | ✅ Per-process | ✅ Per-process |
| macOS    | ScreenCaptureKit | macOS 13 Ventura | ✅ Per-app | ✅ Per-app |
| Linux    | PipeWire | 0.3.26+ | ✅ Per-process | ⚠️ System audio* |

> \* Linux exclude mode captures all system audio from the default output. Per-process exclusion is an OS-level limitation of PipeWire.

## Installation

```bash
npm install electron-native-screenshare
```

### Platform Prerequisites

**Windows**: Visual Studio Build Tools with "Desktop development with C++" workload.

**macOS**: Xcode Command Line Tools. macOS 13 (Ventura) or later required.

**Linux**:
```bash
sudo apt install libpipewire-0.3-dev libx11-dev
```

> ⚠️ If PipeWire is not installed on Linux, the module will load but audio functions will throw a descriptive error at runtime. It will **not** crash your application.

## Quick Start

```javascript
const {
  startCapture,
  stopCapture,
  getPidFromWindowHandle,
  isAvailable
} = require('electron-native-screenshare');

// Check if native module loaded successfully
if (!isAvailable()) {
  console.warn('Native audio capture is not available on this platform');
}
```

## API

### `startCapture(processId?, isIncludeMode?, onData?)`

Starts audio capture with process-level isolation.

**Parameters:**
| Name | Type | Default | Description |
|------|------|---------|-------------|
| `processId` | `number` | `process.pid` | Target process ID |
| `isIncludeMode` | `boolean` | `false` | `true` = capture only target, `false` = exclude target |
| `onData` | `function` | `() => {}` | Callback `(data: Buffer, meta: AudioMetadata) => void` |

**Returns:** `boolean` — `true` if started successfully.

**Throws:** `Error` if native module unavailable or initialization fails.

#### AudioMetadata

```typescript
interface AudioMetadata {
  sampleRate: number;    // e.g., 48000
  channels: number;      // e.g., 2 (stereo)
  bitsPerSample: number; // e.g., 32
  isFloat: boolean;      // true = IEEE float, false = integer PCM
}
```

### `stopCapture()`

Stops the active capture session. Safe to call even if nothing is capturing.

**Returns:** `boolean`

### `getPidFromWindowHandle(windowHandle)`

Resolves a native window handle to its owning process ID.

| Platform | Handle Type | Notes |
|----------|-------------|-------|
| Windows  | `HWND` | Auto-resolves UWP ApplicationFrameWindow → child process |
| macOS    | `CGWindowID` | Uses CGWindowListCopyWindowInfo |
| Linux    | X11 Window ID | Uses `_NET_WM_PID` atom |

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `windowHandle` | `number` | Native window handle from Electron's `desktopCapturer` |

**Returns:** `number` — Process ID, or `0` if not found.

### `isAvailable()`

Returns `true` if the native module loaded successfully.

### `getPlatform()`

Returns the current platform: `'win32'`, `'darwin'`, or `'linux'`.

### `getLoadError()`

Returns the load error message if the native module failed, or `null` on success.

## Usage Examples

### Screen Sharing (Exclude Mode)

Capture all system audio **except** your Electron app:

```javascript
const { startCapture, stopCapture } = require('electron-native-screenshare');

// Your app's audio will NOT go into the stream
startCapture(process.pid, false, (audioData, meta) => {
  // audioData: Buffer of raw PCM float32 samples
  // meta: { sampleRate: 48000, channels: 2, bitsPerSample: 32, isFloat: true }

  // Send to WebRTC, write to file, or process as needed
  webrtcTrack.write(audioData);
});

// Later...
stopCapture();
```

### Window Sharing (Include Mode)

Capture **only** a specific window's audio:

```javascript
const {
  startCapture,
  stopCapture,
  getPidFromWindowHandle
} = require('electron-native-screenshare');

// Get sources from Electron's desktopCapturer
const sources = await desktopCapturer.getSources({ types: ['window'] });
const target = sources.find(s => s.name === 'Spotify');

// Extract window handle and resolve PID
const hwnd = parseInt(target.id.split(':')[1]);
const pid = getPidFromWindowHandle(hwnd);

// Capture only Spotify's audio
startCapture(pid, true, (audioData, meta) => {
  // Only Spotify's audio is in the buffer
  webrtcTrack.write(audioData);
});
```

### Graceful Degradation

```javascript
const capture = require('electron-native-screenshare');

if (!capture.isAvailable()) {
  const error = capture.getLoadError();
  console.warn(`Audio capture unavailable: ${error}`);
  // Fall back to Electron's built-in audio capture
  // or disable audio in your sharing feature
} else {
  capture.startCapture(process.pid, false, onAudioData);
}
```

## How It Works

### Windows — WASAPI Process Loopback
Uses the Windows Audio Session API (WASAPI) with `AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK` (Windows 10 2004+). This provides true kernel-level process audio isolation with zero mixing overhead.

### macOS — ScreenCaptureKit
Uses Apple's `SCStream` with `SCContentFilter` (macOS 13+). Filters by `SCRunningApplication` to include/exclude specific apps. Audio is delivered via `SCStreamOutput` delegate as `CMSampleBuffer`.

### Linux — PipeWire
Uses PipeWire's `pw_stream` API for audio capture. Include mode connects directly to the target process's audio node via `PW_KEY_TARGET_OBJECT`. Exclude mode captures from the default sink monitor.

## CI/CD

| Workflow | Trigger | Description |
|----------|---------|-------------|
| `test.yml` | Push to `main`, PRs | Builds and tests on Windows, macOS, Linux × Node 18/20/22 |
| `publish-npm.yml` | GitHub Release | Publishes to npm (requires passing tests) |
| `publish-github.yml` | GitHub Release | Publishes to GitHub Packages as `@CilginSinek/electron-native-screenshare` |

> Tests **must pass** before any publish. If the test pipeline fails, the package will not be published.

## License

[MIT](LICENSE)
