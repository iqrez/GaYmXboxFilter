using System;
using System.Runtime.InteropServices;
public static class XInputProbe {
  [StructLayout(LayoutKind.Sequential)] public struct XINPUT_GAMEPAD { public ushort wButtons; public byte bLeftTrigger; public byte bRightTrigger; public short sThumbLX; public short sThumbLY; public short sThumbRX; public short sThumbRY; }
  [StructLayout(LayoutKind.Sequential)] public struct XINPUT_STATE { public uint dwPacketNumber; public XINPUT_GAMEPAD Gamepad; }
  [DllImport("xinput1_4.dll", EntryPoint="XInputGetState")] public static extern uint XInputGetState14(uint idx, out XINPUT_STATE state);
  [DllImport("xinput9_1_0.dll", EntryPoint="XInputGetState")] public static extern uint XInputGetState910(uint idx, out XINPUT_STATE state);
  public static void Main() {
    for (uint i = 0; i < 4; i++) {
      XINPUT_STATE s;
      uint r14 = XInputGetState14(i, out s);
      Console.WriteLine(string.Format("xinput1_4 pad {0}: result={1} pkt={2} LX={3} LY={4} Btn=0x{5:X4}", i, r14, s.dwPacketNumber, s.Gamepad.sThumbLX, s.Gamepad.sThumbLY, s.Gamepad.wButtons));
      uint r910 = XInputGetState910(i, out s);
      Console.WriteLine(string.Format("xinput9_1_0 pad {0}: result={1} pkt={2} LX={3} LY={4} Btn=0x{5:X4}", i, r910, s.dwPacketNumber, s.Gamepad.sThumbLX, s.Gamepad.sThumbLY, s.Gamepad.wButtons));
    }
  }
}
