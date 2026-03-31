using System;
using System.Runtime.InteropServices;
using System.Threading;
public static class XInputCapture {
  [StructLayout(LayoutKind.Sequential)] public struct XINPUT_GAMEPAD { public ushort wButtons; public byte bLeftTrigger; public byte bRightTrigger; public short sThumbLX; public short sThumbLY; public short sThumbRX; public short sThumbRY; }
  [StructLayout(LayoutKind.Sequential)] public struct XINPUT_STATE { public uint dwPacketNumber; public XINPUT_GAMEPAD Gamepad; }
  [DllImport("xinput1_4.dll", EntryPoint="XInputGetState")] public static extern uint XInputGetState(uint dwUserIndex, out XINPUT_STATE pState);
  public static void Main() {
    var start = Environment.TickCount;
    while (Environment.TickCount - start < 3500) {
      XINPUT_STATE s; uint r = XInputGetState(0, out s);
      if (r == 0) {
        Console.WriteLine(string.Format("t={0} pkt={1} LX={2} Btn=0x{3:X4}", Environment.TickCount - start, s.dwPacketNumber, s.Gamepad.sThumbLX, s.Gamepad.wButtons));
      } else {
        Console.WriteLine(string.Format("t={0} result={1}", Environment.TickCount - start, r));
      }
      Thread.Sleep(100);
    }
  }
}
