/* SPDX-License-Identifier: MIT */
#include <ntddk.h>
#include <wdf.h>
#include <hidport.h>
#include <vhf.h>
#include "protocol.h"

#define JI_TAG 'iS6J'
#define JI_SNAPSHOT CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED, FILE_READ_ACCESS)
typedef struct {
    ULONG version, phase, status, booted, vendor, product;
    ULONG packets, malformed, timeouts, keyboard, mouse, submit_errors, resets;
    ULONG spi_control, spi_fifo, gpio_input, descriptor_size;
} JI_DIAG;
typedef struct {
    WDFDEVICE device;
    WDFTIMER timer;
    WDFSPINLOCK output_lock;
    volatile LONG active;
    PUCHAR spi, ap, nub;
    VHFHANDLE keyboard_hid, mouse_hid;
    struct ji_assembly assembly;
    struct ji_mouse mouse;
    JI_DIAG diag;
    UCHAR sequence, pending_sequence, pending_command, pending_report, pending_interface;
    UCHAR response[JI_DESCRIPTOR], keyboard_descriptor[JI_DESCRIPTOR];
    ULONG response_size, keyboard_descriptor_size;
    BOOLEAN response_ready, request_pending, mt_pending, output_pending;
    UCHAR output[64];
    ULONG output_size;
} JI_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(JI_CONTEXT, JiContext)
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD JiAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE JiPrepare;
EVT_WDF_DEVICE_RELEASE_HARDWARE JiRelease;
EVT_WDF_DEVICE_D0_ENTRY JiPowerOn;
EVT_WDF_DEVICE_D0_EXIT JiPowerOff;
EVT_WDF_TIMER JiPoll;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL JiIoctl;
EVT_VHF_ASYNC_OPERATION JiOutput;

static ULONG rd(PUCHAR p, ULONG off) { return READ_REGISTER_ULONG((PULONG)(p + off)); }
static VOID wr(PUCHAR p, ULONG off, ULONG v) { WRITE_REGISTER_ULONG((PULONG)(p + off), v); }
static VOID modify(PUCHAR p, ULONG off, ULONG mask, ULONG v)
{ wr(p, off, (rd(p, off) & ~mask) | v); }
static VOID sleep_ms(ULONG ms)
{
    LARGE_INTEGER t; t.QuadPart = -(LONGLONG)ms * 10000;
    KeDelayExecutionThread(KernelMode, FALSE, &t);
}
static VOID chip_select(JI_CONTEXT *c, BOOLEAN selected)
{
    if (!selected) KeStallExecutionProcessor(65);
    wr(c->spi, 0x0c, selected ? 0 : 2);
    KeStallExecutionProcessor(selected ? 65 : 250);
}

/* TX-only and RX-only transfers: RX generates its own clocks on Apple SPI. */
static NTSTATUS transfer(JI_CONTEXT *c, const UCHAR *tx, UCHAR *rx, ULONG count)
{
    ULONG sent = 0, received = 0, wanted = tx ? 2 : 1;
    ULONGLONG end = KeQueryInterruptTime() + 2000000; /* 200 ms absolute */
    wr(c->spi, 0, 12);
    wr(c->spi, 0x134, ~0u); wr(c->spi, 0x13c, ~0u);
    wr(c->spi, 0x4c, tx ? count : 0); wr(c->spi, 0x34, rx ? count : 0);
    if (tx) while (sent < count && sent < 16) wr(c->spi, 0x10, tx[sent++]);
    wr(c->spi, 0, 1);
    for (;;) {
        ULONG fifo = rd(c->spi, 0x10c);
        if (rx) {
            ULONG available = fifo >> 24;
            if (available > count - received) goto bad;
            while (available--) rx[received++] = (UCHAR)rd(c->spi, 0x20);
        } else {
            ULONG level = (fifo >> 8) & 0xff;
            if (level > 16) goto bad;
            ULONG space = 16 - level;
            while (space-- && sent < count) wr(c->spi, 0x10, tx[sent++]);
        }
        if ((rd(c->spi, 0x134) & wanted) && (tx ? sent : received) == count) {
            wr(c->spi, 0, 0);
            return STATUS_SUCCESS;
        }
        if (rd(c->spi, 0x13c) & ((1u << 16) | (1u << 17))) goto bad;
        if (KeQueryInterruptTime() >= end) {
            ++c->diag.timeouts;
            wr(c->spi, 0, 12);
            return STATUS_IO_TIMEOUT;
        }
        KeStallExecutionProcessor(2);
    }
bad:
    wr(c->spi, 0, 12);
    return STATUS_DEVICE_DATA_ERROR;
}

