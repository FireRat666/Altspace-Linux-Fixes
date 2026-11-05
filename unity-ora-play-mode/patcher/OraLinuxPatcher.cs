// Patches SideQuest.Ora.dll so the Ora browser works in the Unity editor on Linux.
// Adds the helper class SideQuest.Ora.OraLinuxShm and routes two things through it:
//
// 1. Frames: Ora's Windows backend hands frames over in a Windows named memory mapping,
//    which Mono on Linux can't open across processes. The Linux backend writes each frame
//    to /tmp/ora-frames/<name>; OraView's MemoryMappedFile.OpenExisting call goes through
//    OraLinuxShm.Open, which opens that file (original call on other platforms, or when
//    the file is missing).
// 2. Priority: StartWindowsProcess sets the browser to RealTime priority, which Linux
//    refuses without root. The exception skipped recording the process, so Unity couldn't
//    stop it later. The setters go through helpers that do nothing on Linux.
//
// Usage: mono OraLinuxPatcher.exe <path/to/SideQuest.Ora.dll>
// Idempotent: exits 0 without changes when the DLL already has every patch. A DLL with an
// older version of the patch is re-patched from the original kept in <dll>.orig~.
using System;
using System.IO;
using System.Linq;
using Mono.Cecil;
using Mono.Cecil.Cil;

static class OraLinuxPatcher
{
    const string HelperNamespace = "SideQuest.Ora";
    const string HelperName = "OraLinuxShm";
    // Present only in DLLs with every patch from this version of the patcher. Bump when patches change.
    const string LatestMarker = "PatchV3";
    // Where the Linux browser writes frames. A plain tmpfs folder: inside the Unity Hub Flatpak,
    // /tmp is shared with the host (as $XDG_RUNTIME_DIR/.flatpak/<app>/tmp) but /dev/shm is not.
    const string FrameDir = "/tmp/ora-frames/";

    static int Main(string[] args)
    {
        if (args.Length != 1) { Console.Error.WriteLine("usage: OraLinuxPatcher <SideQuest.Ora.dll>"); return 2; }
        var path = Path.GetFullPath(args[0]);
        var resolver = new DefaultAssemblyResolver();
        resolver.AddSearchDirectory(Path.GetDirectoryName(path));

        var backup = path + ".orig~"; // trailing ~ : Unity ignores it
        byte[] current = File.ReadAllBytes(path);
        var module = Read(current, resolver);
        var existing = module.Types.FirstOrDefault(t => t.Namespace == HelperNamespace && t.Name == HelperName);
        if (existing != null && existing.Methods.Any(m => m.Name == LatestMarker))
        {
            Console.WriteLine("already patched: " + path);
            return 0;
        }

        byte[] original = current;
        if (existing != null)
        {
            // Older patch: start again from the untouched DLL.
            if (!File.Exists(backup)) { Console.Error.WriteLine("older patch found but no original at " + backup); return 1; }
            original = File.ReadAllBytes(backup);
            module = Read(original, resolver);
        }

        var calls = module.Types.SelectMany(AllTypes)
            .SelectMany(t => t.Methods).Where(m => m.HasBody)
            .SelectMany(m => m.Body.Instructions.Select(i => new { m, i }))
            .Where(x => (x.i.OpCode == OpCodes.Call || x.i.OpCode == OpCodes.Callvirt) && x.i.Operand is MethodReference)
            .ToList();

        // 1. Every MemoryMappedFile.OpenExisting(string, MemoryMappedFileRights) call.
        var openSites = calls.Where(x => x.i.OpCode == OpCodes.Call && x.i.Operand is MethodReference mr
                        && mr.Name == "OpenExisting"
                        && mr.DeclaringType.FullName == "System.IO.MemoryMappedFiles.MemoryMappedFile"
                        && mr.Parameters.Count == 2)
            .ToList();
        if (openSites.Count == 0) { Console.Error.WriteLine("no OpenExisting call found; Ora version not supported"); return 1; }

        var openExisting = (MethodReference)openSites[0].i.Operand;
        var helperType = CreateHelperType(module);
        var open = BuildOpen(module, helperType, openExisting);
        foreach (var s in openSites)
        {
            s.i.OpCode = OpCodes.Call;
            s.i.Operand = open;
            Console.WriteLine("patched OpenExisting in " + s.m.FullName);
        }

        // 2. Process.PriorityClass / PriorityBoostEnabled setters.
        foreach (var setterName in new[] { "set_PriorityClass", "set_PriorityBoostEnabled" })
        {
            var sites = calls.Where(x => x.i.Operand is MethodReference mr && mr.Name == setterName
                            && mr.DeclaringType.FullName == "System.Diagnostics.Process").ToList();
            if (sites.Count == 0) continue;
            var skip = BuildSkipOnUnix(module, helperType, "Set" + setterName.Substring(4), (MethodReference)sites[0].i.Operand);
            foreach (var s in sites)
            {
                s.i.OpCode = OpCodes.Call;
                s.i.Operand = skip;
                Console.WriteLine("patched " + setterName + " in " + s.m.FullName);
            }
        }
        BuildSkipOnUnix(module, helperType, LatestMarker, null); // empty marker: "already patched" check

        if (!File.Exists(backup)) File.WriteAllBytes(backup, original);
        module.Write(path);
        Console.WriteLine("patched: " + path + " (original: " + backup + ")");
        return 0;
    }

