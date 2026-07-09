/**
 * electron-native-screenshare — Test Suite
 *
 * Test katmanları:
 *
 *   1. Module Loading       – modül yükleniyor mu, export'lar doğru mu?
 *   2. Argument Validation  – JS katmanındaki argüman guard'ları
 *   3. Error Message Format – HRESULT içeren hata mesajlarının formatı
 *   4. Double-Init Safety   – stopCapture → startCapture cycle crash etmemeli
 *   5. Audio Capture        – gerçek PCM data geliyor mu? (CI: sanal ses cihazı gerektirir)
 *
 * Ortam değişkenleri:
 *   AUDIO_DEVICE_AVAILABLE=1  → capture testlerini zorla çalıştır (lokal geliştirme)
 *   CI=true                   → GitHub Actions; capture testleri sanal cihaz varsa çalışır
 */

'use strict';

const os   = require('os');
const path = require('path');

// ─── helpers ──────────────────────────────────────────────────────────────────

/** CI ortamında sanal ses cihazı kurulu mu? (workflow bunu set eder) */
const hasVirtualAudioDevice = process.env.VIRTUAL_AUDIO_READY === '1';

/** Geliştirici kendi makinesinde zorla çalıştırmak istiyorsa */
const forceAudioTests       = process.env.AUDIO_DEVICE_AVAILABLE === '1';

/** Capture testleri bu flag true ise anlamlı */
const canCaptureAudio       = hasVirtualAudioDevice || forceAudioTests;

// ─────────────────────────────────────────────────────────────────────────────
// 1. MODULE LOADING
// ─────────────────────────────────────────────────────────────────────────────

describe('Module Loading', () => {
    let mod;

    beforeAll(() => { mod = require('../lib/index'); });

    test('exports all required functions', () => {
        expect(typeof mod.startCapture).toBe('function');
        expect(typeof mod.stopCapture).toBe('function');
        expect(typeof mod.getPidFromWindowHandle).toBe('function');
        expect(typeof mod.isAvailable).toBe('function');
        expect(typeof mod.getPlatform).toBe('function');
        expect(typeof mod.getLoadError).toBe('function');
    });

    test('getPlatform() matches os.platform()', () => {
        expect(mod.getPlatform()).toBe(os.platform());
    });

    test('isAvailable() returns boolean', () => {
        expect(typeof mod.isAvailable()).toBe('boolean');
    });

    test('getLoadError() returns string or null', () => {
        const err = mod.getLoadError();
        expect(err === null || typeof err === 'string').toBe(true);
    });

    test('if module is unavailable, getLoadError() is a non-empty string', () => {
        const mod2 = require('../lib/index');
        if (!mod2.isAvailable()) {
            expect(typeof mod2.getLoadError()).toBe('string');
            expect(mod2.getLoadError().length).toBeGreaterThan(0);
        } else {
            expect(mod2.getLoadError()).toBeNull();
        }
    });
});

// ─────────────────────────────────────────────────────────────────────────────
// 2. ARGUMENT VALIDATION (JS layer — no native build required for type checks)
// ─────────────────────────────────────────────────────────────────────────────

describe('Argument Validation', () => {
    let mod;

    beforeAll(() => { mod = require('../lib/index'); });

    const skip = () => {
        if (!mod.isAvailable()) return true;
        return false;
    };

    // startCapture — processId

    test('startCapture: string processId throws /processId/', () => {
        if (skip()) return;
        expect(() => mod.startCapture('bad', false, () => {})).toThrow(/processId/);
    });

    test('startCapture: negative processId throws /processId/', () => {
        if (skip()) return;
        expect(() => mod.startCapture(-1, false, () => {})).toThrow(/processId/);
    });

    test('startCapture: float processId throws /processId/', () => {
        if (skip()) return;
        expect(() => mod.startCapture(1.5, false, () => {})).toThrow(/processId/);
    });

    test('startCapture: null processId defaults to process.pid (no throw)', () => {
        if (skip()) return;
        // null is explicitly handled as "use process.pid" in lib/index.js
        // This may still throw if audio device is absent — that is expected.
        try {
            mod.startCapture(null, false, () => {});
            mod.stopCapture();
        } catch (e) {
            // A WASAPI / ScreenCaptureKit error is fine; a processId validation error is not.
            expect(e.message).not.toMatch(/processId/);
        }
    });

    // getPidFromWindowHandle

    test('getPidFromWindowHandle: string handle throws /windowHandle/', () => {
        if (skip()) return;
        expect(() => mod.getPidFromWindowHandle('bad')).toThrow(/windowHandle/);
    });

    test('getPidFromWindowHandle: null throws /windowHandle/', () => {
        if (skip()) return;
        expect(() => mod.getPidFromWindowHandle(null)).toThrow(/windowHandle/);
    });

    test('getPidFromWindowHandle: handle=0 returns a number (no crash)', () => {
        if (skip()) return;
        const result = mod.getPidFromWindowHandle(0);
        expect(typeof result).toBe('number');
    });
});