static NTSTATUS send_command(JI_CONTEXT *c, UCHAR target, UCHAR command, UCHAR report,
                             UCHAR iface, USHORT reply, const UCHAR *data, ULONG size)
{
    UCHAR packet[JI_PACKET], ack[4];
    UCHAR seq = c->sequence++;
    if (!ji_request(packet, target, command, report, iface, seq, reply, data, size))
        return STATUS_INVALID_PARAMETER;
    c->pending_sequence = seq; c->pending_command = command;
    c->pending_report = report; c->pending_interface = iface;
    c->response_size = 0; c->response_ready = FALSE;
    chip_select(c, TRUE);
    NTSTATUS status = transfer(c, packet, NULL, sizeof(packet));
    if (NT_SUCCESS(status)) {
        KeStallExecutionProcessor(200);
        status = transfer(c, NULL, ack, sizeof(ack));
        if (NT_SUCCESS(status) && memcmp(ack, "\xac\x27\x68\xd5", 4))
            status = STATUS_DEVICE_DATA_ERROR;
    }
    chip_select(c, FALSE);
    return status;
}

static VOID submit(JI_CONTEXT *c, VHFHANDLE hid, UCHAR *data, ULONG size)
{
    if (!hid || !size) return;
    HID_XFER_PACKET packet = {data, size, data[0]};
    if (!NT_SUCCESS(VhfReadReportSubmit(hid, &packet))) ++c->diag.submit_errors;
}

static VOID message(void *context, uint8_t device, uint8_t flags,
                     const uint8_t *m, size_t length)
{
    JI_CONTEXT *c = context;
    ULONG size = (ULONG)length - 10;
    const UCHAR *payload = m + 8;
    if (flags == 0x40 && c->request_pending && m[3] == c->pending_sequence &&
        m[0] == c->pending_command && m[1] == c->pending_report &&
        m[2] == c->pending_interface && size <= sizeof(c->response)) {
        memcpy(c->response, payload, size); c->response_size = size;
        c->response_ready = TRUE;
    }
    if (flags != 0x20 || m[0] != 0x10 || !size) return;
    if (device == 1 && payload[0] == 1 && size == 10) {
        ++c->diag.keyboard;
        submit(c, c->keyboard_hid, (UCHAR *)payload, size);
    } else if (device == 2) {
        UCHAR mouse[8];
        int result = ji_trackpad(&c->mouse, payload, size, mouse);
        if (result == 1) {
            ++c->diag.mouse; submit(c, c->mouse_hid, mouse, sizeof(mouse));
        } else if (result == 2) {
            ++c->diag.resets; c->mt_pending = TRUE;
            RtlZeroMemory(mouse, sizeof(mouse)); mouse[0] = 1;
            submit(c, c->mouse_hid, mouse, sizeof(mouse));
        } else ++c->diag.malformed;
    }
}

static NTSTATUS read_packet(JI_CONTEXT *c)
{
    UCHAR packet[JI_PACKET];
    chip_select(c, TRUE);
    NTSTATUS status = transfer(c, NULL, packet, sizeof(packet));
    chip_select(c, FALSE);
    if (!NT_SUCCESS(status)) return status;
    ++c->diag.packets;
    int result = ji_receive(&c->assembly, packet, message, c);
    if (result == 2) c->diag.booted = 1;
    else if (result < 0) ++c->diag.malformed;
    return STATUS_SUCCESS;
}

