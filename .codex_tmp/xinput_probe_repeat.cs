using System;
using System.Runtime.InteropServices;
public static class XInputProbe {
  [StructLayout(LayoutKind.Sequential)] public struct XINPUT_GAMEPAD { public ushort wButtons; public byte bLeftTrigger; public byte bRightTrigger; public short sThumbLX; public short sThumbLY; public short sThumbRX; public short sThumbRY; }
  [StructLayout(LayoutKind.Sequential)] public struct XINPUT_STATE { public uint dwPacketNumber; public XINPUT_GAMEPAD Gamepad; }
  [DllImport("xinput1_4.dll", EntryPoint="XInputGetState")] public static extern uint XInputGetState(uint dwUserIndex, out XINPUT_STATE pState);
  public static void Main() {
    for (int j = 0; j < 10; j++) {
      XINPUT_STATE s; uint r = XInputGetState(0, out s);
      Console.WriteLine(string.Format("sample={0} result={1} pkt={2} LX={3} Btn={4}", j, r, r==0 ? s.dwPacketNumber : 0, r==0 ? s.Gamepad.sThumbLX : 0, r==0 ? s.Gamepad.wButtons : 0));
      System.Threading.Thread.Sleep(200);
    }
  }
}
