// Read-only Windows diagnostic helper. Enumerates handle TYPES, never names or
// object contents, in the explicitly selected test process. All duplicates close.
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static partial class ProcessHandleTypes {
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll")]
    static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DuplicateHandle(IntPtr source, IntPtr handle, IntPtr target,
        out IntPtr duplicate, uint access, bool inherit, uint options);
    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool QueryFullProcessImageName(IntPtr process, uint flags, StringBuilder name, ref uint size);
    [DllImport("ntdll.dll")]
    static extern int NtQueryInformationProcess(IntPtr process, int infoClass,
        IntPtr buffer, int length, out int returned);
    [DllImport("ntdll.dll")]
    static extern int NtQueryObject(IntPtr handle, int infoClass,
        IntPtr buffer, int length, out int returned);

    [StructLayout(LayoutKind.Sequential)]
    struct Entry {
        public IntPtr Handle, HandleCount, PointerCount;
        public uint GrantedAccess, ObjectTypeIndex, Attributes, Reserved;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct UnicodeString {
        public ushort Length, MaximumLength;
        public IntPtr Buffer;
    }
    static readonly Dictionary<uint, string> Names = new Dictionary<uint, string>();

    public static Dictionary<string, int> Sample(int pid) {
        LastProcessTraces.Clear();
        var process = OpenProcess(0x440, false, pid); // QUERY_INFORMATION | DUP_HANDLE
        if (process == IntPtr.Zero) throw new System.ComponentModel.Win32Exception();
        IntPtr buffer = IntPtr.Zero;
        try {
            int size = 65536, returned;
            while (true) {
                buffer = Marshal.AllocHGlobal(size);
                int status = NtQueryInformationProcess(process, 51, buffer, size, out returned);
                if (status >= 0) break;
                Marshal.FreeHGlobal(buffer); buffer = IntPtr.Zero;
                if (status != unchecked((int)0xc0000004) || size >= 16777216)
                    throw new InvalidOperationException("Handle snapshot status: " + status.ToString("X"));
                size = Math.Max(size * 2, returned);
            }
            long count = Marshal.ReadIntPtr(buffer).ToInt64();
            int stride = Marshal.SizeOf(typeof(Entry));
            var totals = new Dictionary<string, int>();
            for (long i = 0; i < count; i++) {
                var entry = (Entry)Marshal.PtrToStructure(IntPtr.Add(buffer,
                    checked(2 * IntPtr.Size + (int)i * stride)), typeof(Entry));
                string name;
                if (!Names.TryGetValue(entry.ObjectTypeIndex, out name)) {
                    IntPtr duplicate;
                    if (!DuplicateHandle(process, entry.Handle, GetCurrentProcess(), out duplicate, 0, false, 2)) continue;
                    var typeBuffer = Marshal.AllocHGlobal(4096);
                    try {
                        if (NtQueryObject(duplicate, 2, typeBuffer, 4096, out returned) < 0) continue;
                        var text = (UnicodeString)Marshal.PtrToStructure(typeBuffer, typeof(UnicodeString));
                        name = Marshal.PtrToStringUni(text.Buffer, text.Length / 2);
                        Names[entry.ObjectTypeIndex] = name;
                    } finally { Marshal.FreeHGlobal(typeBuffer); CloseHandle(duplicate); }
                }
                if (name == "Process") {
                    if (TraceEnabled) LastProcessTraces.Add(TraceProcessHandle(process, pid, entry.Handle, entry.GrantedAccess));
                    IntPtr duplicate;
                    if (DuplicateHandle(process, entry.Handle, GetCurrentProcess(), out duplicate, 0, false, 2)) {
                        try {
                            var image = new StringBuilder(32768); uint length = 32768;
                            if (QueryFullProcessImageName(duplicate, 0, image, ref length))
                                name += ":" + System.IO.Path.GetFileName(image.ToString());
                        } finally { CloseHandle(duplicate); }
                    }
                }
                int previous; totals.TryGetValue(name, out previous); totals[name] = previous + 1;
            }
            return totals;
        } finally { if (buffer != IntPtr.Zero) Marshal.FreeHGlobal(buffer); CloseHandle(process); }
    }
}