static BOOLEAN input_pending(JI_CONTEXT *c) { return !(rd(c->nub, 6 * 4) & 1); }
static NTSTATUS wait_response(JI_CONTEXT *c, BOOLEAN boot)
{
    ULONGLONG deadline = KeQueryInterruptTime() + 10000000;
    while (KeQueryInterruptTime() < deadline) {
        if (boot ? c->diag.booted != 0 : c->response_ready) return STATUS_SUCCESS;
        if (input_pending(c)) {
            NTSTATUS status = read_packet(c);
            if (!NT_SUCCESS(status)) return status;
        } else sleep_ms(1);
    }
    return STATUS_IO_TIMEOUT;
}
static NTSTATUS query(JI_CONTEXT *c, UCHAR report, UCHAR iface, USHORT reply)
{
    c->request_pending = TRUE;
    NTSTATUS status = send_command(c, 0xd0, 0x20, report, iface, reply, NULL, 0);
    if (NT_SUCCESS(status)) status = wait_response(c, FALSE);
    c->request_pending = FALSE;
    return status;
}

static NTSTATUS create_hid(JI_CONTEXT *c, UCHAR *descriptor, USHORT size,
                           BOOLEAN keyboard, VHFHANDLE *hid)
{
    VHF_CONFIG config;
    static WCHAR keyboard_instance[] = L"J316Keyboard";
    static WCHAR trackpad_instance[] = L"J316Trackpad";
    VHF_CONFIG_INIT(&config, WdfDeviceWdmGetDeviceObject(c->device), size, descriptor);
    config.VendorID = 0x05ac; config.ProductID = (USHORT)c->diag.product;
    config.VersionNumber = 1; config.VhfClientContext = c;
    config.InstanceID = keyboard ? keyboard_instance : trackpad_instance;
    config.InstanceIDLength = sizeof(keyboard_instance) - sizeof(WCHAR);
    if (keyboard) config.EvtVhfAsyncOperationWriteReport = JiOutput;
    NTSTATUS status = VhfCreate(&config, hid);
    if (NT_SUCCESS(status)) status = VhfStart(*hid);
    if (!NT_SUCCESS(status) && *hid) { VhfDelete(*hid, TRUE); *hid = NULL; }
    return status;
}

VOID JiOutput(PVOID context, VHFOPERATIONHANDLE operation, PVOID operation_context,
               PHID_XFER_PACKET packet)
{
    UNREFERENCED_PARAMETER(operation_context);
    JI_CONTEXT *c = context;
    NTSTATUS status = STATUS_INVALID_PARAMETER;
    if (packet && packet->reportBuffer && packet->reportBufferLen == 2 &&
        packet->reportId == 1 && packet->reportBuffer[0] == 1) {
        WdfSpinLockAcquire(c->output_lock);
        if (InterlockedCompareExchange(&c->active, 1, 1)) {
            memcpy(c->output, packet->reportBuffer, 2);
            c->output_size = 2; c->output_pending = TRUE; status = STATUS_SUCCESS;
        } else status = STATUS_DEVICE_NOT_READY;
        WdfSpinLockRelease(c->output_lock);
    }
    VhfAsyncOperationComplete(operation, status);
}

