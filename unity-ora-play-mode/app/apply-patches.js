#!/usr/bin/env node
// Applies the Linux changes to an unpacked Ora Electron app (resources/app.asar from
// com.sidequest.ora's Plugins/Windows~). Usage: node apply-patches.js <unpacked app dir>
//
// Each change is an exact text replacement, so an Ora update that changes these lines
// fails loudly here instead of producing a half-patched app.
"use strict";
const fs = require("fs");
const path = require("path");

const appDir = process.argv[2];
if (!appDir) {
    console.error("usage: node apply-patches.js <unpacked app dir>");
    process.exit(2);
}
const dist = path.join(appDir, "dist");

function patch(file, from, to, expectedCount) {
    const p = path.join(dist, file);
    const text = fs.readFileSync(p, "utf8");
    if (text.includes(to)) {
        console.log(`  ${file}: already patched`);
        return;
    }
    const count = text.split(from).length - 1;
    if (count !== expectedCount) {
        throw new Error(`${file}: expected ${expectedCount} match(es) for:\n${from}\nfound ${count}. Ora version not supported.`);
    }
    fs.writeFileSync(p, text.split(from).join(to));
    console.log(`  ${file}: patched`);
}

// Same, but for a change only some Ora versions need: no match is fine, more than one is not.
function patchIfPresent(file, from, to) {
    const text = fs.readFileSync(path.join(dist, file), "utf8");
    const count = text.split(from).length - 1;
    if (count === 0 && !text.includes(to)) {
        console.log(`  ${file}: not needed for this Ora version`);
        return;
    }
    patch(file, from, to, 1);
}

// 1. Frames: load linux-frames.js instead of the Windows D3D11 addon.
fs.copyFileSync(path.join(__dirname, "linux-frames.js"), path.join(dist, "linux-frames.js"));
patch("native.js",
    "module.exports = require('./ora-windows.node');",
    "// Linux: frames go through a shared tmpfs folder (linux-frames.js) instead of the Windows D3D11 addon.\n" +
    "module.exports = process.platform === 'linux' ? require('./linux-frames.js') : require('./ora-windows.node');",
    1);

// 2. Offscreen rendering: CPU bitmaps on Linux (no shared GPU texture).
patch("windowmanager.js",
    "offscreen: openBrowser ? false : { useSharedTexture: true },",
    "offscreen: openBrowser ? false : { useSharedTexture: process.platform !== 'linux' },",
    2);
// The wrapper's sharedTextureHandle is where older Ora reads the handle; linuxImage is for the next patch.
patch("windowmanager.js",
    `        const texture = a.texture;`,
    `        // Linux: no shared texture; wrap the CPU bitmap (NativeImage c) so the code below is unchanged.
        const texture = a.texture || (process.platform === 'linux' && c ? {
            linuxImage: c,
            textureInfo: { widgetType: 'frame', pixelFormat: 'bgra', sharedTextureHandle: c, codedSize: c.getSize() },
            release() { },
        } : undefined);`,
    1);
// Newer Ora reads the NT handle from textureInfo.handle.ntHandle and rejects anything that is not a Buffer.
// On Linux, hand linux-frames.js the NativeImage instead. Older Ora has no such check.
patchIfPresent("windowmanager.js",
    `            const ntHandle = (_a = texture.textureInfo.handle) === null || _a === void 0 ? void 0 : _a.ntHandle;
            if (!Buffer.isBuffer(ntHandle) || ntHandle.length < 8)`,
    `            // Linux: the "handle" is the CPU bitmap (NativeImage) that linux-frames.js reads.
            const ntHandle = texture.linuxImage || ((_a = texture.textureInfo.handle) === null || _a === void 0 ? void 0 : _a.ntHandle);
            if (!texture.linuxImage && (!Buffer.isBuffer(ntHandle) || ntHandle.length < 8))`);

// 3. Local multiplayer hub: report the editor PID the launcher passes (differs from the host PID in Flatpak).
patch(path.join("relay", "relay.js"),
    "this.parentPid = (_e = options.parentPid) !== null && _e !== void 0 ? _e : process.ppid;",
    "this.parentPid = (_e = options.parentPid) !== null && _e !== void 0 ? _e : (Number(process.env.ORA_EDITOR_PID) || process.ppid); // Linux: editor PID from the launcher",
    1);

// 4. Remove the Windows-only native addons (audio then plays straight from the browser).
for (const addon of ["ora-windows.node", "ora-audio-ring.node", path.join("Windows", "src", "ora-windows.node")]) {
    fs.rmSync(path.join(dist, addon), { force: true });
}
console.log("  removed Windows native addons");
