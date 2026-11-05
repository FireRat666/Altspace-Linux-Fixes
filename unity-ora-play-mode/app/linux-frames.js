"use strict";
// Linux stand-in for ora-windows.node (the D3D11 shared-texture addon).
//
// Each view's frames go to <frame dir>/<regionId>_<W>x<H> as raw BGRA. The patched
// SideQuest.Ora.dll (OraLinuxShm.Open) maps that file in Unity. The folder is on tmpfs,
// so writes land in shared memory that Unity's mapping sees directly.
//
// API matches what windowmanager.js uses: Initialize, Cleanup, ExtractPixels, UnmapRegion.
// On Linux, windowmanager passes the paint event's NativeImage as sharedTextureHandle.
Object.defineProperty(exports, "__esModule", { value: true });
const fs = require("fs");
const path = require("path");

// Set by the launcher. Inside Flatpak it is the host path of the sandbox's /tmp/ora-frames,
// which is where the patched SideQuest.Ora.dll looks.
const SHM_DIR = process.env.ORA_FRAME_DIR || "/tmp/ora-frames";
fs.mkdirSync(SHM_DIR, { recursive: true, mode: 0o700 });
const regions = new Map(); // regionId -> { name, fd, width, height }

function closeRegion(r) {
    try { fs.closeSync(r.fd); } catch (_) { /* ignore */ }
    // Unity may still have the old file mapped; unlinking keeps the mapping valid until it lets go.
    try { fs.unlinkSync(path.join(SHM_DIR, r.name)); } catch (_) { /* ignore */ }
}

function pidAlive(pid) {
    try { process.kill(pid, 0); return true; } catch (e) { return e.code === "EPERM"; }
}

// Remove frame files left behind by Electron processes that crashed or were killed.
function removeStaleFiles() {
    let names = [];
    try { names = fs.readdirSync(SHM_DIR); } catch (_) { return; }
    for (const name of names) {
        // "BanterPixelBuffer" is the name Ora itself gives its frame buffers.
        const m = /^BanterPixelBuffer\d+p(\d+)_\d+x\d+$/.exec(name);
        if (m && !pidAlive(Number(m[1]))) {
            try { fs.unlinkSync(path.join(SHM_DIR, name)); } catch (_) { /* ignore */ }
        }
    }
}

exports.Initialize = function () { removeStaleFiles(); };

exports.Cleanup = function () {
    for (const r of regions.values()) closeRegion(r);
    regions.clear();
};

exports.UnmapRegion = function (regionId) {
    const r = regions.get(regionId);
    if (!r) return;
    closeRegion(r);
    regions.delete(regionId);
};

/** Writes one frame. Returns the region name ("<regionId>_<W>x<H>") Unity opens, or undefined to skip. */
exports.ExtractPixels = function (opts) {
    const image = opts.sharedTextureHandle;
    if (!image || typeof image.toBitmap !== "function") return undefined;
    const { width, height } = image.getSize();
    if (!width || !height) return undefined;
    const bytes = width * height * 4;
    const pixels = image.toBitmap(); // BGRA on Linux, same layout Unity's B8G8R8A8 texture expects
    if (pixels.length < bytes) return undefined;

    let r = regions.get(opts.regionId);
    if (!r || r.width !== width || r.height !== height) {
        if (r) closeRegion(r);
        const name = `${opts.regionId}_${width}x${height}`;
        const fd = fs.openSync(path.join(SHM_DIR, name), "w+", 0o600);
        fs.ftruncateSync(fd, bytes);
        r = { name, fd, width, height, staging: Buffer.allocUnsafe(bytes) };
        regions.set(opts.regionId, r);
    }
    // Chromium's bitmap is top row first. Unity on Linux (OpenGL/Vulkan) treats the first row as the
    // bottom, so write the rows in reverse order (Ora's Windows path relies on Direct3D's top-first rows).
    const stride = width * 4;
    for (let y = 0; y < height; y++) {
        pixels.copy(r.staging, (height - 1 - y) * stride, y * stride, (y + 1) * stride);
    }
    fs.writeSync(r.fd, r.staging, 0, bytes, 0);
    return r.name;
};
