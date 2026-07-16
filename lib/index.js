/**
 * electron-native-screenshare
 *
 * Cross-platform native audio capture for Electron screen sharing
 * with process-level audio isolation.
 *
 * Unified API — platform detection is automatic. The consumer never
 * needs to import OS-specific modules.
 *
 * Supported platforms:
 *   - Windows 10 2004+ (WASAPI Process Loopback)
 *   - macOS 13 Ventura+ (ScreenCaptureKit)
 *   - Linux (PipeWire 0.3.26+)
 *
 * @module electron-native-screenshare
 */

'use strict';

const os = require('os');
const path = require('path');

const platform = os.platform();
const SUPPORTED_PLATFORMS = ['win32', 'darwin', 'linux'];

/** @type {import('./types').NativeCapture | null} */
let capture = null;

/** @type {string | null} */
let loadError = null;

// --- Native module loading with graceful degradation ---

if (!SUPPORTED_PLATFORMS.includes(platform)) {
    loadError = `[electron-native-screenshare] Unsupported platform: "${platform}". ` +
        `Supported: ${SUPPORTED_PLATFORMS.join(', ')}. ` +
        `Audio capture functions will throw when called.`;
    console.warn(loadError);
} else {
    try {
        capture = require('../build/Release/electron_native_screenshare.node');
    } catch (e) {
        const platformHints = {
            win32: 'Ensure Visual Studio Build Tools and Windows 10 SDK are installed.',
            darwin: 'Ensure Xcode Command Line Tools are installed. Requires macOS 13+.',
            linux: 'Ensure libpipewire-0.3-dev and libx11-dev are installed. ' +
                'Run: sudo apt install libpipewire-0.3-dev libx11-dev'
        };

        loadError = `[electron-native-screenshare] Failed to load native module on ${platform}.\n` +
            `  Error: ${e.message}\n` +
            `  Hint: ${platformHints[platform] || 'Check native build dependencies.'}`;
        console.warn(loadError);
    }
}

/**
 * Throws a descriptive error if the native module isn't loaded.
 * @private
 */
function ensureLoaded() {
    if (!capture) {
        throw new Error(
            loadError ||
            `[electron-native-screenshare] Native module is not available on ${platform}.`
        );
    }
}

/**
 * Starts audio capture with process-level isolation.
 *
 * Behavior depends on `isIncludeMode`:
 *   - `false` (default): Captures ALL system audio EXCEPT the specified process.
 *     Use for screen sharing — your app's audio won't leak into the stream.
 *   - `true`: Captures ONLY the specified process's audio.
 *     Use for window sharing — only that window's audio goes to the stream.
 *
 * @param {number|number[]} [processId] - Target process ID or an array of PIDs. Defaults to `process.pid` (current process).
 * @param {boolean} [isIncludeMode=false] - `true` to include only the target, `false` to exclude it.
 * @param {function(Buffer, AudioMetadata): void} [onData] - Callback receiving raw PCM audio chunks.
 *   - `data` {Buffer} — Raw PCM audio (float32, stereo, 48kHz by default)
 *   - `meta` {AudioMetadata} — `{ sampleRate, channels, bitsPerSample, isFloat }`
 * @returns {boolean} `true` if capture started successfully.
 * @throws {Error} If the native module failed to load or initialization fails.
 *
 * @example
 * const { startCapture, stopCapture } = require('electron-native-screenshare');
 *
 * // Screen share: exclude your app's audio
 * startCapture(process.pid, false, (data, meta) => {
 *   console.log(`Got ${data.length} bytes, ${meta.sampleRate}Hz ${meta.channels}ch`);
 * });
 *
 * // Window share: include only a specific window's audio
 * startCapture(targetPid, true, (data, meta) => { ... });
 */
function startCapture(processId, isIncludeMode, onData) {
    ensureLoaded();

    if (processId === undefined || processId === null) {
        processId = process.pid;
    }
    if (Array.isArray(processId)) {
        if (!processId.every(id => typeof id === 'number' && Number.isInteger(id) && id >= 0)) {
            throw new Error('[electron-native-screenshare] processId array must contain non-negative integers.');
        }
    } else if (typeof processId !== 'number' || !Number.isInteger(processId) || processId < 0) {
        throw new Error('[electron-native-screenshare] processId must be a non-negative integer or an array of integers.');
    }
    if (typeof isIncludeMode !== 'boolean') {
        isIncludeMode = false;
    }
    if (typeof onData !== 'function') {
        onData = () => { };
    }

    return capture.startCapture(processId, isIncludeMode, (data, meta) => onData(data, meta));
}

/**
 * Stops the active audio capture session.
 *
 * Safe to call even if no capture is running.
 *
 * @returns {boolean} `true` if stopped successfully.
 * @throws {Error} If the native module failed to load.
 */
function stopCapture() {
    ensureLoaded();
    return capture.stopCapture();
}

/**
 * Resolves a native window handle to its owning process ID.
 *
 * Platform-specific handle types:
 *   - Windows: HWND (from Electron's `desktopCapturer` source ID)
 *   - macOS: CGWindowID
 *   - Linux: X11 Window ID
 *
 * On Windows, this also handles UWP ApplicationFrameWindow → actual child process resolution.
 *
 * @param {number} windowHandle - The native window handle (HWND / CGWindowID / X11 Window).
 * @returns {number} Process ID owning the window, or `0` if not found.
 * @throws {Error} If the native module failed to load or argument is invalid.
 *
 * @example
 * const { getPidFromWindowHandle } = require('electron-native-screenshare');
 *
 * // From Electron desktopCapturer source:
 * // source.id = "window:12345:0" → extract 12345
 * const hwnd = parseInt(source.id.split(':')[1]);
 * const pid = getPidFromWindowHandle(hwnd);
 */
function getPidFromWindowHandle(windowHandle) {
    ensureLoaded();

    if (typeof windowHandle !== 'number') {
        throw new Error('[electron-native-screenshare] windowHandle must be a number.');
    }

    return capture.getPidFromHwnd(windowHandle);
}

/**
 * Returns whether the native module loaded successfully on this platform.
 *
 * Use this to gracefully check availability before calling capture functions,
 * instead of wrapping everything in try/catch.
 *
 * @returns {boolean} `true` if capture functions are available.
 */
function isAvailable() {
    return capture !== null;
}

/**
 * Returns the current platform name.
 * @returns {string} 'win32', 'darwin', 'linux', or the raw os.platform() string.
 */
function getPlatform() {
    return platform;
}

/**
 * If the native module failed to load, returns the error message.
 * Returns `null` if the module loaded successfully.
 * @returns {string | null}
 */
function getLoadError() {
    return loadError;
}

module.exports = {
    startCapture,
    stopCapture,
    getPidFromWindowHandle,
    isAvailable,
    getPlatform,
    getLoadError,
};
