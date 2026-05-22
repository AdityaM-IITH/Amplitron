/**
 * Amplitron Serverless Preset Sharing Module
 *
 * Implements LZ-String compression to serialize and URL-encode the global signal-chain
 * preset state JSON. Hydrates the application state on load if share parameters are detected.
 */

// Global sharing function to be called from UI (or manually via console)
window.sharePresetToUrl = function () {
    if (typeof Module === 'undefined' || !Module.ccall) {
        console.error('[PresetShare] WASM module not ready.');
        return;
    }

    try {
        // Retrieve JSON state from C++ AudioEngine
        const jsonStr = UTF8ToString(Module.ccall('export_preset_json', 'number', [], []));
        if (!jsonStr) {
            console.error('[PresetShare] Failed to retrieve preset JSON from engine.');
            return;
        }

        // Compress using lz-string
        const compressed = LZString.compressToEncodedURIComponent(jsonStr);

        // Update URL
        const url = new URL(window.location);
        url.searchParams.set('preset', compressed);
        history.pushState(null, '', url);

        // Copy to clipboard
        navigator.clipboard.writeText(url.toString()).then(() => {
            console.log('[PresetShare] URL copied to clipboard.');
        }).catch(err => {
            console.error('[PresetShare] Clipboard write failed:', err);
        });
    } catch (err) {
        console.error('[PresetShare] Error during preset sharing:', err);
    }
};

/**
 * Hydration Hook
 * Detects incoming share parameters on load, decompresses the payload safely,
 * and re-hydrates the application state.
 */
function hydrateSharedPreset() {
    const params = new URLSearchParams(window.location.search);
    const presetPayload = params.get('preset');

    if (presetPayload) {
        try {
            console.log('[PresetShare] Shared preset detected. Decompressing...');
            const decompressedJson = LZString.decompressFromEncodedURIComponent(presetPayload);
            
            if (decompressedJson) {
                // Pass JSON string to C++ for deserialization
                Module.ccall(
                    'import_preset_json',
                    'void',
                    ['string'],
                    [decompressedJson]
                );
                console.log('[PresetShare] Preset successfully re-hydrated.');
                
                // Clear URL to prevent re-hydration on refresh
                const url = new URL(window.location);
                url.searchParams.delete('preset');
                history.replaceState(null, '', url);
            } else {
                console.warn('[PresetShare] Decompression resulted in empty payload. Invalid URL?');
            }
        } catch (err) {
            console.error('[PresetShare] Failed to hydrate preset:', err);
        }
    }
}

// Hook into Module.onRuntimeInitialized so that C++ is ready before we hydrate
if (typeof Module !== 'undefined') {
    const originalOnRuntimeInit = Module.onRuntimeInitialized;
    Module.onRuntimeInitialized = function () {
        if (originalOnRuntimeInit) {
            originalOnRuntimeInit();
        }
        hydrateSharedPreset();
    };
}