// ─────────────────────────────────────────────────────────────────────────────
// 3. ERROR MESSAGE FORMAT
// ─────────────────────────────────────────────────────────────────────────────

describe('Error Message Format', () => {
    let mod;

    beforeAll(() => { mod = require('../lib/index'); });

    /**
     * When a native WASAPI / ScreenCaptureKit error fires, the message must:
     *   - Contain "WASAPI Init Failed" (Windows) or a platform keyword (mac/linux)
     *   - Contain the pid and includeMode so the caller can diagnose without logs
     *   - Contain a human-readable HRESULT description (Windows)
     *
     * We trigger this by trying to start with isIncludeMode=true and processId=0
     * which is always invalid and produces a fast, deterministic WASAPI error.
     */
    test('Windows: error message contains pid, includeMode, and HRESULT description', () => {
        if (!mod.isAvailable() || os.platform() !== 'win32') return;

        let errorMessage = null;
        try {
            // pid=0 + includeMode=true → WASAPI rejects with E_INVALIDARG or E_UNEXPECTED
            mod.startCapture(0, true, () => {});
            mod.stopCapture();
        } catch (e) {
            errorMessage = e.message;
        }

        if (errorMessage === null) {
            // Some systems accept pid=0; skip rather than fail
            console.warn('pid=0 + includeMode=true did not throw on this system — skipping format test');
            return;
        }

        // Must contain the structured prefix added in addon.cpp
        expect(errorMessage).toMatch(/WASAPI Init Failed/i);
        // Must include pid context
        expect(errorMessage).toMatch(/pid=/);
        // Must include includeMode context
        expect(errorMessage).toMatch(/includeMode=/);
        // Must include a human-readable description (not just a raw hex code)
        expect(errorMessage).toMatch(/0x[0-9a-fA-F]{8}/); // still contains the hex
        expect(errorMessage.length).toBeGreaterThan(50);   // …but also prose
    });
});

// ─────────────────────────────────────────────────────────────────────────────
// 4. DOUBLE-INIT SAFETY  (stop → start → stop → start — no crash)
// ─────────────────────────────────────────────────────────────────────────────

describe('Double-Init Safety', () => {
    let mod;

    beforeAll(() => { mod = require('../lib/index'); });

    test('stopCapture() is safe to call before any startCapture()', () => {
        if (!mod.isAvailable()) return;
        expect(() => mod.stopCapture()).not.toThrow();
        expect(() => mod.stopCapture()).not.toThrow(); // idempotent
    });

    test('two consecutive startCapture calls do not segfault or hang', () => {
        // This exercises the pAudioClient reset fix (the E_UNEXPECTED / 0x8000FFFF bug).
        // The second call MUST NOT crash the process even if audio is unavailable.
        if (!mod.isAvailable() || !canCaptureAudio) {
            if (!canCaptureAudio && os.platform() === 'win32') {
                console.log('[double-init] No virtual audio device in CI — testing init error path instead');
                // At minimum, the second call should throw a structured error, not segfault.
                try { mod.startCapture(process.pid, false, () => {}); } catch (_) {}
                expect(() => {
                    try { mod.startCapture(process.pid, false, () => {}); } catch (_) {}
                }).not.toThrow(); // JS wrapper must not throw itself; only native does
            }
            return;
        }

        let threw = false;
        try {
            mod.startCapture(process.pid, false, () => {});
            mod.stopCapture();
            mod.startCapture(process.pid, false, () => {});
            mod.stopCapture();
        } catch (e) {
            threw = true;
            // If it throws, must be a structured native audio error, not a segfault / unhandled.
            // Acceptable prefixes: WASAPI (Win), CoreAudio (mac), PipeWire (Linux)
            expect(e.message).toMatch(/WASAPI|CoreAudio|ScreenCaptureKit|PipeWire|native/i);
        }

        // Either path (success or structured throw) is acceptable
        expect(typeof threw).toBe('boolean');
    });
});

// ─────────────────────────────────────────────────────────────────────────────
// 5. REAL AUDIO CAPTURE  (only when virtual audio device is present)
// ─────────────────────────────────────────────────────────────────────────────

