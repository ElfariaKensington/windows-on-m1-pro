/* SPDX-License-Identifier: MIT
 * Disposable Windows VM integration probe. Never binds physical Apple input.
 */
#include <ntddk.h>
#include <wdf.h>
#include <hidport.h>
#include <vhf.h>
#include "vhf_binding.h"
#include "protocol.h"
#include "probe-keyboard.h"

typedef struct { VHFHANDLE keyboard, mouse; WDFTIMER timer; ULONG tick; } PROBE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(PROBE, Probe)
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD Add;
EVT_WDF_DEVICE_D0_ENTRY On;
EVT_WDF_DEVICE_D0_EXIT Off;
EVT_WDF_OBJECT_CONTEXT_CLEANUP Cleanup;
EVT_WDF_TIMER Tick;
EVT_VHF_ASYNC_OPERATION Output;

VOID Output(PVOID context, VHFOPERATIONHANDLE op, PVOID extra, PHID_XFER_PACKET packet)
{
    UNREFERENCED_PARAMETER(context); UNREFERENCED_PARAMETER(extra); UNREFERENCED_PARAMETER(packet);
    VhfAsyncOperationComplete(op, STATUS_SUCCESS);
}
static VOID send(VHFHANDLE hid, UCHAR *data, ULONG size)
{
    HID_XFER_PACKET packet = {data, size, data[0]};
    (void)VhfReadReportSubmit(hid, &packet);
}
VOID Tick(WDFTIMER timer)
{
    PROBE *p = Probe(WdfTimerGetParentObject(timer));
    BOOLEAN pressed = (++p->tick & 1) != 0;
    UCHAR keys[10] = {1,0,0,0,0,0,0,0,0,0};
    UCHAR mouse[8] = {1,0,0,0,0,0,0,0};
    keys[3] = pressed ? 4 : 0;
    SHORT dx = pressed ? 8 : -8;
    mouse[2] = (UCHAR)dx; mouse[3] = (UCHAR)(dx >> 8);
    send(p->keyboard, keys, sizeof(keys)); send(p->mouse, mouse, sizeof(mouse));
    WdfTimerStart(timer, WDF_REL_TIMEOUT_IN_MS(250));
}
NTSTATUS On(WDFDEVICE device, WDF_POWER_DEVICE_STATE previous)
{
    UNREFERENCED_PARAMETER(previous);
    WdfTimerStart(Probe(device)->timer, WDF_REL_TIMEOUT_IN_MS(500)); return STATUS_SUCCESS;
}
NTSTATUS Off(WDFDEVICE device, WDF_POWER_DEVICE_STATE target)
{
    UNREFERENCED_PARAMETER(target);
    PROBE *p = Probe(device); WdfTimerStop(p->timer, TRUE);
    UCHAR keys[10] = {1}, mouse[8] = {1};
    send(p->keyboard, keys, sizeof(keys)); send(p->mouse, mouse, sizeof(mouse));
    return STATUS_SUCCESS;
}
VOID Cleanup(WDFOBJECT object)
{
    PROBE *p = Probe(object);
    if (p->keyboard) VhfDelete(p->keyboard, TRUE);
    if (p->mouse) VhfDelete(p->mouse, TRUE);
}
static NTSTATUS create(WDFDEVICE device, UCHAR *descriptor, USHORT size,
                        BOOLEAN keyboard, VHFHANDLE *hid)
{
    static WCHAR keyboard_id[] = L"ProbeKeyboard", mouse_id[] = L"ProbeTrackpad";
    VHF_CONFIG config;
    VHF_CONFIG_INIT(&config, WdfDeviceWdmGetDeviceObject(device), size, descriptor);
    config.VendorID = 0x05ac; config.ProductID = 0x343; config.VersionNumber = 1;
    ji_vhf_binding(&config, keyboard ? keyboard_id : mouse_id, sizeof(keyboard_id));
    if (keyboard) config.EvtVhfAsyncOperationWriteReport = Output;
    NTSTATUS status = VhfCreate(&config, hid);
    return NT_SUCCESS(status) ? VhfStart(*hid) : status;
}
NTSTATUS Add(WDFDRIVER driver, PWDFDEVICE_INIT init)
{
    UNREFERENCED_PARAMETER(driver);
    WDF_PNPPOWER_EVENT_CALLBACKS power; WDF_OBJECT_ATTRIBUTES a; WDFDEVICE device;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&power); power.EvtDeviceD0Entry = On; power.EvtDeviceD0Exit = Off;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &power);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a, PROBE);
    a.ExecutionLevel = WdfExecutionLevelPassive; a.EvtCleanupCallback = Cleanup;
    NTSTATUS status = WdfDeviceCreate(&init, &a, &device);
    if (!NT_SUCCESS(status)) return status;
    PROBE *p = Probe(device); WDF_TIMER_CONFIG timer;
    WDF_TIMER_CONFIG_INIT(&timer, Tick); timer.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&a); a.ParentObject = device; a.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfTimerCreate(&timer, &a, &p->timer); if (!NT_SUCCESS(status)) return status;
    status = create(device, keyboard_descriptor, sizeof(keyboard_descriptor), TRUE, &p->keyboard);
    if (!NT_SUCCESS(status)) return status;
    return create(device, (UCHAR *)ji_mouse_descriptor, (USHORT)ji_mouse_descriptor_size, FALSE, &p->mouse);
}
NTSTATUS DriverEntry(PDRIVER_OBJECT object, PUNICODE_STRING path)
{
    WDF_DRIVER_CONFIG config; WDF_DRIVER_CONFIG_INIT(&config, Add);
    return WdfDriverCreate(object, path, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
