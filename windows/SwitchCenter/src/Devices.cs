// Access to the Switch drivers. All IOCTL structures are ULONG/LONG fields
// (see SwitchCpuClock\src\switchcpu_ioctl.h and SwitchFan\src\switchfan.c).
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32;
using Microsoft.Win32.SafeHandles;

namespace SwitchCenter
{
    static class Native
    {
        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        static extern SafeFileHandle CreateFile(string name, uint access, uint share, IntPtr sa, uint disposition, uint flags, IntPtr template);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool DeviceIoControl(SafeFileHandle h, uint code, byte[] inBuf, int inLen, byte[] outBuf, int outLen, out int returned, IntPtr ov);

        /// <summary>One IOCTL on \\.\Name. Throws Win32Exception on failure.</summary>
        public static byte[] Ioctl(string device, uint code, byte[] input, int outLen)
        {
            using SafeFileHandle h = CreateFile(device, 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
            if (h.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error());
            byte[] output = new byte[outLen];
            if (!DeviceIoControl(h, code, input, input?.Length ?? 0, output, outLen, out _, IntPtr.Zero))
                throw new Win32Exception(Marshal.GetLastWin32Error());
            return output;
        }

        public static int I32(byte[] b, int index) => BitConverter.ToInt32(b, index * 4);
    }

    /// <summary>switchcpu.sys (\\.\SwitchCpu): CPU profiles, clock, voltage, thermal guard.</summary>
    static class CpuClock
    {
        const string Device = @"\\.\SwitchCpu";
        const uint GetStatusCode = 0x00226000, GetProfilesCode = 0x00226004, SetProfileCode = 0x0022A008;
        public const int FlagExperimental = 1, FlagDefault = 2;

        public sealed class Status
        {
            public int Khz, Mv, Active, Requested, TempSoC, TempBoard, Switches;
            public bool Throttled, TempValid;
        }

        public sealed class Profile
        {
            public int Index, Khz, Mv, Flags;
            public bool Experimental => (Flags & FlagExperimental) != 0;
            public bool Default => (Flags & FlagDefault) != 0;
        }

        public static Status GetStatus()
        {
            byte[] b = Native.Ioctl(Device, GetStatusCode, null, 48);
            return new Status
            {
                Khz = Native.I32(b, 1), Mv = Native.I32(b, 2), Active = Native.I32(b, 3), Requested = Native.I32(b, 4),
                Throttled = Native.I32(b, 5) != 0, TempSoC = Native.I32(b, 6), TempBoard = Native.I32(b, 7),
                TempValid = Native.I32(b, 8) != 0, Switches = Native.I32(b, 10),
            };
        }

        public static Profile[] GetProfiles()
        {
            byte[] b = Native.Ioctl(Device, GetProfilesCode, null, 4 + 8 * 12);
            int count = Math.Min(Native.I32(b, 0), 8);
            var list = new Profile[count];
            for (int i = 0; i < count; i++)
                list[i] = new Profile { Index = i, Khz = Native.I32(b, 1 + i * 3), Mv = Native.I32(b, 2 + i * 3), Flags = Native.I32(b, 3 + i * 3) };
            return list;
        }

        public static void SetProfile(int index) => Native.Ioctl(Device, SetProfileCode, BitConverter.GetBytes(index), 0);
    }

    /// <summary>switchfan.sys v2 (\\.\SwitchBacklight): brightness 8..255 and the display state.</summary>
    static class Backlight
    {
        const string Device = @"\\.\SwitchBacklight";
        const uint GetCode = 0x00226040, SetCode = 0x0022A044;
        public const int Min = 8, Max = 255;

        public sealed class Status
        {
            public int Level, Duty, DisplayState;      // DisplayState: 0 off, 1 on, 2 dimmed
        }

        public static Status Get()
        {
            byte[] b = Native.Ioctl(Device, GetCode, null, 16);
            return new Status { Level = Native.I32(b, 1), Duty = Native.I32(b, 2), DisplayState = Native.I32(b, 3) };
        }

        public static void Set(int level) => Native.Ioctl(Device, SetCode, BitConverter.GetBytes(Math.Clamp(level, Min, Max)), 0);

        public static int PercentToLevel(int percent) => Math.Max(Min, percent * Max / 100);
        public static int LevelToPercent(int level) => level * 100 / Max;
    }

    /// <summary>The DWORD traces every driver writes under Services\name\Parameters.</summary>
    static class DriverTrace
    {
        public static int? Get(string service, string name)
        {
            using RegistryKey k = Registry.LocalMachine.OpenSubKey($@"SYSTEM\CurrentControlSet\Services\{service}\Parameters");
            return k?.GetValue(name) is int v ? v : null;
        }
    }
}