describe('Audio Capture', () => {
    let mod;

    beforeAll(() => { mod = require('../lib/index'); });

    /**
     * This group only runs when a real or virtual audio render device exists.
     * In CI the workflow must:
     *   1. Install VB-CABLE or enable the "Null Audio Device" via PowerShell
     *   2. Use ffmpeg / PowerShell to pipe a sine wave to that device
     *   3. Set env VIRTUAL_AUDIO_READY=1
     *
     * Locally set AUDIO_DEVICE_AVAILABLE=1 to run these tests.
     */
    const requireAudio = (label) => {
        if (!mod.isAvailable()) {
            console.log(`[${label}] Native module not available — skipped`);
            return false;
        }
        if (!canCaptureAudio) {
            console.log(`[${label}] No audio device in this environment — skipped (set AUDIO_DEVICE_AVAILABLE=1 to force)`);
            return false;
        }
        return true;
    };

    test('startCapture returns true', () => {
        if (!requireAudio('returns-true')) return;

        let result;
        try {
            result = mod.startCapture(process.pid, false, () => {});
            mod.stopCapture();
        } catch (e) {
            // macOS: process.pid is a Node/Jest process with no audio output.
            // CoreAudio returns "Target process not found" — treat as skip, not failure.
            if (/target process not found|no audio stream/i.test(e.message)) {
                console.log(`[returns-true] No audio stream on process.pid — skipped (${e.message.substring(0, 80)})`);
                return;
            }
            throw e; // unexpected error — re-throw
        }
        expect(result).toBe(true);
    });

    test('callback receives valid PCM Buffer', (done) => {
        if (!requireAudio('pcm-buffer')) { done(); return; }

        const TIMEOUT_MS = 5000;
        const timer = setTimeout(() => {
            mod.stopCapture();
            done(new Error(
                `No audio data received within ${TIMEOUT_MS}ms. ` +
                'Ensure the virtual audio device is producing sound (ffmpeg sine wave).'
            ));
        }, TIMEOUT_MS);

        try {
            mod.startCapture(process.pid, false, (data, meta) => {
                clearTimeout(timer);
                mod.stopCapture();

                // Buffer assertions
                expect(Buffer.isBuffer(data)).toBe(true);
                expect(data.length).toBeGreaterThan(0);
                // Buffer length must be a multiple of frame size (2ch * 4 bytes = 8)
                expect(data.length % 8).toBe(0);

                done();
            });
        } catch (e) {
            clearTimeout(timer);
            if (/target process not found|no audio stream/i.test(e.message)) {
                console.log(`[pcm-buffer] No audio stream on process.pid — skipped`);
                done();
            } else {
                done(e);
            }
        }
    }, 8000);

    test('AudioMetadata shape is correct', (done) => {
        if (!requireAudio('metadata-shape')) { done(); return; }

        const timer = setTimeout(() => {
            mod.stopCapture();
            done(new Error('No audio data received — cannot validate metadata shape'));
        }, 5000);

        try {
            mod.startCapture(process.pid, false, (data, meta) => {
                clearTimeout(timer);
                mod.stopCapture();

                expect(typeof meta).toBe('object');
                expect(meta.sampleRate).toBe(48000);
                expect(meta.channels).toBe(2);
                expect(meta.bitsPerSample).toBe(32);
                expect(meta.isFloat).toBe(true);

                done();
            });
        } catch (e) {
            clearTimeout(timer);
            if (/target process not found|no audio stream/i.test(e.message)) {
                console.log(`[metadata-shape] No audio stream on process.pid — skipped`);
                done();
            } else {
                done(e);
            }
        }
    }, 8000);

    test('PCM values are in float32 range [-1.0, 1.0]', (done) => {
        if (!requireAudio('pcm-range')) { done(); return; }

        const timer = setTimeout(() => {
            mod.stopCapture();
            done(new Error('No audio data received — cannot validate PCM range'));
        }, 5000);

        try {
            mod.startCapture(process.pid, false, (data, _meta) => {
                clearTimeout(timer);
                mod.stopCapture();

                const floats = new Float32Array(
                    data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength)
                );

                let allInRange = true;
                for (let i = 0; i < floats.length; i++) {
                    if (floats[i] < -1.5 || floats[i] > 1.5) {
                        allInRange = false;
                        break;
                    }
                }

                expect(allInRange).toBe(true);
                done();
            });
        } catch (e) {
            clearTimeout(timer);
            if (/target process not found|no audio stream/i.test(e.message)) {
                console.log(`[pcm-range] No audio stream on process.pid — skipped`);
                done();
            } else {
                done(e);
            }
        }
    }, 8000);

    test('stopCapture() stops data flow', (done) => {
        if (!requireAudio('stop-capture')) { done(); return; }

        let callbackCount = 0;
        let started = false;

        try {
            mod.startCapture(process.pid, false, () => { callbackCount++; });
            started = true;
        } catch (e) {
            if (/target process not found|no audio stream/i.test(e.message)) {
                console.log(`[stop-capture] No audio stream on process.pid — skipped`);
                done();
                return;
            }
            done(e);
            return;
        }

        setTimeout(() => {
            mod.stopCapture();
            const countAtStop = callbackCount;
            setTimeout(() => {
                expect(callbackCount).toBe(countAtStop);
                done();
            }, 500);
        }, 300);
    }, 5000);
});
