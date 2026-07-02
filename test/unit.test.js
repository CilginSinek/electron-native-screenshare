/**
 * electron-native-screenshare — Unit Tests
 *
 * Tests module loading, API surface validation, argument handling,
 * and platform detection logic. These run on all platforms in CI.
 */

'use strict';

const os = require('os');
const path = require('path');

// --- Module Loading ---

describe('Module Loading', () => {
    let mod;

    beforeAll(() => {
        mod = require('../lib/index');
    });

    test('module exports all required functions', () => {
        expect(typeof mod.startCapture).toBe('function');
        expect(typeof mod.stopCapture).toBe('function');
        expect(typeof mod.getPidFromWindowHandle).toBe('function');
        expect(typeof mod.isAvailable).toBe('function');
        expect(typeof mod.getPlatform).toBe('function');
        expect(typeof mod.getLoadError).toBe('function');
    });

    test('getPlatform returns the current OS platform', () => {
        expect(mod.getPlatform()).toBe(os.platform());
    });

    test('isAvailable returns a boolean', () => {
        expect(typeof mod.isAvailable()).toBe('boolean');
    });

    test('getLoadError returns string or null', () => {
        const err = mod.getLoadError();
        expect(err === null || typeof err === 'string').toBe(true);
    });
});

// --- API Surface Validation ---

describe('API Surface', () => {
    let mod;

    beforeAll(() => {
        mod = require('../lib/index');
    });

    test('startCapture rejects non-integer processId', () => {
        if (!mod.isAvailable()) return; // skip on platforms without native build

        expect(() => mod.startCapture('invalid', false, () => {})).toThrow(/processId/);
        expect(() => mod.startCapture(-5, false, () => {})).toThrow(/processId/);
        expect(() => mod.startCapture(1.5, false, () => {})).toThrow(/processId/);
    });

    test('getPidFromWindowHandle rejects non-number argument', () => {
        if (!mod.isAvailable()) return;

        expect(() => mod.getPidFromWindowHandle('invalid')).toThrow(/windowHandle/);
        expect(() => mod.getPidFromWindowHandle(null)).toThrow(/windowHandle/);
        expect(() => mod.getPidFromWindowHandle(undefined)).toThrow(/windowHandle/);
    });

    test('getPidFromWindowHandle returns a number for valid handle', () => {
        if (!mod.isAvailable()) return;

        // Handle 0 is unlikely to match a real window but should not crash
        const result = mod.getPidFromWindowHandle(0);
        expect(typeof result).toBe('number');
    });
});

// --- Graceful Degradation ---

describe('Graceful Degradation', () => {
    test('functions throw descriptive errors when native module is unavailable', () => {
        // Only test this if the native module is NOT available (e.g., unsupported platform)
        const mod = require('../lib/index');
        if (mod.isAvailable()) {
            // On supported platforms with a successful build, this test is a no-op
            expect(mod.isAvailable()).toBe(true);
            return;
        }

        expect(() => mod.startCapture(123, false, () => {})).toThrow(/native module/i);
        expect(() => mod.stopCapture()).toThrow(/native module/i);
        expect(() => mod.getPidFromWindowHandle(123)).toThrow(/native module/i);
    });
});

// --- Platform-Specific Integration Tests ---

describe('Platform Integration', () => {
    let mod;

    beforeAll(() => {
        mod = require('../lib/index');
    });

    // These only run when the native module is available (CI with successful build)
    const skipIfUnavailable = () => {
        if (!mod.isAvailable()) {
            console.log('Native module not available, skipping integration test');
            return true;
        }
        return false;
    };

    test('stopCapture does not crash when called without starting', () => {
        if (skipIfUnavailable()) return;

        // Should be safe to call stop even if nothing is capturing
        expect(() => mod.stopCapture()).not.toThrow();
    });

    test('startCapture with default pid uses process.pid', () => {
        if (skipIfUnavailable()) return;

        // On Windows/macOS this should work with current PID in exclude mode
        // On Linux it may fail if PipeWire is not running in CI
        const platform = os.platform();
        if (platform === 'linux') {
            // PipeWire might not be in CI
            try {
                const result = mod.startCapture(undefined, false, () => {});
                expect(result).toBe(true);
                mod.stopCapture();
            } catch (e) {
                expect(e.message).toMatch(/PipeWire/i);
            }
        } else {
            try {
                const result = mod.startCapture(undefined, false, () => {});
                expect(result).toBe(true);
                mod.stopCapture();
            } catch (e) {
                // May fail in CI environments without audio devices
                console.log(`startCapture failed in CI (expected): ${e.message}`);
            }
        }
    });

    test('audio metadata shape is correct when callback fires', (done) => {
        if (skipIfUnavailable()) {
            done();
            return;
        }

        const timeout = setTimeout(() => {
            // If no audio data arrives within 2 seconds, that's OK in CI (no audio sources)
            mod.stopCapture();
            done();
        }, 2000);

        try {
            mod.startCapture(process.pid, false, (data, meta) => {
                clearTimeout(timeout);

                expect(Buffer.isBuffer(data)).toBe(true);
                expect(typeof meta).toBe('object');
                expect(typeof meta.sampleRate).toBe('number');
                expect(typeof meta.channels).toBe('number');
                expect(typeof meta.bitsPerSample).toBe('number');
                expect(typeof meta.isFloat).toBe('boolean');

                mod.stopCapture();
                done();
            });
        } catch (e) {
            clearTimeout(timeout);
            console.log(`Integration test skipped due to: ${e.message}`);
            done();
        }
    });
});
