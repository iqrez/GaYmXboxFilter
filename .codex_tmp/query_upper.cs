using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

public static class Native {
  [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
  public static extern SafeFileHandle CreateFileW(string name, uint access, uint share, IntPtr sa, uint creation, uint flags, IntPtr template);
  [DllImport("kernel32.dll", SetLastError=true)]
  public static extern bool DeviceIoControl(SafeFileHandle h, uint code, IntPtr inBuf, uint inSize, out GAYM_DEVICE_INFO outBuf, uint outSize, out uint bytesReturned, IntPtr ov);
}

[StructLayout(LayoutKind.Sequential)]
public struct GAYM_DEVICE_INFO {
  public UInt32 DeviceType; public UInt16 VendorId; public UInt16 ProductId; [MarshalAs(UnmanagedType.U1)] public bool OverrideActive;
  public UInt32 ReportsSent; public UInt32 PendingInputRequests; public UInt32 QueuedInputRequests; public UInt32 CompletedInputRequests; public UInt32 ForwardedInputRequests;
  public UInt32 LastInterceptedIoctl; public UInt32 ReadRequestsSeen; public UInt32 DeviceControlRequestsSeen; public UInt32 InternalDeviceControlRequestsSeen; public UInt32 WriteRequestsSeen;
}
public class Program {
  const uint GENERIC_READ = 0x80000000, GENERIC_WRITE = 0x40000000, FILE_SHARE_READ = 1, FILE_SHARE_WRITE = 2, OPEN_EXISTING = 3;
  static uint CTL(uint deviceType, uint function) { return (deviceType << 16) | (function << 2); }
  public static void Main() {
    SafeFileHandle h = Native.CreateFileW("\\\\.\\GaYmXInputFilterCtl", GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE, IntPtr.Zero, OPEN_EXISTING, 0, IntPtr.Zero);
    Console.WriteLine("upper valid=" + (!h.IsInvalid) + " err=" + Marshal.GetLastWin32Error()); if (h.IsInvalid) return;
    uint code = CTL(0x8000, 0x804); GAYM_DEVICE_INFO info; uint br;
    bool ok = Native.DeviceIoControl(h, code, IntPtr.Zero, 0, out info, (uint)Marshal.SizeOf(typeof(GAYM_DEVICE_INFO)), out br, IntPtr.Zero);
    Console.WriteLine("query ok=" + ok + " err=" + Marshal.GetLastWin32Error() + " bytes=" + br);
    Console.WriteLine("reports=" + info.ReportsSent + " queued=" + info.QueuedInputRequests + " completed=" + info.CompletedInputRequests + " forwarded=" + info.ForwardedInputRequests + " lastIoctl=0x" + info.LastInterceptedIoctl.ToString("X8") + " read=" + info.ReadRequestsSeen + " devctl=" + info.DeviceControlRequestsSeen + " internal=" + info.InternalDeviceControlRequestsSeen + " write=" + info.WriteRequestsSeen + " override=" + info.OverrideActive);
  }
}