NTSTATUS JiPowerOn(WDFDEVICE device, WDF_POWER_DEVICE_STATE previous)
{
    UNREFERENCED_PARAMETER(previous);
    JI_CONTEXT *c = JiContext(device);
    NTSTATUS status;
    c->diag.phase = 1; c->diag.booted = 0;
    RtlZeroMemory(&c->assembly, sizeof(c->assembly));
    RtlZeroMemory(&c->mouse, sizeof(c->mouse));
    /* Linux's divider 3 with a 24 MHz source stays below the 8 MHz limit.
     * Preserve inherited shifter controls, as spi-apple.c does. */
    wr(c->spi, 0x0c, 2); wr(c->spi, 0, 12); wr(c->spi, 4, 0x20);
    wr(c->spi, 0x130, 0); wr(c->spi, 0x138, 0);
    modify(c->spi, 0x150, (1u << 24) | (0x3fu << 16), 8u << 16);
    modify(c->spi, 0x154, 1u << 9, 2);
    wr(c->spi, 0x30, 3); wr(c->spi, 0x160, 0); wr(c->spi, 0x168, 0);
    /* Sample the input without enabling GPIO interrupt generation. */
    modify(c->nub, 6 * 4, 0x7006f, (7u << 1) | (1u << 9));
    modify(c->ap, 194 * 4, 15, 3); sleep_ms(5);
    modify(c->ap, 194 * 4, 15, 2); sleep_ms(5);
    modify(c->ap, 194 * 4, 15, 3); sleep_ms(50);
    c->diag.phase = 2;
    status = wait_response(c, TRUE); if (!NT_SUCCESS(status)) goto fail;
    c->diag.phase = 3;
    status = query(c, 1, 0xd0, 0); if (!NT_SUCCESS(status)) goto fail;
    if (c->response_size < 24 || c->response[4] < 3 ||
        c->response[6] != 0xac || c->response[7] != 5) {
        status = STATUS_DEVICE_PROTOCOL_ERROR; goto fail;
    }
    c->diag.vendor = c->response[6] | ((ULONG)c->response[7] << 8);
    c->diag.product = c->response[8] | ((ULONG)c->response[9] << 8);
    for (UCHAR iface = 0; iface < 3; ++iface) {
        c->diag.phase = 4 + iface;
        status = query(c, 2, iface, JI_DESCRIPTOR); if (!NT_SUCCESS(status)) goto fail;
        if (c->response_size < 16) { status = STATUS_DEVICE_PROTOCOL_ERROR; goto fail; }
    }
    c->diag.phase = 7;
    status = query(c, 0x10, 1, JI_DESCRIPTOR); if (!NT_SUCCESS(status)) goto fail;
    if (!c->response_size) { status = STATUS_DEVICE_PROTOCOL_ERROR; goto fail; }
    if (c->keyboard_hid && (c->keyboard_descriptor_size != c->response_size ||
        memcmp(c->keyboard_descriptor, c->response, c->response_size))) {
        status = STATUS_DEVICE_PROTOCOL_ERROR; goto fail;
    }
    memcpy(c->keyboard_descriptor, c->response, c->response_size);
    c->keyboard_descriptor_size = c->response_size;
    c->diag.descriptor_size = c->response_size;
    c->diag.phase = 8;
    status = query(c, 0x10, 2, JI_DESCRIPTOR); if (!NT_SUCCESS(status)) goto fail;
    c->diag.phase = 9;
    if (!c->keyboard_hid) {
        status = create_hid(c, c->keyboard_descriptor, (USHORT)c->keyboard_descriptor_size,
                            TRUE, &c->keyboard_hid);
        if (!NT_SUCCESS(status)) goto fail;
    }
    if (!c->mouse_hid) {
        status = create_hid(c, (UCHAR *)ji_mouse_descriptor, (USHORT)ji_mouse_descriptor_size,
                            FALSE, &c->mouse_hid);
        if (!NT_SUCCESS(status)) goto fail;
    }
    c->mt_pending = TRUE;
    InterlockedExchange(&c->active, 1);
    c->diag.phase = 10; c->diag.status = STATUS_SUCCESS;
    WdfTimerStart(c->timer, WDF_REL_TIMEOUT_IN_MS(4));
    return STATUS_SUCCESS;
fail:
    c->diag.status = status;
    modify(c->ap, 194 * 4, 15, 2); wr(c->spi, 0, 12);
    return status;
}

