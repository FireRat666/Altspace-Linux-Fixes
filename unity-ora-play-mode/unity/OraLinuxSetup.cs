#if UNITY_EDITOR_LINUX
// Lets the Ora browser (com.sidequest.ora) run in Play mode on the Linux editor.
//
// Ora only ships a Windows browser (ora-windows.exe). On Linux this script:
//   1. Points OraManager.browserFolderOverride at ~/.local/share/ora-linux/bin, whose
//      "ora-windows.exe" is a launcher for a Linux Electron build of the same Ora app.
//   2. Makes sure SideQuest.Ora.dll is patched to read browser frames from /tmp/ora-frames
//      (Windows named shared memory doesn't exist for Mono on Linux) and to skip the
//      RealTime priority boost Linux refuses. The patch is re-applied automatically when
//      the package is updated or re-downloaded.
// Setup lives outside the project in ~/.local/share/ora-linux (see README.md there).
using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Threading;
using UnityEditor;
using Debug = UnityEngine.Debug;

[InitializeOnLoad]
static class OraLinuxSetup
{
    const string Tag = "[Ora Linux]";
    const string OraPackage = "com.sidequest.ora";
    const string DllAssetPath = "Packages/com.sidequest.ora/Plugins/SideQuest.Ora.dll";

    static string Home => Environment.GetEnvironmentVariable("ORA_LINUX_HOME")
        ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".local/share/ora-linux");
    static string LauncherFolder => Path.Combine(Home, "bin");

    static OraLinuxSetup()
    {
        var oraAssembly = AppDomain.CurrentDomain.GetAssemblies()
            .FirstOrDefault(a => a.GetName().Name == "SideQuest.Ora");
        if (oraAssembly == null)
        {
            Debug.Log($"{Tag} SideQuest.Ora isn't loaded in this project; nothing to set up.");
            return;
        }

        if (!File.Exists(Path.Combine(LauncherFolder, "ora-windows.exe")))
        {
            Debug.LogWarning($"{Tag} No Linux browser at {LauncherFolder}. Ora views won't work in Play mode. See {Home}/README.md.");
            return;
        }

        var manager = oraAssembly.GetType("SideQuest.Ora.OraManager");
        var overrideField = manager?.GetField("browserFolderOverride", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static);
        if (overrideField == null)
        {
            Debug.LogWarning($"{Tag} This Ora version has no OraManager.browserFolderOverride; the Linux browser can't be used.");
            return;
        }
        overrideField.SetValue(null, LauncherFolder);

        // PatchV3 marks the current patch (see OraLinuxPatcher.LatestMarker); older patched DLLs get re-patched.
        if (oraAssembly.GetType("SideQuest.Ora.OraLinuxShm")?.GetMethod("PatchV3") == null)
        {
            // A patch that failed isn't retried on every reload (it would run each time the scripts compile): only once
            // the DLL changes, or from Tools > Ora Linux > Re-apply Ora DLL Patch.
            var dll = OraDllPath();
            if (dll != null && EditorPrefs.GetString(FailedKey, "") == DllStamp(dll))
            {
                if (!SessionState.GetBool("OraLinuxSetup.FailedLogged", false))
                {
                    SessionState.SetBool("OraLinuxSetup.FailedLogged", true);
                    Debug.LogWarning($"{Tag} SideQuest.Ora.dll isn't patched for Linux: the last patch failed (see the log). " +
                                     "Use Tools > Ora Linux > Re-apply Ora DLL Patch to try again.");
                }
                return;
            }
            Debug.Log($"{Tag} SideQuest.Ora.dll isn't fully patched for Linux yet; patching.");
            EditorApplication.delayCall += PatchOraDll;
        }
        else if (!SessionState.GetBool("OraLinuxSetup.Logged", false))
        {
            SessionState.SetBool("OraLinuxSetup.Logged", true);
            Debug.Log($"{Tag} Ora browser views will use the Linux browser in {LauncherFolder}.");
        }
    }

    const string FailedKey = "OraLinuxSetup.FailedFor";
    // How long the patcher may take before it's stopped. It runs off the main thread, so the editor never waits on it.
    const int PatchTimeoutMs = 60000;

    static string OraDllPath()
    {
        var package = UnityEditor.PackageManager.PackageInfo.FindForAssetPath("Packages/" + OraPackage);
        return package == null ? null : Path.Combine(package.resolvedPath, "Plugins", "SideQuest.Ora.dll");
    }

    // Which DLL a failure was for: its path, size and time, so a new or re-downloaded DLL is tried again.
    static string DllStamp(string dll)
    {
        var info = new FileInfo(dll);
        return info.Exists ? $"{dll}|{info.Length}|{info.LastWriteTimeUtc.Ticks}" : dll;
    }

    static bool patching;

    [MenuItem("Tools/Ora Linux/Re-apply Ora DLL Patch")]
    static void PatchOraDll()
    {
        if (patching) { Debug.Log($"{Tag} A patch is already running."); return; }
        var dll = OraDllPath();
        if (dll == null) { Debug.LogError($"{Tag} {OraPackage} not found."); return; }
        var patcher = Path.Combine(Home, "patcher", "OraLinuxPatcher.exe");
        if (!File.Exists(patcher)) { Debug.LogError($"{Tag} Patcher missing: {patcher}"); return; }

        var data = EditorApplication.applicationContentsPath;
        var monoDir = Path.Combine(data, "MonoBleedingEdge");
        // The assemblies SideQuest.Ora.dll references (UnityEngine.CoreModule and so on), for Cecil to resolve.
        var managed = Path.Combine(data, "Managed");
        var args = $"\"{patcher}\" \"{dll}\" \"{Path.Combine(managed, "UnityEngine")}\" \"{managed}\"";
        var psi = new ProcessStartInfo(Path.Combine(monoDir, "bin", "mono"), args)
        {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
        };
        psi.EnvironmentVariables["MONO_PATH"] = Path.Combine(monoDir, "lib", "mono", "4.5");

        patching = true;
        int exitCode = -1;
        string output = "";
        var done = false;
        new Thread(() =>
        {
            try
            {
                using (var p = Process.Start(psi))
                {
                    // Both streams at once, so a full stderr pipe can't stall the patcher.
                    var err = p.StandardError.ReadToEndAsync();
                    var outText = p.StandardOutput.ReadToEndAsync();
                    if (!p.WaitForExit(PatchTimeoutMs))
                    {
                        try { p.Kill(); } catch (Exception) { }
                        output = $"timed out after {PatchTimeoutMs / 1000} s and was stopped";
                    }
                    else
                    {
                        p.WaitForExit();
                        exitCode = p.ExitCode;
                        output = (outText.Wait(5000) ? outText.Result : "") + (err.Wait(5000) ? err.Result : "");
                    }
                }
            }
            catch (Exception e) { output = e.ToString(); }
            done = true;
        }) { IsBackground = true, Name = "Ora Linux patcher" }.Start();

        // Back on the main thread once it's finished.
        EditorApplication.CallbackFunction check = null;
        check = () =>
        {
            if (!done) return;
            EditorApplication.update -= check;
            patching = false;
            if (exitCode != 0)
            {
                EditorPrefs.SetString(FailedKey, DllStamp(dll));
                Debug.LogError($"{Tag} Patching SideQuest.Ora.dll failed:\n{output}");
                return;
            }
            EditorPrefs.DeleteKey(FailedKey);
            Debug.Log($"{Tag} {output.Trim()}");
            // Reimport so the editor reloads the patched assembly.
            AssetDatabase.ImportAsset(DllAssetPath, ImportAssetOptions.ForceUpdate);
        };
        EditorApplication.update += check;
    }
}
#endif
