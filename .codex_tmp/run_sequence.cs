using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
using System.Threading;

public static class Native {
  [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
  public static extern SafeFileHandle CreateFileW(string name, uint access, uint share, IntPtr sa, uint creation, uint flags, IntPtr template);
  [DllImport("kernel32.dll", SetLastError=true)]
  public static extern bool DeviceIoControl(SafeFileHandle h, uint code, byte[] inBuf, uint inSize, IntPtr outBuf, uint outSize, out uint bytesReturned, IntPtr ov);
}

[StructLayout(LayoutKind.Sequential, Pack=1)]
public struct GAYM_REPORT {
  public byte ReportId;
  [MarshalAs(UnmanagedType.ByValArray, SizeConst=4)] public byte[] Buttons;
  public byte DPad;
  public byte TriggerLeft;
  public byte TriggerRight;
  public short ThumbLeftX;
  public short ThumbLeftY;
  public short ThumbRightX;
  public short ThumbRightY;
  [MarshalAs(UnmanagedType.ByValArray, SizeConst=32)] public byte[] Reserved;
}

public class Program {
  const uint GENERIC_READ = 0x80000000, GENERIC_WRITE = 0x40000000, FILE_SHARE_READ = 1, FILE_SHARE_WRITE = 2, OPEN_EXISTING = 3;
  const uint DEV = 0x8000;
  static uint CTL(uint fn) { return (DEV << 16) | (fn << 2); }
  static readonly uint IOCTL_ACQUIRE = CTL(0x800);
  static readonly uint IOCTL_ON = CTL(0x801);
  static readonly uint IOCTL_OFF = CTL(0x802);
  static readonly uint IOCTL_INJECT = CTL(0x803);
  static readonly uint IOCTL_RELEASE = CTL(0x809);
  const byte GAYM_BTN_A = 0x01;
  const byte GAYM_DPAD_NEUTRAL = 0x0F;

  static byte[] ToBytes(GAYM_REPORT r) {
    int size = Marshal.SizeOf(typeof(GAYM_REPORT));
    IntPtr ptr = Marshal.AllocHGlobal(size);
    try {
      Marshal.StructureToPtr(r, ptr, false);
      byte[] bytes = new byte[size];
      Marshal.Copy(ptr, bytes, 0, size);
      return bytes;
    } finally { Marshal.FreeHGlobal(ptr); }
  }

  static GAYM_REPORT Neutral() {
    return new GAYM_REPORT {
      Buttons = new byte[4],
      DPad = GAYM_DPAD_NEUTRAL,
      Reserved = new byte[32]
    };
  }

  static void SendIoctl(SafeFileHandle h, uint code) {
    uint br;
    if (!Native.DeviceIoControl(h, code, null, 0, IntPtr.Zero, 0, out br, IntPtr.Zero)) {
      throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(), "ioctl 0x" + code.ToString("X8"));
    }
  }

  static void Inject(SafeFileHandle h, GAYM_REPORT report) {
    uint br;
    byte[] bytes = ToBytes(report);
    if (!Native.DeviceIoControl(h, IOCTL_INJECT, bytes, (uint)bytes.Length, IntPtr.Zero, 0, out br, IntPtr.Zero)) {
      throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(), "inject");
    }
  }

  static void Hold(SafeFileHandle h, GAYM_REPORT report, int ms) {
    int end = Environment.TickCount + ms;
    while (Environment.TickCount < end) {
      Inject(h, report);
      Thread.Sleep(8);
    }
  }

  public static void Main() {
    SafeFileHandle h = Native.CreateFileW("\\\\.\\GaYmXInputFilterCtl", GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE, IntPtr.Zero, OPEN_EXISTING, 0, IntPtr.Zero);
    if (h.IsInvalid) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(), "open upper ctl");

    try {
      SendIoctl(h, IOCTL_ACQUIRE);
      SendIoctl(h, IOCTL_ON);

      GAYM_REPORT r;

      r = Neutral();
      r.Buttons[0] = GAYM_BTN_A;
      Console.WriteLine("A 1s");
      Hold(h, r, 1000);

      r = Neutral();
      r.ThumbLeftX = 32767;
      Console.WriteLine("LX right 2s");
      Hold(h, r, 2000);

      r = Neutral();
      r.ThumbLeftX = -32767;
      Console.WriteLine("LX left 2s");
      Hold(h, r, 2000);

      r = Neutral();
      r.ThumbRightY = -32767;
      Console.WriteLine("RY up 2s");
      Hold(h, r, 2000);

      r = Neutral();
      r.TriggerLeft = 255;
      Console.WriteLine("LT full 2s");
      Hold(h, r, 2000);

      r = Neutral();
      Console.WriteLine("neutral 1s");
      Hold(h, r, 1000);

      SendIoctl(h, IOCTL_OFF);
      SendIoctl(h, IOCTL_RELEASE);
      Console.WriteLine("Sequence complete.");
    } finally {
      h.Close();
    }
  }
}
