// Optional, per-test-process handle creation tracing. No system-wide verifier,
// registry changes or handle closure in the target process. Layout reference:
// https://github.com/winsiderss/phnt/blob/master/ntpsapi.h
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;

public static partial class ProcessHandleTypes {
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern uint GetProcessId(IntPtr process);
    [DllImport("ntdll.dll")]
    static extern int NtSetInformationProcess(IntPtr process, int infoClass, IntPtr buffer, int length);
    public static bool TraceEnabled;
    public static List<Dictionary<string, object>> LastProcessTraces = new List<Dictionary<string, object>>();

    public static void EnableTrace(int pid) {
        if (IntPtr.Size != 8) throw new InvalidOperationException("Handle traces require a 64-bit monitor.");
        var process = OpenProcess(0x600, false, pid);
        if (process == IntPtr.Zero) throw new System.ComponentModel.Win32Exception();
        var buffer = Marshal.AllocHGlobal(8);
        try {
            Marshal.WriteInt32(buffer, 0, 0);
            Marshal.WriteInt32(buffer, 4, 16384);
            int status = NtSetInformationProcess(process, 32, buffer, 8);
            if (status < 0) throw new InvalidOperationException("Enable handle trace: " + status.ToString("X"));
            TraceEnabled = true;
        } finally { Marshal.FreeHGlobal(buffer); CloseHandle(process); }
    }

    static Dictionary<string, object> TraceProcessHandle(IntPtr process, int pid, IntPtr handle, uint access) {
        var result = new Dictionary<string, object>();
        result["handle"] = handle.ToInt64().ToString("X");
        result["access"] = access.ToString("X");
        IntPtr duplicate;
        if (DuplicateHandle(process, handle, GetCurrentProcess(), out duplicate, 0, false, 2)) {
            try { result["processId"] = GetProcessId(duplicate); }
            finally { CloseHandle(duplicate); }
        }
        var entries = new List<object>();
        result["entries"] = entries;
        const int size = 65536;
        var buffer = Marshal.AllocHGlobal(size);
        try {
            Marshal.WriteIntPtr(buffer, handle);
            Marshal.WriteInt32(buffer, 8, 0);
            int returned;
            int status = NtQueryInformationProcess(process, 32, buffer, size, out returned);
            result["status"] = status.ToString("X");
            if (status < 0) return result;
            int count = Math.Min(Marshal.ReadInt32(buffer, 8), (size - 16) / 160);
            using (var target = Process.GetProcessById(pid)) {
                var modules = target.Modules;
                for (int i = 0; i < count; i++) {
                    int offset = 16 + i * 160;
                    if (Marshal.ReadInt32(buffer, offset + 24) != 1) continue;
                    var stack = new List<string>();
                    for (int frame = 0; frame < 16; frame++) {
                        long address = Marshal.ReadInt64(buffer, offset + 32 + frame * 8);
                        if (address == 0) continue;
                        string symbol = address.ToString("X");
                        foreach (ProcessModule module in modules) {
                            long start = module.BaseAddress.ToInt64();
                            if (address >= start && address < start + module.ModuleMemorySize) {
                                symbol = module.ModuleName + "+0x" + (address - start).ToString("X");
                                break;
                            }
                        }
                        stack.Add(symbol);
                    }
                    entries.Add(stack);
                }
            }
        } finally { Marshal.FreeHGlobal(buffer); }
        return result;
    }
}