VOID JiPoll(WDFTIMER timer)
{
    JI_CONTEXT *c = JiContext(WdfTimerGetParentObject(timer));
    if (!InterlockedCompareExchange(&c->active, 1, 1)) return;
    NTSTATUS status = STATUS_SUCCESS;
    for (unsigned i = 0; i < 8 && input_pending(c); ++i) {
        status = read_packet(c); if (!NT_SUCCESS(status)) break;
    }
    if (NT_SUCCESS(status) && c->mt_pending) {
        const UCHAR mt[] = {2, 1};
        status = send_command(c, 2, 0x52, 2, 0, 2, mt, sizeof(mt));
        if (NT_SUCCESS(status)) c->mt_pending = FALSE;
    }
    UCHAR output[64]; ULONG size = 0;
    WdfSpinLockAcquire(c->output_lock);
    if (NT_SUCCESS(status) && c->output_pending) {
        size = c->output_size; memcpy(output, c->output, size); c->output_pending = FALSE;
    }
    WdfSpinLockRelease(c->output_lock);
    if (size) status = send_command(c, 1, 0x51, 1, 0, 0, output, size);
    c->diag.status = status;
    c->diag.spi_control = rd(c->spi, 0); c->diag.spi_fifo = rd(c->spi, 0x10c);
    c->diag.gpio_input = rd(c->nub, 6 * 4);
    if (InterlockedCompareExchange(&c->active, 1, 1))
        WdfTimerStart(c->timer, WDF_REL_TIMEOUT_IN_MS(NT_SUCCESS(status) ? 4 : 100));
}

NTSTATUS JiPowerOff(WDFDEVICE device, WDF_POWER_DEVICE_STATE target)
{
    UNREFERENCED_PARAMETER(target);
    JI_CONTEXT *c = JiContext(device);
    InterlockedExchange(&c->active, 0); WdfTimerStop(c->timer, TRUE);
    UCHAR keys[10] = {1}, mouse[8] = {1};
    submit(c, c->keyboard_hid, keys, sizeof(keys));
    submit(c, c->mouse_hid, mouse, sizeof(mouse));
    WdfSpinLockAcquire(c->output_lock); c->output_pending = FALSE;
    WdfSpinLockRelease(c->output_lock);
    modify(c->ap, 194 * 4, 15, 2); wr(c->spi, 0, 12);
    return STATUS_SUCCESS;
}

NTSTATUS JiPrepare(WDFDEVICE device, WDFCMRESLIST raw, WDFCMRESLIST translated)
{
    UNREFERENCED_PARAMETER(raw);
    JI_CONTEXT *c = JiContext(device);
    const ULONGLONG bases[] = {0x39b10c000, 0x39b028000, 0x2921f0000};
    PHYSICAL_ADDRESS addresses[3]; ULONG found = 0, interrupts = 0;
    RtlZeroMemory(addresses, sizeof(addresses));
    /* Validate the entire resource set before the first mapping or write. */
    for (ULONG i = 0; i < WdfCmResourceListGetCount(translated); ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r = WdfCmResourceListGetDescriptor(translated, i);
        if (r->Type == CmResourceTypeMemory) {
            unsigned j;
            for (j = 0; j < 3; ++j) if ((ULONGLONG)r->u.Memory.Start.QuadPart == bases[j]) break;
            if (j == 3 || (found & (1u << j)) || r->u.Memory.Length != 0x4000)
                return STATUS_DEVICE_CONFIGURATION_ERROR;
            found |= 1u << j; addresses[j] = r->u.Memory.Start;
        } else if (r->Type == CmResourceTypeInterrupt) ++interrupts;
    }
    if (found != 7 || interrupts != 1) return STATUS_DEVICE_CONFIGURATION_ERROR;
    c->spi = MmMapIoSpaceEx(addresses[0], 0x4000, PAGE_READWRITE | PAGE_NOCACHE);
    c->ap = MmMapIoSpaceEx(addresses[1], 0x4000, PAGE_READWRITE | PAGE_NOCACHE);
    c->nub = MmMapIoSpaceEx(addresses[2], 0x4000, PAGE_READWRITE | PAGE_NOCACHE);
    if (!c->spi || !c->ap || !c->nub) {
        if (c->spi) MmUnmapIoSpace(c->spi, 0x4000);
        if (c->ap) MmUnmapIoSpace(c->ap, 0x4000);
        if (c->nub) MmUnmapIoSpace(c->nub, 0x4000);
        c->spi = c->ap = c->nub = NULL; return STATUS_INSUFFICIENT_RESOURCES;
    }
    return STATUS_SUCCESS;
}