    static ModuleDefinition Read(byte[] bytes, IAssemblyResolver resolver) =>
        // Read from memory (fully, not lazily) so the file can be overwritten afterwards.
        ModuleDefinition.ReadModule(new MemoryStream(bytes),
            new ReaderParameters { AssemblyResolver = resolver, ReadingMode = ReadingMode.Immediate });

    static TypeDefinition CreateHelperType(ModuleDefinition module)
    {
        var type = new TypeDefinition(HelperNamespace, HelperName,
            TypeAttributes.Public | TypeAttributes.Abstract | TypeAttributes.Sealed | TypeAttributes.BeforeFieldInit,
            module.TypeSystem.Object);
        module.Types.Add(type);
        return type;
    }

    // Emits: if (Environment.OSVersion.Platform == PlatformID.Unix) goto target;
    static void EmitBranchIfUnix(ModuleDefinition module, IMetadataScope bcl, ILProcessor il, Instruction target)
    {
        var tOs = new TypeReference("System", "OperatingSystem", module, bcl);
        var tPlatformId = new TypeReference("System", "PlatformID", module, bcl, true);
        var tEnvironment = new TypeReference("System", "Environment", module, bcl);
        il.Emit(OpCodes.Call, new MethodReference("get_OSVersion", tOs, tEnvironment));
        il.Emit(OpCodes.Callvirt, new MethodReference("get_Platform", tPlatformId, tOs) { HasThis = true });
        il.Emit(OpCodes.Ldc_I4_4); // PlatformID.Unix
        il.Emit(OpCodes.Beq, target);
    }

    // public static void <name>(Process p, T value) { if (!Unix) p.<setter>(value); }
    // With setter == null, an empty marker method.
    static MethodDefinition BuildSkipOnUnix(ModuleDefinition module, TypeDefinition type, string name, MethodReference setter)
    {
        var m = new MethodDefinition(name, MethodAttributes.Public | MethodAttributes.Static | MethodAttributes.HideBySig, module.TypeSystem.Void);
        type.Methods.Add(m);
        var il = m.Body.GetILProcessor();
        if (setter == null) { il.Emit(OpCodes.Ret); return m; }
        m.Parameters.Add(new ParameterDefinition("process", ParameterAttributes.None, setter.DeclaringType));
        m.Parameters.Add(new ParameterDefinition("value", ParameterAttributes.None, setter.Parameters[0].ParameterType));
        var ret = il.Create(OpCodes.Ret);
        EmitBranchIfUnix(module, setter.DeclaringType.Scope, il, ret);
        il.Emit(OpCodes.Ldarg_0);
        il.Emit(OpCodes.Ldarg_1);
        il.Emit(OpCodes.Callvirt, setter);
        il.Append(ret);
        return m;
    }

    static System.Collections.Generic.IEnumerable<TypeDefinition> AllTypes(TypeDefinition t)
    {
        yield return t;
        foreach (var n in t.NestedTypes)
            foreach (var x in AllTypes(n)) yield return x;
    }

