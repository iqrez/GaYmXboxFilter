using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class EnumIfaces {
    [StructLayout(LayoutKind.Sequential)]
    public struct SP_DEVICE_INTERFACE_DATA {
        public int cbSize;
        public Guid InterfaceClassGuid;
        public int Flags;
        public IntPtr Reserved;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct SP_DEVICE_INTERFACE_DETAIL_DATA {
        public int cbSize;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 1024)]
        public string DevicePath;
    }

    const int DIGCF_PRESENT = 0x2;
    const int DIGCF_DEVICEINTERFACE = 0x10;

    [DllImport("setupapi.dll", SetLastError=true)]
    static extern IntPtr SetupDiGetClassDevs(ref Guid ClassGuid, IntPtr Enumerator, IntPtr hwndParent, int Flags);

    [DllImport("setupapi.dll", SetLastError=true)]
    static extern bool SetupDiEnumDeviceInterfaces(IntPtr DeviceInfoSet, IntPtr DeviceInfoData, ref Guid InterfaceClassGuid, int MemberIndex, ref SP_DEVICE_INTERFACE_DATA DeviceInterfaceData);

    [DllImport("setupapi.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr DeviceInfoSet, ref SP_DEVICE_INTERFACE_DATA DeviceInterfaceData, IntPtr DeviceInterfaceDetailData, int DeviceInterfaceDetailDataSize, out int RequiredSize, IntPtr DeviceInfoData);

    [DllImport("setupapi.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr DeviceInfoSet, ref SP_DEVICE_INTERFACE_DATA DeviceInterfaceData, ref SP_DEVICE_INTERFACE_DETAIL_DATA DeviceInterfaceDetailData, int DeviceInterfaceDetailDataSize, out int RequiredSize, IntPtr DeviceInfoData);

    [DllImport("setupapi.dll", SetLastError=true)]
    static extern bool SetupDiDestroyDeviceInfoList(IntPtr DeviceInfoSet);

    public static string[] Get(Guid guid) {
        var list = new List<string>();
        IntPtr info = SetupDiGetClassDevs(ref guid, IntPtr.Zero, IntPtr.Zero, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (info == new IntPtr(-1)) return list.ToArray();
        try {
            int index = 0;
            while (true) {
                var data = new SP_DEVICE_INTERFACE_DATA();
                data.cbSize = Marshal.SizeOf<SP_DEVICE_INTERFACE_DATA>();
                if (!SetupDiEnumDeviceInterfaces(info, IntPtr.Zero, ref guid, index, ref data)) {
                    break;
                }
                int required;
                SetupDiGetDeviceInterfaceDetail(info, ref data, IntPtr.Zero, 0, out required, IntPtr.Zero);
                var detail = new SP_DEVICE_INTERFACE_DETAIL_DATA();
                detail.cbSize = IntPtr.Size == 8 ? 8 : 6;
                if (SetupDiGetDeviceInterfaceDetail(info, ref data, ref detail, Marshal.SizeOf<SP_DEVICE_INTERFACE_DETAIL_DATA>(), out required, IntPtr.Zero)) {
                    list.Add(detail.DevicePath);
                }
                index++;
            }
        } finally {
            SetupDiDestroyDeviceInfoList(info);
        }
        return list.ToArray();
    }
}