NTSTATUS JiRelease(WDFDEVICE device, WDFCMRESLIST resources)
{
    UNREFERENCED_PARAMETER(resources);
    JI_CONTEXT *c = JiContext(device);
    InterlockedExchange(&c->active, 0); WdfTimerStop(c->timer, TRUE);
    if (c->keyboard_hid) { VhfDelete(c->keyboard_hid, TRUE); c->keyboard_hid = NULL; }
    if (c->mouse_hid) { VhfDelete(c->mouse_hid, TRUE); c->mouse_hid = NULL; }
    if (c->spi) MmUnmapIoSpace(c->spi, 0x4000);
    if (c->ap) MmUnmapIoSpace(c->ap, 0x4000);
    if (c->nub) MmUnmapIoSpace(c->nub, 0x4000);
    c->spi = c->ap = c->nub = NULL;
    return STATUS_SUCCESS;
}

VOID JiIoctl(WDFQUEUE queue, WDFREQUEST request, size_t outsize, size_t insize, ULONG code)
{
    UNREFERENCED_PARAMETER(insize); UNREFERENCED_PARAMETER(outsize);
    JI_CONTEXT *c = JiContext(WdfIoQueueGetDevice(queue));
    JI_DIAG *out; NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    if (code == JI_SNAPSHOT) {
        status = WdfRequestRetrieveOutputBuffer(request, sizeof(*out), (PVOID *)&out, NULL);
        if (NT_SUCCESS(status)) {
            *out = c->diag; WdfRequestCompleteWithInformation(request, status, sizeof(*out)); return;
        }
    }
    WdfRequestComplete(request, status);
}

NTSTATUS JiAdd(WDFDRIVER driver, PWDFDEVICE_INIT init)
{
    UNREFERENCED_PARAMETER(driver);
    WDF_PNPPOWER_EVENT_CALLBACKS power; WDF_OBJECT_ATTRIBUTES attributes;
    WDFDEVICE device; WDF_TIMER_CONFIG timer; WDF_IO_QUEUE_CONFIG queue;
    DECLARE_CONST_UNICODE_STRING(link, L"\\DosDevices\\J316SpiInput");
    WdfDeviceInitSetDeviceType(init, FILE_DEVICE_UNKNOWN);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&power);
    power.EvtDevicePrepareHardware = JiPrepare; power.EvtDeviceReleaseHardware = JiRelease;
    power.EvtDeviceD0Entry = JiPowerOn; power.EvtDeviceD0Exit = JiPowerOff;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &power);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, JI_CONTEXT);
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    NTSTATUS status = WdfDeviceCreate(&init, &attributes, &device);
    if (!NT_SUCCESS(status)) return status;
    JI_CONTEXT *c = JiContext(device); c->device = device; c->diag.version = 1;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes); attributes.ParentObject = device;
    status = WdfSpinLockCreate(&attributes, &c->output_lock);
    if (!NT_SUCCESS(status)) return status;
    WDF_TIMER_CONFIG_INIT(&timer, JiPoll); timer.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes); attributes.ParentObject = device;
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfTimerCreate(&timer, &attributes, &c->timer);
    if (!NT_SUCCESS(status)) return status;
    status = WdfDeviceCreateSymbolicLink(device, &link);
    if (!NT_SUCCESS(status)) return status;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue, WdfIoQueueDispatchSequential);
    queue.EvtIoDeviceControl = JiIoctl;
    return WdfIoQueueCreate(device, &queue, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT object, PUNICODE_STRING path)
{
    WDF_DRIVER_CONFIG config; WDF_DRIVER_CONFIG_INIT(&config, JiAdd);
    return WdfDriverCreate(object, path, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