    // public static MemoryMappedFile Open(string name, MemoryMappedFileRights rights)
    // {
    //     if (Environment.OSVersion.Platform == PlatformID.Unix)
    //     {
    //         string p = "/tmp/ora-frames/" + name;
    //         if (File.Exists(p))
    //             return MemoryMappedFile.CreateFromFile(p, FileMode.Open, null, 0L, MemoryMappedFileAccess.ReadWrite);
    //     }
    //     return MemoryMappedFile.OpenExisting(name, rights);
    // }
    static MethodDefinition BuildOpen(ModuleDefinition module, TypeDefinition type, MethodReference openExisting)
    {
        // Reuse the scope the DLL already references for BCL types (netstandard).
        var bcl = openExisting.DeclaringType.Scope;
        var mmf = openExisting.DeclaringType;
        var rightsType = openExisting.Parameters[1].ParameterType;
        TypeReference T(string ns, string name, bool valueType = false) =>
            new TypeReference(ns, name, module, bcl, valueType);

        var tString = module.TypeSystem.String;
        var tBool = module.TypeSystem.Boolean;
        var tInt64 = module.TypeSystem.Int64;
        var tEnvironment = T("System", "Environment");
        var tOs = T("System", "OperatingSystem");
        var tPlatformId = T("System", "PlatformID", true);
        var tFile = T("System.IO", "File");
        var tFileMode = T("System.IO", "FileMode", true);
        var tAccess = T("System.IO.MemoryMappedFiles", "MemoryMappedFileAccess", true);

        var getOsVersion = new MethodReference("get_OSVersion", tOs, tEnvironment);
        var getPlatform = new MethodReference("get_Platform", tPlatformId, tOs) { HasThis = true };
        var concat = new MethodReference("Concat", tString, tString);
        concat.Parameters.Add(new ParameterDefinition(tString));
        concat.Parameters.Add(new ParameterDefinition(tString));
        var exists = new MethodReference("Exists", tBool, tFile);
        exists.Parameters.Add(new ParameterDefinition(tString));
        var createFromFile = new MethodReference("CreateFromFile", mmf, mmf);
        createFromFile.Parameters.Add(new ParameterDefinition(tString));
        createFromFile.Parameters.Add(new ParameterDefinition(tFileMode));
        createFromFile.Parameters.Add(new ParameterDefinition(tString));
        createFromFile.Parameters.Add(new ParameterDefinition(tInt64));
        createFromFile.Parameters.Add(new ParameterDefinition(tAccess));

        var m = new MethodDefinition("Open", MethodAttributes.Public | MethodAttributes.Static | MethodAttributes.HideBySig, mmf);
        m.Parameters.Add(new ParameterDefinition("name", ParameterAttributes.None, tString));
        m.Parameters.Add(new ParameterDefinition("rights", ParameterAttributes.None, rightsType));
        type.Methods.Add(m);

        var pathVar = new VariableDefinition(tString);
        m.Body.Variables.Add(pathVar);
        m.Body.InitLocals = true;
        var il = m.Body.GetILProcessor();
        var fallback = il.Create(OpCodes.Ldarg_0);

        il.Emit(OpCodes.Call, getOsVersion);
        il.Emit(OpCodes.Callvirt, getPlatform);
        il.Emit(OpCodes.Ldc_I4_4); // PlatformID.Unix
        il.Emit(OpCodes.Bne_Un, fallback);
        il.Emit(OpCodes.Ldstr, FrameDir);
        il.Emit(OpCodes.Ldarg_0);
        il.Emit(OpCodes.Call, concat);
        il.Emit(OpCodes.Stloc, pathVar);
        il.Emit(OpCodes.Ldloc, pathVar);
        il.Emit(OpCodes.Call, exists);
        il.Emit(OpCodes.Brfalse, fallback);
        il.Emit(OpCodes.Ldloc, pathVar);
        il.Emit(OpCodes.Ldc_I4_3); // FileMode.Open
        il.Emit(OpCodes.Ldnull);
        il.Emit(OpCodes.Ldc_I8, 0L);
        il.Emit(OpCodes.Ldc_I4_0); // MemoryMappedFileAccess.ReadWrite
        il.Emit(OpCodes.Call, createFromFile);
        il.Emit(OpCodes.Ret);
        il.Append(fallback);
        il.Emit(OpCodes.Ldarg_1);
        il.Emit(OpCodes.Call, openExisting);
        il.Emit(OpCodes.Ret);
        return m;
    }
}
