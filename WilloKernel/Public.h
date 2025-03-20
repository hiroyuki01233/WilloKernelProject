/*++

Module Name:

    public.h

Abstract:

    This module contains the common declarations shared by driver
    and user applications.

Environment:

    user and kernel

--*/

//
// Define an Interface Guid so that apps can find the device and talk to it.
//

DEFINE_GUID (GUID_DEVINTERFACE_WilloKernel,
    0xb1800615,0x03a7,0x4289,0x9d,0x74,0x16,0x7d,0x51,0x86,0xef,0xe0);
// {b1800615-03a7-4289-9d74-167d5186efe0}
