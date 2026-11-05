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
            Debug.Log($"{Tag} SideQuest.Ora.dll isn't fully patched for Linux yet; patching.");
            EditorApplication.delayCall += PatchOraDll;
        }
        else if (!SessionState.GetBool("OraLinuxSetup.Logged", false))
        {
            SessionState.SetBool("OraLinuxSetup.Logged", true);
            Debug.Log($"{Tag} Ora browser views will use the Linux browser in {LauncherFolder}.");
        }
    }

    [MenuItem("Tools/Ora Linux/Re-apply Ora DLL Patch")]
    static void PatchOraDll()
    {
        var package = UnityEditor.PackageManager.PackageInfo.FindForAssetPath("Packages/" + OraPackage);
        if (package == null) { Debug.LogError($"{Tag} {OraPackage} not found."); return; }
        var dll = Path.Combine(package.resolvedPath, "Plugins", "SideQuest.Ora.dll");
        var patcher = Path.Combine(Home, "patcher", "OraLinuxPatcher.exe");
        if (!File.Exists(patcher)) { Debug.LogError($"{Tag} Patcher missing: {patcher}"); return; }

        var monoDir = Path.Combine(EditorApplication.applicationContentsPath, "MonoBleedingEdge");
        var psi = new ProcessStartInfo(Path.Combine(monoDir, "bin", "mono"), $"\"{patcher}\" \"{dll}\"")
        {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
        };
        psi.EnvironmentVariables["MONO_PATH"] = Path.Combine(monoDir, "lib", "mono", "4.5");
        using (var p = Process.Start(psi))
        {
            var output = p.StandardOutput.ReadToEnd() + p.StandardError.ReadToEnd();
            p.WaitForExit();
            if (p.ExitCode != 0) { Debug.LogError($"{Tag} Patching SideQuest.Ora.dll failed:\n{output}"); return; }
            Debug.Log($"{Tag} {output.Trim()}");
        }
        // Reimport so the editor reloads the patched assembly.
        AssetDatabase.ImportAsset(DllAssetPath, ImportAssetOptions.ForceUpdate);
    }
}
#endif
