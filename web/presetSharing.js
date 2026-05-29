/**
 * Amplitron Serverless Preset Sharing Module
 *
 * Implements LZ-String compression to serialize and URL-encode the global signal-chain
 * preset state JSON. Hydrates the application state on load if share parameters are detected.
 */

// Global sharing function to be called from UI (or manually via console)
window.sharePresetToUrl = function (jsonStr) {
    try {
        if (!jsonStr) {
            console.error('[PresetShare] Empty preset JSON provided.');
            return;
        }

        // Compress using lz-string
        const compressed = LZString.compressToEncodedURIComponent(jsonStr);

        // Update URL hash
        const url = new URL(window.location);
        const hashParams = new URLSearchParams(url.hash.replace('#', ''));
        hashParams.set('preset', compressed);
        url.hash = hashParams.toString();
        // Generate the shareable string
        let shareableUrl = url.toString();
        if (shareableUrl.includes('localhost') || shareableUrl.includes('127.0.0.1')) {
            const prodBase = 'https://sudip-mondal-2002.github.io/Amplitron/demo/';
            shareableUrl = prodBase + url.hash;
        }

        // Copy to clipboard FIRST to avoid history.pushState consuming the user activation token
        if (navigator.clipboard && window.isSecureContext) {
            navigator.clipboard.writeText(shareableUrl).then(() => {
                console.log('[PresetShare] URL copied to clipboard.');
            }).catch(err => {
                console.error('[PresetShare] Clipboard write failed:', err);
                fallbackCopyTextToClipboard(shareableUrl);
            });
        } else {
            fallbackCopyTextToClipboard(shareableUrl);
        }
        
        // Update URL hash LAST
        history.pushState(null, '', url);
    } catch (err) {
        console.error('[PresetShare] Error during preset sharing:', err);
    }
};

function fallbackCopyTextToClipboard(text) {
    var textArea = document.createElement("textarea");
    textArea.value = text;
    // Avoid scrolling to bottom
    textArea.style.top = "0";
    textArea.style.left = "0";
    textArea.style.position = "fixed";
    document.body.appendChild(textArea);
    textArea.focus();
    textArea.select();

    try {
        var successful = document.execCommand('copy');
        if (!successful) {
            window.prompt("Press Ctrl+C or Cmd+C to copy the link:", text);
        }
        var msg = successful ? 'successful' : 'unsuccessful';
        console.log('[PresetShare] Fallback copy ' + msg);
    } catch (err) {
        console.error('[PresetShare] Fallback copy failed', err);
        window.prompt("Press Ctrl+C or Cmd+C to copy the link:", text);
    }
    document.body.removeChild(textArea);
}

/**
 * Hydration Hook
 * Detects incoming share parameters on load, decompresses the payload safely,
 * and re-hydrates the application state.
 */
const MAX_COMPRESSED_PRESET_LEN = 12000;
const MAX_DECOMPRESSED_PRESET_LEN = 500000;

window.hydrateSharedPreset = function() {
    const params = new URLSearchParams(window.location.hash.replace('#', ''));
    const presetPayload = params.get('preset');

    if (presetPayload) {
        if (presetPayload.length > MAX_COMPRESSED_PRESET_LEN) return;
        try {
            console.log('[PresetShare] Shared preset detected. Decompressing...');
            const decompressedJson = LZString.decompressFromEncodedURIComponent(presetPayload);
            
            if (decompressedJson && decompressedJson.length <= MAX_DECOMPRESSED_PRESET_LEN) {
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
                const hashParams = new URLSearchParams(url.hash.replace('#', ''));
                hashParams.delete('preset');
                url.hash = hashParams.toString();
                history.replaceState(null, '', url);
            } else {
                console.warn('[PresetShare] Decompression resulted in empty payload. Invalid URL?');
            }
        } catch (err) {
            console.error('[PresetShare] Failed to hydrate preset:', err);
        }
    }
};
