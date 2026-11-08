// SPDX-License-Identifier: MIT
// J316s native Wi-Fi: WiFiCx (control path) + NetAdapterCx (data path) client for the
// m1n1-owned BCM4387 exposed through the J316 bridge window (ACPI\APPL0010).
//
// Ownership: m1n1 (EL2) owns PCIe, DART, DMA, firmware boot and the msgbuf rings. This
// driver owns the WLAN state machine above the firmware command interface, exactly the
// layer brcmfmac's cfg80211 code owns on Linux: escan, join, key installation, events.
// Windows runs the WPA2 supplicant (WDI has no 4-way-handshake offload), so the firmware
// supplicant is disabled (sup_wpa 0), EAPOL travels on the data path and the pairwise /
// group keys arrive through WDI_SET_ADD_CIPHER_KEYS.
//
// Supported: open networks and WPA2-Personal (RSNA-PSK) with CCMP pairwise and a CCMP or
// TKIP group cipher (mixed WPA/WPA2 routers) on 2.4 and 5 GHz. Not supported: TKIP
// pairwise, WEP, WPA3-SAE, 802.1X, PMF, roaming offload, Wi-Fi Direct, wake.
//
// References: bridge.h (ABI), m1n1 wifi_bcm.c (validated firmware command layouts),
// Microsoft WiFiCx documentation and the WiFiCx/NetAdapterCx samples (control and data
// path contracts). No sample code is copied.

#include <ntddk.h>
#include <wdf.h>
#include <netadaptercx.h>
#include <net/virtualaddress.h>
#include <wificx.h>
#include <TlvGeneratorParser.hpp>
#include <ntstrsafe.h>

extern "C" {
#include "../j316-wifi/bridge.h"
}

extern "C" DRIVER_INITIALIZE DriverEntry;

#define JX_TAG 'xWJJ'
#define JX_VERSION "0.6.0.0"

// ---------------------------------------------------------------------------------------
// Allocation glue required by the WiFiCx TLV generator/parser library.
// ---------------------------------------------------------------------------------------
struct JX_ALLOC_HEADER { SIZE_T Size; SIZE_T Pad; };

static void *JxAlloc(size_t size)
{
    if (size > (SIZE_T)-1 - sizeof(JX_ALLOC_HEADER)) return nullptr;
    auto *h = static_cast<JX_ALLOC_HEADER *>(
        ExAllocatePool2(POOL_FLAG_NON_PAGED, size + sizeof(JX_ALLOC_HEADER), JX_TAG));
    if (!h) return nullptr;
    h->Size = size;
    return h + 1;
}
static void JxFree(void *p)
{
    if (p) ExFreePoolWithTag(static_cast<JX_ALLOC_HEADER *>(p) - 1, JX_TAG);
}
void *__cdecl operator new(size_t size) { return JxAlloc(size); }
void *__cdecl operator new[](size_t size) { return JxAlloc(size); }
// The TLV library passes TLV_CONTEXT::AllocationContext; 0 means "allocate from the heap".
void *__cdecl operator new(size_t size, ULONG_PTR context) noexcept
{
    return context ? nullptr : JxAlloc(size);
}
void *__cdecl operator new[](size_t size, ULONG_PTR context) noexcept
{
    return context ? nullptr : JxAlloc(size);
}
void __cdecl operator delete(void *p) noexcept { JxFree(p); }
void __cdecl operator delete[](void *p) noexcept { JxFree(p); }
void __cdecl operator delete(void *p, size_t) noexcept { JxFree(p); }
void __cdecl operator delete[](void *p, size_t) noexcept { JxFree(p); }
void __cdecl operator delete(void *p, ULONG_PTR, int) noexcept { JxFree(p); }

// ---------------------------------------------------------------------------------------
// Broadcom firmware interface (brcmfmac fwil_types.h layouts, as used by m1n1 wifi_bcm.c).
// ---------------------------------------------------------------------------------------
enum : ULONG {
    BCM_C_UP = 2, BCM_C_SET_INFRA = 20, BCM_C_SET_AUTH = 22, BCM_C_GET_BSSID = 23,
    BCM_C_SET_SSID = 26, BCM_C_DISASSOC = 52, BCM_C_GET_RSSI = 127,
    BCM_C_GET_VAR = 262, BCM_C_SET_VAR = 263,
};
enum : ULONG {
    BCM_E_SET_SSID = 0, BCM_E_AUTH = 3, BCM_E_DEAUTH = 5, BCM_E_DEAUTH_IND = 6,
    BCM_E_ASSOC = 7, BCM_E_DISASSOC = 11, BCM_E_DISASSOC_IND = 12, BCM_E_LINK = 16,
    BCM_E_MIC_ERROR = 17, BCM_E_PSK_SUP = 46, BCM_E_ESCAN_RESULT = 69,
};
#define BCM_ESCAN_PARTIAL 8u
#define BCM_EVENT_HDR 72u      // Ethernet(14) + bcmeth(10) + brcmf_event_msg_be(48)
#define BCM_BSS_FIXED 128u     // brcmf_bss_info_le fixed part
#define BCM_WPA2_AUTH_PSK 0x80u
#define BCM_WSEC_TKIP 2u
#define BCM_WSEC_AES 4u
#define BCM_CRYPTO_TKIP 2u
#define BCM_CRYPTO_AES_CCM 4u
#define BCM_PRIMARY_KEY 2u
#define BCM_EVENT_GROUP 4u    // brcmf_event_msg flags: MIC error on a group-addressed frame

static inline USHORT Le16(const UCHAR *p) { return (USHORT)(p[0] | (p[1] << 8)); }
static inline ULONG Le32(const UCHAR *p) { return Le16(p) | ((ULONG)Le16(p + 2) << 16); }
static inline ULONG Be32(const UCHAR *p) { return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) | ((ULONG)p[2] << 8) | p[3]; }
static inline USHORT Be16(const UCHAR *p) { return (USHORT)((p[0] << 8) | p[1]); }
static inline void Put16(UCHAR *p, ULONG v) { p[0] = (UCHAR)v; p[1] = (UCHAR)(v >> 8); }
static inline void Put32(UCHAR *p, ULONG v) { Put16(p, v); Put16(p + 2, v >> 16); }

// ---------------------------------------------------------------------------------------
// Device state
// ---------------------------------------------------------------------------------------
#define JX_BSS_MAX 48u
#define JX_IE_MAX 1024u
#define JX_CMD_MAX 32u
#define JX_PEER_DEMUX_RANGE 1

struct JX_BSS {
    UCHAR Bssid[6];
    UCHAR Channel;
    USHORT Chanspec;
    LONG Rssi;
    USHORT Capability, BeaconPeriod;
    ULONG IeLength;
    UCHAR Ie[JX_IE_MAX];
};

struct JX_CMD {
    WIFIREQUEST Request;
    UINT16 MessageId;
};

enum JX_CONNECT_STATE { JxIdle, JxJoining, JxConnected };

struct JX_DEVICE {
    // NetAdapterCx requires the first context field to point to WDF triage data.
    void *WdfTriageInfoPtr;
    WDFDEVICE Device;
    TLV_CONTEXT Tlv;
    PUCHAR Regs;
    SIZE_T RegsLength;
    UCHAR Mac[6];
    NETADAPTER Adapter;
    WDI_PORT_ID PortId;
    NETPACKETQUEUE TxQueue, RxQueue;
    NET_EXTENSION TxVa, RxVa;
    volatile LONG TxNotify, RxNotify;
    KSPIN_LOCK TxLock, RxLock;
    ULONG TxProducer, RxConsumer;   // guest-owned bridge counters (native copies)
    WDFTIMER DataTimer;
    // Control worker
    PKTHREAD Worker;
    KEVENT WorkEvent;
    volatile LONG Stop;
    KSPIN_LOCK CmdLock;
    JX_CMD Cmds[JX_CMD_MAX];
    ULONG CmdHead, CmdCount;
    ULONG CtlSeq, EvConsumer;
    bool Native, FwFault, ScanV2, RadioOn;
    // Scan task
    bool Scanning;
    WDI_MESSAGE_HEADER ScanHeader;
    LARGE_INTEGER ScanDeadline;
    USHORT ScanSync;
    // Connect task
    JX_CONNECT_STATE Conn;
    WDI_MESSAGE_HEADER ConnectHeader;
    LARGE_INTEGER ConnectDeadline;
    UCHAR Peer[6];
    UCHAR Ssid[32];
    ULONG SsidLength;
    bool Secure, JoinPinned, JoinRetried;
    bool PeerAdded;
    UCHAR PeerAddr[6];
    // Group cipher of the current association (TKIP on mixed WPA/WPA2 routers) and the
    // installed group key, kept so a TKIP MIC-order mismatch can be corrected once.
    bool GroupTkip, MicSwapped, HaveGroupKey;
    UCHAR GroupKey[164];
    UCHAR RsnIe[24];
    ULONG RsnIeLength;
    // BSS cache (last scans)
    JX_BSS Bss[JX_BSS_MAX];
    ULONG BssCount, BssNext;
    // Scratch
    UCHAR Ctl[JW_CTL_MAX];
    UCHAR Event[JW_EV_SLOT];
    // Data-path diagnostics: EAPOL frames noted at DISPATCH, logged by the worker.
    struct JX_FRAMELOG { UCHAR Dir; UCHAR Type; USHORT Len; USHORT KeyInfo; };
    JX_FRAMELOG FrameLog[64];
    volatile LONG FrameLogHead;
    ULONG FrameLogTail;
    volatile LONG TxFrames, RxFrames, TxRingFull;
    ULONG LastTxFrames, LastRxFrames, LastTxRingFull;
    LONGLONG LastStats;
    UCHAR Frame[2048];      // worker thread only
    UCHAR TxFrame[2048];    // under TxLock
    UCHAR RxFrame[2048];    // under RxLock
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(JX_DEVICE, JxGetDevice);

struct JX_ADAPTER { JX_DEVICE *Dev; };
static JX_DEVICE *g_Dev;   // the one J316 device; queue callbacks must never dereference a null context

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(JX_ADAPTER, JxGetAdapter);
struct JX_QUEUE { JX_DEVICE *Dev; NET_EXTENSION Va; bool IsTx; };
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(JX_QUEUE, JxGetQueue);

// ---------------------------------------------------------------------------------------
// Bridge window access (uncached device mapping; slots are 8-byte aligned)
// ---------------------------------------------------------------------------------------
static ULONG Rd(JX_DEVICE *d, ULONG off) { return READ_REGISTER_ULONG((PULONG)(d->Regs + off)); }
static void Wr(JX_DEVICE *d, ULONG off, ULONG v) { WRITE_REGISTER_ULONG((PULONG)(d->Regs + off), v); }

static void WinRead(JX_DEVICE *d, ULONG off, void *out, ULONG length)
{
    ULONG64 tmp[64];
    PUCHAR o = static_cast<PUCHAR>(out);
    while (length) {
        ULONG chunk = length > sizeof(tmp) ? (ULONG)sizeof(tmp) : length;
        ULONG words = (chunk + 7) / 8;
        READ_REGISTER_BUFFER_ULONG64((PULONG64)(d->Regs + off), tmp, words);
        RtlCopyMemory(o, tmp, chunk);
        o += chunk; off += words * 8; length -= chunk;
    }
}
static void WinWrite(JX_DEVICE *d, ULONG off, const void *in, ULONG length)
{
    ULONG64 tmp[64];
    const UCHAR *i = static_cast<const UCHAR *>(in);
    while (length) {
        ULONG chunk = length > sizeof(tmp) ? (ULONG)sizeof(tmp) : length;
        ULONG words = (chunk + 7) / 8;
        RtlZeroMemory(tmp, words * 8);
        RtlCopyMemory(tmp, i, chunk);
        WRITE_REGISTER_BUFFER_ULONG64((PULONG64)(d->Regs + off), tmp, words);
        i += chunk; off += words * 8; length -= chunk;
    }
}

extern "C" NTSYSAPI NTSTATUS NTAPI ZwFlushBuffersFile(_In_ HANDLE FileHandle, _Out_ PIO_STATUS_BLOCK IoStatusBlock);

// Diagnostic log: \SystemRoot\Temp\J316WifiCx.log, appended from PASSIVE_LEVEL only.
// Never logs key material or passphrases.
static void JxLog(const char *format, ...)
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return;
    char line[512];
    LARGE_INTEGER now;
    KeQuerySystemTimePrecise(&now);
    TIME_FIELDS t;
    RtlTimeToTimeFields(&now, &t);
    size_t used = 0;
    if (!NT_SUCCESS(RtlStringCbPrintfA(line, sizeof(line), "%02u:%02u:%02u.%03u ", t.Hour, t.Minute, t.Second, t.Milliseconds)))
        return;
    (void)RtlStringCbLengthA(line, sizeof(line), &used);
    va_list args;
    va_start(args, format);
    (void)RtlStringCbVPrintfA(line + used, sizeof(line) - used - 2, format, args);
    va_end(args);
    (void)RtlStringCbCatA(line, sizeof(line), "\r\n");
    (void)RtlStringCbLengthA(line, sizeof(line), &used);
    UNICODE_STRING path = RTL_CONSTANT_STRING(L"\\SystemRoot\\Temp\\J316WifiCx.log");
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, nullptr, nullptr);
    HANDLE file;
    IO_STATUS_BLOCK io;
    if (!NT_SUCCESS(ZwCreateFile(&file, FILE_APPEND_DATA | SYNCHRONIZE, &oa, &io, nullptr, FILE_ATTRIBUTE_NORMAL,
                                 FILE_SHARE_READ, FILE_OPEN_IF, FILE_SYNCHRONOUS_IO_NONALERT, nullptr, 0)))
        return;
    (void)ZwWriteFile(file, nullptr, nullptr, nullptr, &io, line, (ULONG)used, nullptr, nullptr);
    (void)ZwFlushBuffersFile(file, &io);
    ZwClose(file);
}

static void Sleep(ULONG ms)
{
    LARGE_INTEGER t;
    t.QuadPart = -(LONGLONG)ms * 10000;
    KeDelayExecutionThread(KernelMode, FALSE, &t);
}
static LONGLONG Now() { return KeQueryInterruptTime(); }

// ---------------------------------------------------------------------------------------
// Firmware mailbox (worker thread only; one command in flight)
// ---------------------------------------------------------------------------------------
static void HandleEvents(JX_DEVICE *d);

static NTSTATUS FwIoctl(JX_DEVICE *d, ULONG cmd, void *buf, ULONG length, ULONG *outLength, LONG *fwStatus)
{
    if (d->FwFault) return STATUS_DEVICE_HARDWARE_ERROR;
    if (length > JW_CTL_MAX) return STATUS_INVALID_PARAMETER;
    WinWrite(d, JW_CTL_BUF, buf, length);
    Wr(d, JW_REG_CTL_CMD, cmd);
    Wr(d, JW_REG_CTL_LEN, length);
    KeMemoryBarrier();
    ULONG seq = ++d->CtlSeq;
    Wr(d, JW_REG_CTL_SEQ, seq);
    LONGLONG deadline = Now() + 3LL * 10000000;
    while (Rd(d, JW_REG_CTL_DONE) != seq) {
        if (Now() > deadline || d->Stop) {
            d->FwFault = true;
            JxLog("fw cmd %lu timed out (host mailbox not answering); driver now faulted", cmd);
            return STATUS_IO_TIMEOUT;
        }
        Sleep(1);
    }
    KeMemoryBarrier();
    LONG status = (LONG)Rd(d, JW_REG_CTL_STATUS);
    ULONG n = Rd(d, JW_REG_CTL_RESULT_LEN);
    if (status) {
        char name[24] = {0};
        if (cmd == BCM_C_GET_VAR || cmd == BCM_C_SET_VAR)
            for (ULONG i = 0; i < sizeof(name) - 1 && i < length && static_cast<UCHAR *>(buf)[i]; ++i)
                name[i] = static_cast<char *>(buf)[i];
        JxLog("fw cmd %lu %s len %lu -> status %ld", cmd, name, length, status);
    }
    if (n > length) n = length;
    if (n) WinRead(d, JW_CTL_BUF, buf, n);
    if (outLength) *outLength = n;
    if (fwStatus) *fwStatus = status;
    return status ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
}
static NTSTATUS FwVar(JX_DEVICE *d, const char *name, const void *data, ULONG n, bool get, ULONG capacity = 0)
{
    ULONG key = (ULONG)strlen(name) + 1;
    ULONG total = key + (n > capacity ? n : capacity);
    if (total > JW_CTL_MAX) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(d->Ctl, total);
    RtlCopyMemory(d->Ctl, name, key);
    if (data && n) RtlCopyMemory(d->Ctl + key, data, n);
    return FwIoctl(d, get ? BCM_C_GET_VAR : BCM_C_SET_VAR, d->Ctl, total, nullptr, nullptr);
}
static NTSTATUS FwSetInt(JX_DEVICE *d, const char *name, ULONG value)
{
    UCHAR v[4];
    Put32(v, value);
    return FwVar(d, name, v, 4, false);
}
static NTSTATUS FwCmdInt(JX_DEVICE *d, ULONG cmd, ULONG value)
{
    Put32(d->Ctl, value);
    return FwIoctl(d, cmd, d->Ctl, 4, nullptr, nullptr);
}

// ---------------------------------------------------------------------------------------
// WDI indications
// ---------------------------------------------------------------------------------------
static void Indicate(JX_DEVICE *d, UINT16 messageId, UINT32 transaction, NDIS_STATUS status,
                     const UCHAR *tlv, ULONG tlvLength)
{
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = d->Device;
    WDFMEMORY memory;
    PVOID buffer;
    SIZE_T size = sizeof(WDI_MESSAGE_HEADER) + tlvLength;
    if (!NT_SUCCESS(WdfMemoryCreate(&attributes, NonPagedPoolNx, JX_TAG, size, &memory, &buffer)))
        return;
    RtlZeroMemory(buffer, size);
    auto *h = static_cast<WDI_MESSAGE_HEADER *>(buffer);
    h->PortId = d->PortId;
    h->Status = status;
    h->TransactionId = transaction;
    if (tlvLength) RtlCopyMemory(h + 1, tlv, tlvLength);
    WifiDeviceReceiveIndication(d->Device, messageId, memory);
    WdfObjectDelete(memory);
}
static void IndicateGenerated(JX_DEVICE *d, UINT16 messageId, NDIS_STATUS gen, UINT8 *out, ULONG length)
{
    if (gen != NDIS_STATUS_SUCCESS || !out) JxLog("TLV generate for indication %u failed 0x%08lx", messageId, gen);
    if (gen == NDIS_STATUS_SUCCESS && out) Indicate(d, messageId, WDI_TRANSACTION_ID_UNSOLICIT, NDIS_STATUS_SUCCESS, out, length);
    if (out) FreeGenerated(out);
}
static void CompleteTask(JX_DEVICE *d, UINT16 completeId, const WDI_MESSAGE_HEADER &h, NDIS_STATUS status)
{
    JxLog("M4 %u status 0x%08lx", completeId, status);
    Indicate(d, completeId, h.TransactionId, status, nullptr, 0);
}

static WDI_BAND_ID BandOf(UCHAR channel) { return channel <= 14 ? WDI_BAND_ID_2400 : WDI_BAND_ID_5000; }
static ULONG LinkQuality(LONG rssi)
{
    if (rssi >= -50) return 100;
    if (rssi <= -100) return 0;
    return (ULONG)(2 * (rssi + 100));
}

static JX_BSS *FindBss(JX_DEVICE *d, const UCHAR *bssid)
{
    for (ULONG i = 0; i < d->BssCount; ++i)
        if (RtlEqualMemory(d->Bss[i].Bssid, bssid, 6)) return &d->Bss[i];
    return nullptr;
}

// Beacon body as WDI expects it: timestamp(8) | interval(2) | capability(2) | IEs.
static ULONG BeaconBody(const JX_BSS *b, UCHAR *out, ULONG capacity)
{
    if (capacity < 12 + b->IeLength) return 0;
    RtlZeroMemory(out, 8);
    Put16(out + 8, b->BeaconPeriod);
    Put16(out + 10, b->Capability);
    RtlCopyMemory(out + 12, b->Ie, b->IeLength);
    return 12 + b->IeLength;
}

static void IndicateBss(JX_DEVICE *d, const JX_BSS *b)
{
    WDI_BSS_ENTRY_CONTAINER entry;
    RtlCopyMemory(entry.BSSID.Address, b->Bssid, 6);
    ULONG n = BeaconBody(b, d->Frame, sizeof(d->Frame));
    if (!n) return;
    entry.Optional.BeaconFrame_IsPresent = TRUE;
    entry.BeaconFrame.SimpleAssign(d->Frame, n);
    entry.SignalInfo.RSSI = b->Rssi;
    entry.SignalInfo.LinkQuality = LinkQuality(b->Rssi);
    entry.ChannelInfo.ChannelNumber = b->Channel;
    entry.ChannelInfo.BandId = BandOf(b->Channel);
    WDI_INDICATION_BSS_ENTRY_LIST_PARAMETERS list;
    list.Optional.DeviceDescriptor_IsPresent = TRUE;
    list.DeviceDescriptor.SimpleAssign(&entry, 1);
    UINT8 *out = nullptr;
    ULONG length = 0;
    NDIS_STATUS s = GenerateWdiIndicationBssEntryList(&list, 0, &d->Tlv, &length, &out);
    list.DeviceDescriptor.SimpleAssign(nullptr, 0);
    entry.BeaconFrame.SimpleAssign(nullptr, 0);
    IndicateGenerated(d, WDI_INDICATION_BSS_ENTRY_LIST, s, out, length);
}

// ---------------------------------------------------------------------------------------
// Scan
// ---------------------------------------------------------------------------------------
static NTSTATUS StartEscan(JX_DEVICE *d, const WDI_SCAN_PARAMETERS *p)
{
    UCHAR prm[8 + 72 + 4 * 36];
    RtlZeroMemory(prm, sizeof(prm));
    ULONG nssid = 0;
    if (p) {
        for (ULONG i = 0; i < p->SSIDList.ElementCount && nssid < 4; ++i)
            if (p->SSIDList.pElements[i].ElementCount && p->SSIDList.pElements[i].ElementCount <= 32) nssid++;
    }
    UCHAR *q = prm + 8;
    Put32(prm, d->ScanV2 ? 2 : 1);
    Put16(prm + 4, 1);                 // WL_ESCAN_ACTION_START
    Put16(prm + 6, ++d->ScanSync);
    ULONG ssidOff, size;
    if (d->ScanV2) {
        Put16(q, 2);
        Put16(q + 2, 72 + 36);
        RtlFillMemory(q + 40, 6, 0xff);
        q[46] = 2;                     // DOT11_BSSTYPE_ANY
        Put32(q + 48, 0);              // active scan
        for (ULONG i = 0; i < 4; ++i) Put32(q + 52 + 4 * i, 0xffffffffu);
        Put32(q + 68, nssid << 16);    // all channels
        ssidOff = 72;
    } else {
        RtlFillMemory(q + 36, 6, 0xff);
        q[42] = 2;
        for (ULONG i = 0; i < 4; ++i) Put32(q + 44 + 4 * i, 0xffffffffu);
        Put32(q + 60, nssid << 16);
        ssidOff = 64;
    }
    ULONG k = 0;
    if (p) {
        for (ULONG i = 0; i < p->SSIDList.ElementCount && k < nssid; ++i) {
            const auto &s = p->SSIDList.pElements[i];
            if (!s.ElementCount || s.ElementCount > 32) continue;
            Put32(q + ssidOff + 36 * k, s.ElementCount);
            RtlCopyMemory(q + ssidOff + 36 * k + 4, s.pElements, s.ElementCount);
            k++;
        }
    }
    size = 8 + ssidOff + 36 * (nssid ? nssid : 1);
    return FwVar(d, "escan", prm, size, false);
}
static void AbortEscan(JX_DEVICE *d)
{
    UCHAR prm[8 + 72 + 36];
    RtlZeroMemory(prm, sizeof(prm));
    Put32(prm, d->ScanV2 ? 2 : 1);
    Put16(prm + 4, 3);                 // WL_ESCAN_ACTION_ABORT
    Put16(prm + 6, d->ScanSync);
    if (d->ScanV2) Put16(prm + 8, 2), Put16(prm + 10, 72 + 36);
    (void)FwVar(d, "escan", prm, sizeof(prm), false);
}
static void FinishScan(JX_DEVICE *d, NDIS_STATUS status)
{
    if (!d->Scanning) return;
    d->Scanning = false;
    CompleteTask(d, WDI_INDICATION_SCAN_COMPLETE, d->ScanHeader, status);
}

static void OnEscanResult(JX_DEVICE *d, ULONG status, const UCHAR *data, ULONG n)
{
    if (status != BCM_ESCAN_PARTIAL) {
        FinishScan(d, NDIS_STATUS_SUCCESS);
        return;
    }
    if (n < 12 + BCM_BSS_FIXED) return;
    const UCHAR *bi = data + 12;
    ULONG length = Le32(bi + 4);
    if (length < BCM_BSS_FIXED || length > n - 12) return;
    ULONG ieOff = Le16(bi + 116), ieLength = Le32(bi + 120);
    if (ieOff > length || ieLength > length - ieOff || ieLength > JX_IE_MAX) return;
    JX_BSS *b = FindBss(d, bi + 8);
    if (!b) {
        if (d->BssCount < JX_BSS_MAX) b = &d->Bss[d->BssCount++];
        else { b = &d->Bss[d->BssNext]; d->BssNext = (d->BssNext + 1) % JX_BSS_MAX; }
    }
    RtlCopyMemory(b->Bssid, bi + 8, 6);
    b->BeaconPeriod = Le16(bi + 14);
    b->Capability = Le16(bi + 16);
    b->Chanspec = Le16(bi + 72);
    b->Rssi = (SHORT)Le16(bi + 78);
    b->Channel = bi[88];
    b->IeLength = ieLength;
    RtlCopyMemory(b->Ie, bi + ieOff, ieLength);
    if (d->Scanning) IndicateBss(d, b);
}

// ---------------------------------------------------------------------------------------
// Connect / disconnect
// ---------------------------------------------------------------------------------------
static void IndicateLinkState(JX_DEVICE *d, const JX_BSS *b)
{
    WDI_INDICATION_LINK_STATE_CHANGE_PARAMETERS p;
    WDI_LINK_INFO_CONTAINER info;
    RtlZeroMemory(&info, sizeof(info));
    LONG rssi = b ? b->Rssi : -60;
    RtlCopyMemory(p.LinkStateChangeParameters.PeerMACAddress.Address, d->Peer, 6);
    p.LinkStateChangeParameters.TxLinkSpeed = 300000;   // kbps, nominal
    p.LinkStateChangeParameters.RxLinkSpeed = 300000;
    p.LinkStateChangeParameters.LinkQuality = (UINT8)LinkQuality(rssi);
    RtlCopyMemory(info.LocalLinkMACAddress.Address, d->Mac, 6);
    RtlCopyMemory(info.PeerLinkMACAddress.Address, d->Peer, 6);
    info.ChannelNumber = b ? b->Channel : 0;
    info.BandId = b ? BandOf(b->Channel) : WDI_BAND_ID_2400;
    info.RSSI = rssi;
    info.Bandwidth = 20;
    p.LinkInfo.SimpleAssign(&info, 1);
    UINT8 *out = nullptr;
    ULONG length = 0;
    NDIS_STATUS s = GenerateWdiIndicationLinkStateChange(&p, 0, &d->Tlv, &length, &out);
    p.LinkInfo.SimpleAssign(nullptr, 0);
    IndicateGenerated(d, WDI_INDICATION_LINK_STATE_CHANGE, s, out, length);
}

static void IndicateAssociation(JX_DEVICE *d, WDI_ASSOC_STATUS status, const UCHAR *bssid)
{
    WDI_ASSOCIATION_RESULT_CONTAINER r;
    RtlCopyMemory(r.BSSID.Address, bssid, 6);
    auto &a = r.AssociationResultParameters;
    a.AssociationStatus = status;
    a.AuthAlgorithm = d->Secure ? WDI_AUTH_ALGO_RSNA_PSK : WDI_AUTH_ALGO_80211_OPEN;
    a.UnicastCipherAlgorithm = d->Secure ? WDI_CIPHER_ALGO_CCMP : WDI_CIPHER_ALGO_NONE;
    a.MulticastDataCipherAlgorithm = !d->Secure ? WDI_CIPHER_ALGO_NONE
                                   : d->GroupTkip ? WDI_CIPHER_ALGO_TKIP : WDI_CIPHER_ALGO_CCMP;
    a.MulticastMgmtCipherAlgorithm = WDI_CIPHER_ALGO_NONE;
    JX_BSS *b = FindBss(d, bssid);
    a.BandID = b ? BandOf(b->Channel) : WDI_BAND_ID_2400;
    // Mandatory TLV (no _IsPresent flag): without it the generator rejects the message.
    WDI_PHY_TYPE phy = a.BandID == WDI_BAND_ID_5000 ? WDI_PHY_TYPE_VHT : WDI_PHY_TYPE_HT;
    r.ActivePhyTypeList.SimpleAssign(&phy, 1);

    // Association request/response bodies: fixed fields + the IEs the firmware exchanged.
    static UCHAR req[1024], resp[1024], beacon[JX_IE_MAX + 12];
    ULONG reqLength = 0, respLength = 0, beaconLength = 0;
    if (status == WDI_ASSOC_STATUS_SUCCESS) {
        UCHAR info[8];
        // brcmf_cfg80211_assoc_ielen_le {req_len, resp_len, ...}; brcmfmac reads it with
        // a WL_ASSOC_INFO_MAX (512) buffer, as do the IE queries below.
        if (NT_SUCCESS(FwVar(d, "assoc_info", nullptr, 0, true, 512))) {
            RtlCopyMemory(info, d->Ctl, sizeof(info));
            ULONG reqIe = Le32(info), respIe = Le32(info + 4);
            Put16(req, b ? (b->Capability & 0x0431) | 0x0001 : 0x0401);
            Put16(req + 2, 10);
            reqLength = 4;
            if (reqIe && reqIe <= sizeof(req) - 4 &&
                NT_SUCCESS(FwVar(d, "assoc_req_ies", nullptr, 0, true, sizeof(req)))) {
                RtlCopyMemory(req + 4, d->Ctl, reqIe);
                reqLength += reqIe;
            } else if (d->RsnIeLength) {
                // Fall back to the RSN element we configured (the supplicant checks it in M2/M3).
                RtlCopyMemory(req + 4, d->RsnIe, d->RsnIeLength);
                reqLength += d->RsnIeLength;
            }
            Put16(resp, b ? b->Capability : 0x0401);
            Put16(resp + 2, 0);
            Put16(resp + 4, 0xc001);
            respLength = 6;
            if (respIe && respIe <= sizeof(resp) - 6 &&
                NT_SUCCESS(FwVar(d, "assoc_resp_ies", nullptr, 0, true, sizeof(resp)))) {
                RtlCopyMemory(resp + 6, d->Ctl, respIe);
                respLength += respIe;
            }
        }
        if (reqLength) { r.Optional.AssociationRequestFrame_IsPresent = TRUE; r.AssociationRequestFrame.SimpleAssign(req, reqLength); }
        if (respLength) { r.Optional.AssociationResponseFrame_IsPresent = TRUE; r.AssociationResponseFrame.SimpleAssign(resp, respLength); }
        if (b && (beaconLength = BeaconBody(b, beacon, sizeof(beacon))) != 0) {
            r.Optional.BeaconProbeResponse_IsPresent = TRUE;
            r.BeaconProbeResponse.SimpleAssign(beacon, beaconLength);
        }
    }
    WDI_INDICATION_ASSOCIATION_RESULT_LIST list;
    list.AssociationResults.SimpleAssign(&r, 1);
    UINT8 *out = nullptr;
    ULONG length = 0;
    NDIS_STATUS s = GenerateWdiIndicationAssociationResult(&list, 0, &d->Tlv, &length, &out);
    list.AssociationResults.SimpleAssign(nullptr, 0);
    r.AssociationRequestFrame.SimpleAssign(nullptr, 0);
    r.AssociationResponseFrame.SimpleAssign(nullptr, 0);
    r.BeaconProbeResponse.SimpleAssign(nullptr, 0);
    r.ActivePhyTypeList.SimpleAssign(nullptr, 0);
    JxLog("association result status %u req %lu resp %lu beacon %lu", status, reqLength, respLength, beaconLength);
    IndicateGenerated(d, WDI_INDICATION_ASSOCIATION_RESULT, s, out, length);
}

// NetAdapterCx media state. WiFiCx does not set it from WDI indications: while it stays
// disconnected the OS never transmits on the adapter, so EAPOL 2/4 of the handshake is never
// sent (log: AP retransmits 1/4, "tx 0", then WLAN-AutoConfig 11006 "Dynamic key exchange did
// not succeed"). Connected after association, as a native 802.11 miniport indicates it; the
// 802.1X port stays closed until the keys are installed.
static void SetLink(JX_DEVICE *d, bool up)
{
    if (!d->Adapter) return;
    NET_ADAPTER_LINK_STATE s;
    NET_ADAPTER_LINK_STATE_INIT(&s, up ? 300000000ULL : NET_ADAPTER_LINK_SPEED_UNKNOWN,
                                up ? MediaConnectStateConnected : MediaConnectStateDisconnected,
                                MediaDuplexStateHalf, NetAdapterPauseFunctionTypeUnsupported,
                                NetAdapterAutoNegotiationFlagNone);
    NetAdapterSetLinkState(d->Adapter, &s);
    JxLog("link %s", up ? "up" : "down");
}

// WiFiCx tracks the associated AP as a peer: without WifiAdapterAddPeer the OS never hands
// frames for it to the TX queue (no EAPOL reply, no data), so the handshake cannot start.
static void PeerSync(JX_DEVICE *d, bool add)
{
    if (add) {
        if (d->PeerAdded) return;
        NET_EUI48_ADDRESS a;
        RtlCopyMemory(a.Value, d->Peer, 6);
        RtlCopyMemory(d->PeerAddr, d->Peer, 6);
        WifiAdapterAddPeer(d->Adapter, &a);
        d->PeerAdded = true;
        JxLog("peer added %02x:%02x:%02x:%02x:%02x:%02x", a.Value[0], a.Value[1], a.Value[2], a.Value[3], a.Value[4], a.Value[5]);
    } else if (d->PeerAdded) {
        NET_EUI48_ADDRESS a;
        RtlCopyMemory(a.Value, d->PeerAddr, 6);
        WifiAdapterRemovePeer(d->Adapter, &a);
        d->PeerAdded = false;
        JxLog("peer removed");
        SetLink(d, false);
    }
    d->HaveGroupKey = false;
    RtlSecureZeroMemory(d->GroupKey, sizeof(d->GroupKey));
}

static void IndicateDisassociation(JX_DEVICE *d, WDI_ASSOC_STATUS reason)
{
    PeerSync(d, false);
    WDI_INDICATION_DISASSOCIATION_PARAMETERS p;
    RtlCopyMemory(p.DisconnectIndicationParameters.MacAddress.Address, d->Peer, 6);
    p.DisconnectIndicationParameters.DisassociationWABIReason = reason;
    UINT8 *out = nullptr;
    ULONG length = 0;
    NDIS_STATUS s = GenerateWdiIndicationDisassociation(&p, 0, &d->Tlv, &length, &out);
    IndicateGenerated(d, WDI_INDICATION_DISASSOCIATION, s, out, length);
}

static void FinishConnect(JX_DEVICE *d, bool ok)
{
    if (d->Conn != JxJoining) return;
    if (ok) {
        d->Conn = JxConnected;
        PeerSync(d, true);
        IndicateAssociation(d, WDI_ASSOC_STATUS_SUCCESS, d->Peer);
        SetLink(d, true);
        IndicateLinkState(d, FindBss(d, d->Peer));
        CompleteTask(d, WDI_INDICATION_CONNECT_COMPLETE, d->ConnectHeader, NDIS_STATUS_SUCCESS);
    } else {
        d->Conn = JxIdle;
        PeerSync(d, false);
        IndicateAssociation(d, WDI_ASSOC_STATUS_FAILURE, d->Peer);
        (void)FwIoctl(d, BCM_C_DISASSOC, d->Ctl, 0, nullptr, nullptr);
        CompleteTask(d, WDI_INDICATION_CONNECT_COMPLETE, d->ConnectHeader, NDIS_STATUS_FAILURE);
    }
}

static void Disassociate(JX_DEVICE *d, bool indicate, WDI_ASSOC_STATUS reason)
{
    JX_CONNECT_STATE old = d->Conn;
    d->Conn = JxIdle;
    PeerSync(d, false);
    (void)FwIoctl(d, BCM_C_DISASSOC, d->Ctl, 0, nullptr, nullptr);
    if (old == JxJoining)
        CompleteTask(d, WDI_INDICATION_CONNECT_COMPLETE, d->ConnectHeader, NDIS_STATUS_REQUEST_ABORTED);
    if (old == JxConnected && indicate) IndicateDisassociation(d, reason);
}

// brcmf_ext_join_params_le, pinned to the requested BSSID (and chanspec when scanned).
// pin = false joins by SSID + BSSID without a chanspec (used as the retry when a pinned join
// is refused instantly).
static NTSTATUS IssueJoin(JX_DEVICE *d, bool allowPin)
{
    JX_BSS *b = FindBss(d, d->Peer);
    bool pin = allowPin && b && b->Chanspec;
    d->JoinPinned = pin;
    UCHAR j[72];
    RtlZeroMemory(j, sizeof(j));
    Put32(j, d->SsidLength);
    RtlCopyMemory(j + 4, d->Ssid, d->SsidLength);
    j[36] = 0xff;
    Put32(j + 40, pin ? 320 / 20 : 0xffffffffu);
    Put32(j + 44, pin ? 320 : 0xffffffffu);
    Put32(j + 48, pin ? 400 : 0xffffffffu);
    Put32(j + 52, 0xffffffffu);
    RtlCopyMemory(j + 56, d->Peer, 6);
    if (pin) { Put32(j + 64, 1); Put16(j + 68, b->Chanspec); }
    d->Conn = JxJoining;
    d->ConnectDeadline.QuadPart = Now() + 15LL * 10000000;
    NTSTATUS st = FwVar(d, "join", j, pin ? 70 : 68, false);
    if (!NT_SUCCESS(st)) {
        UCHAR s[52];
        RtlZeroMemory(s, sizeof(s));
        Put32(s, d->SsidLength);
        RtlCopyMemory(s + 4, d->Ssid, d->SsidLength);
        RtlCopyMemory(s + 36, d->Peer, 6);
        if (pin) { Put32(s + 44, 1); Put16(s + 48, b->Chanspec); }
        RtlCopyMemory(d->Ctl, s, sizeof(s));
        st = FwIoctl(d, BCM_C_SET_SSID, d->Ctl, pin ? 52 : 36, nullptr, nullptr);
    }
    if (!NT_SUCCESS(st)) { d->Conn = JxIdle; JxLog("connect: join command failed"); return STATUS_UNSUCCESSFUL; }
    JxLog("connect: join issued to %02x:%02x:%02x:%02x:%02x:%02x chanspec 0x%04x secure %u%s",
          d->Peer[0], d->Peer[1], d->Peer[2], d->Peer[3], d->Peer[4], d->Peer[5], pin ? b->Chanspec : 0, d->Secure,
          d->JoinRetried ? " (retry, unpinned)" : "");
    return STATUS_SUCCESS;
}

// Group cipher suite type (00-0F-AC:n) from the BSS's RSN element; 0 when absent.
static UCHAR BssGroupCipher(const JX_BSS *b)
{
    if (!b) return 0;
    for (ULONG o = 0; o + 2 <= b->IeLength; o += 2 + b->Ie[o + 1]) {
        if (o + 2 + b->Ie[o + 1] > b->IeLength) break;
        if (b->Ie[o] == 48 && b->Ie[o + 1] >= 6 && b->Ie[o + 4] == 0x00 && b->Ie[o + 5] == 0x0f &&
            b->Ie[o + 6] == 0xac)
            return b->Ie[o + 7];
    }
    return 0;
}

// Returns an M3 status. On success the join is in flight and the M4 follows from events.
static NTSTATUS StartConnect(JX_DEVICE *d, const WDI_TASK_CONNECT_PARAMETERS &p)
{
    const auto &c = p.ConnectParameters;
    if (!c.SSIDList.ElementCount || !c.SSIDList.pElements[0].ElementCount ||
        c.SSIDList.pElements[0].ElementCount > 32 || !c.AuthenticationAlgorithms.ElementCount ||
        !p.PreferredBSSEntryList.ElementCount)
        return STATUS_INVALID_PARAMETER;
    WDI_AUTH_ALGORITHM auth = c.AuthenticationAlgorithms.pElements[0];
    JxLog("connect: auth %u (of %lu) ucast %u (of %lu) mcast %u (of %lu) bss %lu",
          auth, c.AuthenticationAlgorithms.ElementCount,
          c.UnicastCipherAlgorithms.ElementCount ? c.UnicastCipherAlgorithms.pElements[0] : 0, c.UnicastCipherAlgorithms.ElementCount,
          c.MulticastCipherAlgorithms.ElementCount ? c.MulticastCipherAlgorithms.pElements[0] : 0, c.MulticastCipherAlgorithms.ElementCount,
          p.PreferredBSSEntryList.ElementCount);
    bool secure;
    if (auth == WDI_AUTH_ALGO_80211_OPEN) secure = false;
    else if (auth == WDI_AUTH_ALGO_RSNA_PSK) secure = true;
    else return STATUS_NOT_SUPPORTED;
    // The OS offers cipher candidate lists (e.g. CCMP + TKIP for multicast). Pairwise must be
    // CCMP; the group cipher follows the AP's RSN element (TKIP on mixed WPA/WPA2 routers,
    // which the firmware refuses to join with a CCMP-only group: SET_SSID status 1).
    bool mcastTkip = false;
    if (secure) {
        bool ucastCcmp = false, mcastCcmp = false;
        for (ULONG i = 0; i < c.UnicastCipherAlgorithms.ElementCount; ++i) {
            JxLog("connect: ucast[%lu]=%u", i, c.UnicastCipherAlgorithms.pElements[i]);
            ucastCcmp |= c.UnicastCipherAlgorithms.pElements[i] == WDI_CIPHER_ALGO_CCMP;
        }
        for (ULONG i = 0; i < c.MulticastCipherAlgorithms.ElementCount; ++i) {
            JxLog("connect: mcast[%lu]=%u", i, c.MulticastCipherAlgorithms.pElements[i]);
            mcastCcmp |= c.MulticastCipherAlgorithms.pElements[i] == WDI_CIPHER_ALGO_CCMP;
            mcastTkip |= c.MulticastCipherAlgorithms.pElements[i] == WDI_CIPHER_ALGO_TKIP;
        }
        if (!ucastCcmp || (!mcastCcmp && !mcastTkip)) return STATUS_NOT_SUPPORTED;
    }
    if (!d->RadioOn) return STATUS_INVALID_DEVICE_STATE;

    if (d->Conn != JxIdle) Disassociate(d, true, WDI_ASSOC_STATUS_DISASSOCIATED_BY_HOST);
    if (d->Scanning) { AbortEscan(d); FinishScan(d, NDIS_STATUS_REQUEST_ABORTED); }

    const auto &ssid = c.SSIDList.pElements[0];
    d->SsidLength = ssid.ElementCount;
    RtlCopyMemory(d->Ssid, ssid.pElements, ssid.ElementCount);
    RtlCopyMemory(d->Peer, p.PreferredBSSEntryList.pElements[0].BSSID.Address, 6);
    d->Secure = secure;
    d->GroupTkip = secure && mcastTkip && BssGroupCipher(FindBss(d, d->Peer)) == 2;
    d->MicSwapped = false;
    d->HaveGroupKey = false;
    d->RsnIeLength = 0;
    if (secure) {
        // RSN element v1: group CCMP (or TKIP), pairwise CCMP, AKM PSK, capabilities 0 (no PMF).
        static const UCHAR rsn[] = {48, 20, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4,
                                    1, 0, 0x00, 0x0f, 0xac, 2, 0, 0};
        RtlCopyMemory(d->RsnIe, rsn, sizeof(rsn));
        if (d->GroupTkip) d->RsnIe[7] = 2;
        d->RsnIeLength = sizeof(rsn);
    }
    JxLog("connect: group cipher %s", !secure ? "none" : d->GroupTkip ? "TKIP" : "CCMP");

    // brcmf_cfg80211_connect order (host supplicant: sup_wpa 0).
    (void)FwVar(d, "wpaie", d->RsnIeLength ? d->RsnIe : nullptr, d->RsnIeLength, false);
    NTSTATUS st = FwSetInt(d, "wpa_auth", secure ? BCM_WPA2_AUTH_PSK : 0);
    if (NT_SUCCESS(st)) st = FwCmdInt(d, BCM_C_SET_AUTH, 0);
    if (NT_SUCCESS(st)) st = FwSetInt(d, "wsec", !secure ? 0 : d->GroupTkip ? BCM_WSEC_TKIP | BCM_WSEC_AES : BCM_WSEC_AES);
    if (NT_SUCCESS(st)) (void)FwSetInt(d, "mfp", 0);
    if (NT_SUCCESS(st)) st = FwSetInt(d, "sup_wpa", 0);
    if (!NT_SUCCESS(st)) { JxLog("connect: security setup failed 0x%08lx", st); return STATUS_UNSUCCESSFUL; }

    // Diagnostics: RSN capabilities of the chosen BSS (bit 6 MFPR = PMF required, bit 7 MFPC).
    if (JX_BSS *rb = FindBss(d, d->Peer)) {
        bool found = false;
        for (ULONG o = 0; o + 2 <= rb->IeLength; o += 2 + rb->Ie[o + 1]) {
            if (o + 2 + rb->Ie[o + 1] > rb->IeLength) break;
            if (rb->Ie[o] != 48) continue;
            ULONG n = rb->Ie[o + 1];
            const UCHAR *r = rb->Ie + o + 2;
            ULONG off = 2 + 4;                                   // version, group cipher
            if (n >= off + 2) off += 2 + 4 * (r[off] | (r[off + 1] << 8));   // pairwise suites
            ULONG akmCount = n >= off + 2 ? (r[off] | (r[off + 1] << 8)) : 0;
            ULONG akmOff = off + 2;
            off = akmOff + 4 * akmCount;
            USHORT caps = n >= off + 2 ? (USHORT)(r[off] | (r[off + 1] << 8)) : 0;
            JxLog("connect: BSS RSN akm0 type %u (count %lu) caps 0x%04x chan %u", n >= akmOff + 4 ? r[akmOff + 3] : 0, akmCount, caps, rb->Channel);
            found = true;
            break;
        }
        if (!found) JxLog("connect: BSS has no RSN IE (ie len %lu)", rb->IeLength);
    }
    d->JoinRetried = false;
    return IssueJoin(d, true);
}

// Wait until the host has taken every queued TX frame (EAPOL 4/4 must leave unencrypted
// before the pairwise key is installed; brcmfmac waits for pending 802.1X frames likewise).
static void DrainTx(JX_DEVICE *d)
{
    for (ULONG i = 0; i < 100; ++i) {
        if (Rd(d, JW_REG_TX_CONS) == d->TxProducer) break;
        Sleep(1);
    }
    Sleep(20);
}

static NTSTATUS AddKeys(JX_DEVICE *d, const WDI_SET_ADD_CIPHER_KEYS_PARAMETERS &p)
{
    for (ULONG i = 0; i < p.SetCipherKey.ElementCount; ++i) {
        const auto &k = p.SetCipherKey.pElements[i];
        bool pairwise = k.CipherKeyTypeInfo.KeyType == WDI_CIPHER_KEY_TYPE_PAIRWISE_KEY;
        bool ccmp = k.Optional.CCMPKey_IsPresent && k.CCMPKey.ElementCount == 16;
        bool tkip = !pairwise && k.Optional.TKIPInfo_IsPresent && k.TKIPInfo.TKIPKey.ElementCount == 16 &&
                    k.TKIPInfo.TKIPMIC.ElementCount == 16;
        if (!ccmp && !tkip) {
            // Not a CCMP-128 key or TKIP group key (e.g. BIP/IGTK): PMF is not negotiated, so it
            // is not needed. Ignore instead of failing the whole handshake.
            JxLog("add key: ignoring cipher %u type %u", k.CipherKeyTypeInfo.CipherAlgorithm, k.CipherKeyTypeInfo.KeyType);
            continue;
        }
        if (!pairwise && k.CipherKeyTypeInfo.KeyType != WDI_CIPHER_KEY_TYPE_GROUP_KEY) return STATUS_NOT_SUPPORTED;
        if (pairwise) DrainTx(d);
        UCHAR key[164];                 // brcmf_wsec_key_le
        RtlZeroMemory(key, sizeof(key));
        Put32(key, k.Optional.CipherKeyID_IsPresent ? k.CipherKeyID.CipherKeyID : 0);
        if (tkip) {
            // brcmf_wsec_key_le data: TK | station TX MIC | station RX MIC. MicSwapped flips the
            // two MIC halves after a group MIC failure (see OnEvent).
            Put32(key + 4, 32);
            RtlCopyMemory(key + 8, k.TKIPInfo.TKIPKey.pElements, 16);
            RtlCopyMemory(key + 24 + (d->MicSwapped ? 8 : 0), k.TKIPInfo.TKIPMIC.pElements, 8);
            RtlCopyMemory(key + 24 + (d->MicSwapped ? 0 : 8), k.TKIPInfo.TKIPMIC.pElements + 8, 8);
            Put32(key + 112, BCM_CRYPTO_TKIP);
        } else {
            Put32(key + 4, 16);
            RtlCopyMemory(key + 8, k.CCMPKey.pElements, 16);
            Put32(key + 112, BCM_CRYPTO_AES_CCM);
        }
        // brcmfmac: pairwise keys go through add_keyext (peer address, no flags); group keys
        // through add_key (BRCMF_PRIMARY_KEY, zero address).
        if (pairwise)
            RtlCopyMemory(key + 156, k.Optional.PeerMacAddress_IsPresent ? k.PeerMacAddress.Address : d->Peer, 6);
        else
            Put32(key + 116, BCM_PRIMARY_KEY);
        if (tkip) { RtlCopyMemory(d->GroupKey, key, sizeof(key)); d->HaveGroupKey = true; }
        NTSTATUS st = FwVar(d, "wsec_key", key, sizeof(key), false);
        JxLog("add %s %s key id %lu -> 0x%08lx", pairwise ? "pairwise" : "group", tkip ? "TKIP" : "CCMP",
              k.Optional.CipherKeyID_IsPresent ? k.CipherKeyID.CipherKeyID : 0, st);
        RtlSecureZeroMemory(key, sizeof(key));
        RtlSecureZeroMemory(d->Ctl, sizeof(key) + 16);
        if (!NT_SUCCESS(st)) return st;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS DeleteKeys(JX_DEVICE *d, const WDI_SET_DELETE_CIPHER_KEYS_PARAMETERS &p)
{
    for (ULONG i = 0; i < p.CipherKeyInfo.ElementCount; ++i) {
        const auto &k = p.CipherKeyInfo.pElements[i];
        UCHAR key[164];
        RtlZeroMemory(key, sizeof(key));
        Put32(key, k.Optional.CipherKeyID_IsPresent ? k.CipherKeyID.CipherKeyID : 0);
        if (k.Optional.PeerMacAddress_IsPresent) {
            Put32(key + 116, BCM_PRIMARY_KEY);
            RtlCopyMemory(key + 156, k.PeerMacAddress.Address, 6);
        }
        (void)FwVar(d, "wsec_key", key, sizeof(key), false);   // len 0 removes the key
    }
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------------------
// Firmware events
// ---------------------------------------------------------------------------------------
static void OnEvent(JX_DEVICE *d, const UCHAR *p, ULONG n)
{
    if (n < BCM_EVENT_HDR || Be16(p + 12) != 0x886c) return;
    ULONG type = Be32(p + 28), status = Be32(p + 32), datalen = Be32(p + 44);
    USHORT flags = Be16(p + 26);
    if (datalen > n - BCM_EVENT_HDR) return;
    const UCHAR *data = p + BCM_EVENT_HDR;
    if (type != BCM_E_ESCAN_RESULT)
        JxLog("fw event %lu status %lu reason %lu flags 0x%x (conn %u)", type, status, Be32(p + 36), flags, d->Conn);
    switch (type) {
    case BCM_E_ESCAN_RESULT:
        if (d->Scanning) OnEscanResult(d, status, data, datalen);
        break;
    case BCM_E_SET_SSID:
        if (d->Conn == JxJoining && status != 0 && d->JoinPinned && !d->JoinRetried) {
            // Firmware refused the pinned join (stale scan chanspec, regulatory, ...): retry by
            // SSID + BSSID and let the firmware pick the channel.
            d->JoinRetried = true;
            if (NT_SUCCESS(IssueJoin(d, false))) break;
        }
        if (d->Conn == JxJoining) FinishConnect(d, status == 0);
        break;
    case BCM_E_LINK:
        if (flags & 1) {
            if (d->Conn == JxJoining) FinishConnect(d, true);
        } else if (d->Conn == JxConnected) {
            d->Conn = JxIdle;
            IndicateDisassociation(d, WDI_ASSOC_STATUS_ROAMING_ASSOCIATION_LOST);
        } else if (d->Conn == JxJoining) {
            FinishConnect(d, false);
        }
        break;
    case BCM_E_MIC_ERROR:
        // Group TKIP MIC failure right after keying: the MIC halves are in the other order.
        // Swap once and reinstall the stored group key.
        if ((flags & BCM_EVENT_GROUP) && d->Conn == JxConnected && d->HaveGroupKey && !d->MicSwapped) {
            d->MicSwapped = true;
            UCHAR mic[8];
            RtlCopyMemory(mic, d->GroupKey + 24, 8);
            RtlCopyMemory(d->GroupKey + 24, d->GroupKey + 32, 8);
            RtlCopyMemory(d->GroupKey + 32, mic, 8);
            RtlSecureZeroMemory(mic, sizeof(mic));
            NTSTATUS st = FwVar(d, "wsec_key", d->GroupKey, sizeof(d->GroupKey), false);
            RtlSecureZeroMemory(d->Ctl, sizeof(d->GroupKey) + 16);
            JxLog("group TKIP MIC failure: MIC halves swapped, key reinstalled -> 0x%08lx", st);
        }
        break;
    case BCM_E_DEAUTH:
    case BCM_E_DEAUTH_IND:
    case BCM_E_DISASSOC_IND:
        if (d->Conn == JxJoining) FinishConnect(d, false);
        else if (d->Conn == JxConnected) {
            d->Conn = JxIdle;
            IndicateDisassociation(d, type == BCM_E_DISASSOC_IND ? WDI_ASSOC_STATUS_PEER_DISASSOCIATED
                                                                 : WDI_ASSOC_STATUS_PEER_DEAUTHENTICATED);
        }
        break;
    default:
        break;
    }
}

static void HandleEvents(JX_DEVICE *d)
{
    for (ULONG budget = 0; budget < JW_EV_COUNT; ++budget) {
        ULONG prod = Rd(d, JW_REG_EV_PROD);
        if (prod == d->EvConsumer) break;
        if (prod - d->EvConsumer > JW_EV_COUNT) { d->EvConsumer = prod; Wr(d, JW_REG_EV_CONS, prod); break; }
        ULONG slot = JW_EV_BASE + (d->EvConsumer % JW_EV_COUNT) * JW_EV_SLOT;
        KeMemoryBarrier();
        ULONG length = Rd(d, slot);
        if (length >= BCM_EVENT_HDR && length <= JW_EV_SLOT - 4) {
            WinRead(d, slot, d->Event, length + 4);
            Wr(d, JW_REG_EV_CONS, ++d->EvConsumer);
            OnEvent(d, d->Event + 4, length);
        } else {
            Wr(d, JW_REG_EV_CONS, ++d->EvConsumer);
        }
    }
}

// ---------------------------------------------------------------------------------------
// WDI command dispatch (worker thread, PASSIVE_LEVEL)
// ---------------------------------------------------------------------------------------
template <typename T>
using JX_PARSER = NDIS_STATUS (__stdcall *)(ULONG, UINT8 const *, PCTLV_CONTEXT, T *);
template <typename T>
using JX_CLEANUP = void (__stdcall *)(T *);

template <typename T>
static NTSTATUS ParseCmd(JX_DEVICE *d, void *buffer, UINT inLength, JX_PARSER<T> parse, T *out)
{
    if (inLength < sizeof(WDI_MESSAGE_HEADER)) return STATUS_INVALID_PARAMETER;
    NDIS_STATUS s = parse(inLength - sizeof(WDI_MESSAGE_HEADER),
                          static_cast<UINT8 *>(buffer) + sizeof(WDI_MESSAGE_HEADER), &d->Tlv, out);
    return s == NDIS_STATUS_SUCCESS ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}

static UINT16 g_LastId;
static void M3(WIFIREQUEST r, NTSTATUS s)
{
    JxLog("cmd %u -> M3 0x%08lx", g_LastId, s);
    WifiRequestComplete(r, s, sizeof(WDI_MESSAGE_HEADER));
}

static void RunCommand(JX_DEVICE *d, WIFIREQUEST r, UINT16 id)
{
    UINT inLength = 0, outLength = 0;
    void *buffer = WifiRequestGetInOutBuffer(r, &inLength, &outLength);
    if (!buffer || inLength < sizeof(WDI_MESSAGE_HEADER)) { M3(r, STATUS_INVALID_PARAMETER); return; }
    WDI_MESSAGE_HEADER h = *static_cast<WDI_MESSAGE_HEADER *>(buffer);
    NTSTATUS st;
    g_LastId = id;

    switch (id) {
    case WDI_TASK_SCAN: {
        WDI_SCAN_PARAMETERS p;
        st = ParseCmd(d, buffer, inLength, ParseWdiTaskScan, &p);
        if (NT_SUCCESS(st)) {
            if (d->Scanning) st = STATUS_DEVICE_BUSY;
            else {
                d->ScanHeader = h;
                d->Scanning = true;
                d->ScanDeadline.QuadPart = Now() + 15LL * 10000000;
                if (d->RadioOn && d->Conn != JxJoining) {
                    st = StartEscan(d, &p);
                    if (!NT_SUCCESS(st)) d->Scanning = false;
                }
            }
            CleanupParsedWdiTaskScan(&p);
        }
        M3(r, st);
        // Radio off or joining: report an empty scan.
        if (NT_SUCCESS(st) && (!d->RadioOn || d->Conn == JxJoining)) FinishScan(d, NDIS_STATUS_SUCCESS);
        return;
    }
    case WDI_TASK_CONNECT: {
        WDI_TASK_CONNECT_PARAMETERS p;
        st = ParseCmd(d, buffer, inLength, ParseWdiTaskConnect, &p);
        if (NT_SUCCESS(st)) {
            d->ConnectHeader = h;
            st = StartConnect(d, p);
            CleanupParsedWdiTaskConnect(&p);
        }
        M3(r, st);
        return;
    }
    case WDI_TASK_DISCONNECT: {
        M3(r, STATUS_SUCCESS);
        Disassociate(d, true, WDI_ASSOC_STATUS_DISASSOCIATED_BY_HOST);
        CompleteTask(d, WDI_INDICATION_DISCONNECT_COMPLETE, h, NDIS_STATUS_SUCCESS);
        return;
    }
    case WDI_TASK_DOT11_RESET: {
        M3(r, STATUS_SUCCESS);
        if (d->Scanning) { AbortEscan(d); FinishScan(d, NDIS_STATUS_REQUEST_ABORTED); }
        if (d->Conn != JxIdle) Disassociate(d, true, WDI_ASSOC_STATUS_DISASSOCIATED_BY_HOST);
        CompleteTask(d, WDI_INDICATION_DOT11_RESET_COMPLETE, h, NDIS_STATUS_SUCCESS);
        return;
    }
    case WDI_TASK_SET_RADIO_STATE: {
        WDI_SET_RADIO_STATE_PARAMETERS p;
        st = ParseCmd(d, buffer, inLength, ParseWdiTaskSetRadioState, &p);
        M3(r, st);
        if (!NT_SUCCESS(st)) return;
        bool on = p.SoftwareRadioState != 0;
        CleanupParsedWdiTaskSetRadioState(&p);
        if (!on) {
            if (d->Scanning) { AbortEscan(d); FinishScan(d, NDIS_STATUS_REQUEST_ABORTED); }
            if (d->Conn != JxIdle) Disassociate(d, true, WDI_ASSOC_STATUS_DISASSOCIATED_BY_HOST);
        }
        if (on != d->RadioOn) {
            d->RadioOn = on;
            WDI_INDICATION_RADIO_STATUS_PARAMETERS rs;
            rs.RadioState.HardwareState = TRUE;
            rs.RadioState.SoftwareState = on;
            UINT8 *out = nullptr;
            ULONG length = 0;
            NDIS_STATUS g = GenerateWdiIndicationRadioStatus(&rs, 0, &d->Tlv, &length, &out);
            IndicateGenerated(d, WDI_INDICATION_RADIO_STATUS, g, out, length);
        }
        CompleteTask(d, WDI_INDICATION_SET_RADIO_STATE_COMPLETE, h, NDIS_STATUS_SUCCESS);
        return;
    }
    case WDI_GET_STATISTICS: {
        // Windows polls this after every connect; answer with empty (all-unknown) statistics
        // instead of failing the request.
        WDI_GET_STATISTICS_PARAMETERS sp;
        WDI_MAC_STATISTICS_CONTAINER mac;
        WDI_PHY_STATISTICS_CONTAINER phy;
        RtlCopyMemory(mac.MACAddress.Address, d->PeerAdded ? d->PeerAddr : d->Mac, 6);
        mac.TransmittedFrameCount = (ULONG)d->TxFrames;
        mac.ReceivedFrameCount = (ULONG)d->RxFrames;
        phy.PhyType = WDI_PHY_TYPE_ANY;
        phy.TransmittedFrameCount = (ULONG)d->TxFrames;
        phy.ReceivedFrameCount = (ULONG)d->RxFrames;
        sp.PeerMACStatistics.SimpleAssign(&mac, 1);
        sp.PhyStatistics.SimpleAssign(&phy, 1);
        UINT8 *out = nullptr;
        ULONG length = 0;
        NDIS_STATUS g = GenerateWdiGetStatisticsFromIhv(&sp, 0, &d->Tlv, &length, &out);
        sp.PeerMACStatistics.SimpleAssign(nullptr, 0);
        sp.PhyStatistics.SimpleAssign(nullptr, 0);
        if (g == NDIS_STATUS_SUCCESS && out && sizeof(WDI_MESSAGE_HEADER) + length > outLength) {
            WifiRequestSetBytesNeeded(r, sizeof(WDI_MESSAGE_HEADER) + length);
            JxLog("cmd %u -> M3 buffer too short (need %lu, have %u)", g_LastId, (ULONG)(sizeof(WDI_MESSAGE_HEADER) + length), outLength);
            WifiRequestComplete(r, (NTSTATUS)NDIS_STATUS_BUFFER_TOO_SHORT, sizeof(WDI_MESSAGE_HEADER));
        } else if (g == NDIS_STATUS_SUCCESS && out) {
            RtlCopyMemory(static_cast<UINT8 *>(buffer) + sizeof(WDI_MESSAGE_HEADER), out, length);
            JxLog("cmd %u -> M3 0x00000000 (statistics, %lu bytes)", g_LastId, length);
            WifiRequestComplete(r, STATUS_SUCCESS, sizeof(WDI_MESSAGE_HEADER) + length);
        } else {
            JxLog("statistics: generate 0x%08lx length %lu out buffer %u", g, length, outLength);
            M3(r, STATUS_NOT_SUPPORTED);
        }
        if (out) FreeGenerated(out);
        return;
    }
    case WDI_ABORT_TASK:
        M3(r, STATUS_SUCCESS);
        if (d->Scanning) { AbortEscan(d); FinishScan(d, NDIS_STATUS_REQUEST_ABORTED); }
        else if (d->Conn == JxJoining) Disassociate(d, false, WDI_ASSOC_STATUS_DISASSOCIATED_BY_HOST);
        return;
    case WDI_SET_ADD_CIPHER_KEYS: {
        WDI_SET_ADD_CIPHER_KEYS_PARAMETERS p;
        st = ParseCmd(d, buffer, inLength, ParseWdiSetAddCipherKeys, &p);
        if (NT_SUCCESS(st)) { st = AddKeys(d, p); CleanupParsedWdiSetAddCipherKeys(&p); }
        M3(r, st);
        return;
    }
    case WDI_SET_DELETE_CIPHER_KEYS: {
        WDI_SET_DELETE_CIPHER_KEYS_PARAMETERS p;
        st = ParseCmd(d, buffer, inLength, ParseWdiSetDeleteCipherKeys, &p);
        if (NT_SUCCESS(st)) { st = DeleteKeys(d, p); CleanupParsedWdiSetDeleteCipherKeys(&p); }
        M3(r, st);
        return;
    }
    // Configuration the firmware either applies by default or this driver does not offload:
    // accept so the OS state machine proceeds (packet filtering is done by the OS stack).
    case WDI_SET_ADAPTER_CONFIGURATION:
    case WDI_SET_RECEIVE_PACKET_FILTER:
    case WDI_SET_MULTICAST_LIST:
    case WDI_SET_PRIVACY_EXEMPTION_LIST:
    case WDI_SET_CONNECTION_QUALITY:
    case WDI_SET_DEFAULT_KEY_ID:
    case WDI_SET_POWER_STATE:
    case WDI_SET_FLUSH_BSS_ENTRY:
    case WDI_SET_HOST_DETECT_ERROR:
    case WDI_SET_ENCAPSULATION_OFFLOAD:
    case WDI_SET_END_DWELL_TIME:
    case WDI_SET_NEIGHBOR_REPORT_ENTRIES:
    case WDI_SET_LOCATION_PRIVACY:
        M3(r, STATUS_SUCCESS);
        return;
    default:
        M3(r, STATUS_NOT_SUPPORTED);
        return;
    }
}

static void FlushFrameLog(JX_DEVICE *d);
static void Worker(PVOID context)
{
    auto *d = static_cast<JX_DEVICE *>(context);
    LARGE_INTEGER period;
    period.QuadPart = -5LL * 10000;   // 5 ms
    while (!d->Stop) {
        (void)KeWaitForSingleObject(&d->WorkEvent, Executive, KernelMode, FALSE, &period);
        if (d->Stop) break;
        HandleEvents(d);
        FlushFrameLog(d);
        for (;;) {
            JX_CMD c;
            KIRQL irql;
            KeAcquireSpinLock(&d->CmdLock, &irql);
            bool have = d->CmdCount != 0;
            if (have) {
                c = d->Cmds[d->CmdHead];
                d->CmdHead = (d->CmdHead + 1) % JX_CMD_MAX;
                d->CmdCount--;
            }
            KeReleaseSpinLock(&d->CmdLock, irql);
            if (!have) break;
            RunCommand(d, c.Request, c.MessageId);
            HandleEvents(d);
        }
        LONGLONG now = Now();
        if (d->Scanning && now > d->ScanDeadline.QuadPart) { AbortEscan(d); FinishScan(d, NDIS_STATUS_SUCCESS); }
        if (d->Conn == JxJoining && now > d->ConnectDeadline.QuadPart) FinishConnect(d, false);
        if (d->FwFault) {
            if (d->Scanning) FinishScan(d, NDIS_STATUS_FAILURE);
            if (d->Conn == JxJoining) FinishConnect(d, false);
        }
    }
    // Fail whatever is still queued.
    KIRQL irql;
    KeAcquireSpinLock(&d->CmdLock, &irql);
    while (d->CmdCount) {
        JX_CMD c = d->Cmds[d->CmdHead];
        d->CmdHead = (d->CmdHead + 1) % JX_CMD_MAX;
        d->CmdCount--;
        KeReleaseSpinLock(&d->CmdLock, irql);
        M3(c.Request, STATUS_DEVICE_NOT_READY);
        KeAcquireSpinLock(&d->CmdLock, &irql);
    }
    KeReleaseSpinLock(&d->CmdLock, irql);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static EVT_WIFI_DEVICE_SEND_COMMAND EvtSendCommand;
static void EvtSendCommand(WDFDEVICE device, WIFIREQUEST request)
{
    JX_DEVICE *d = JxGetDevice(device);
    UINT16 id = WifiRequestGetMessageId(request);
    KIRQL irql;
    KeAcquireSpinLock(&d->CmdLock, &irql);
    bool ok = d->Worker && !d->Stop && d->CmdCount < JX_CMD_MAX;
    if (ok) {
        d->Cmds[(d->CmdHead + d->CmdCount) % JX_CMD_MAX] = {request, id};
        d->CmdCount++;
    }
    KeReleaseSpinLock(&d->CmdLock, irql);
    if (!ok) { M3(request, STATUS_DEVICE_BUSY); return; }
    KeSetEvent(&d->WorkEvent, IO_NO_INCREMENT, FALSE);
}

// ---------------------------------------------------------------------------------------
// Data path (NetAdapterCx rings <-> bridge rings). TX copies synchronously; RX copies on
// Advance; a 1 ms high-resolution timer re-arms notifications.
// ---------------------------------------------------------------------------------------
// frame = Ethernet frame (no length prefix). dir 0 = TX, 1 = RX. Safe at DISPATCH_LEVEL.
static void NoteFrame(JX_DEVICE *d, UCHAR dir, const UCHAR *frame, ULONG length)
{
    InterlockedIncrement(dir ? &d->RxFrames : &d->TxFrames);
    if (length < 21 || frame[12] != 0x88 || frame[13] != 0x8e) return;
    LONG slot = InterlockedIncrement(&d->FrameLogHead) - 1;
    auto &e = d->FrameLog[(ULONG)slot % 64];
    e.Dir = dir;
    e.Type = frame[15];                                   // EAPOL packet type (3 = key)
    e.Len = (USHORT)length;
    e.KeyInfo = (USHORT)((frame[19] << 8) | frame[20]);   // EAPOL-Key key information
}

static void FlushFrameLog(JX_DEVICE *d)
{
    LONG head = d->FrameLogHead;
    if ((ULONG)head - d->FrameLogTail > 64) d->FrameLogTail = (ULONG)head - 64;
    while (d->FrameLogTail != (ULONG)head) {
        const auto &e = d->FrameLog[d->FrameLogTail % 64];
        JxLog("%s EAPOL type %u len %u keyinfo 0x%04x", e.Dir ? "rx" : "tx", e.Type, e.Len, e.KeyInfo);
        d->FrameLogTail++;
    }
    LONGLONG now = Now();
    if (now - d->LastStats > 5LL * 10000000) {
        d->LastStats = now;
        ULONG tx = (ULONG)d->TxFrames, rx = (ULONG)d->RxFrames, full = (ULONG)d->TxRingFull;
        if (tx != d->LastTxFrames || rx != d->LastRxFrames || full != d->LastTxRingFull)
            JxLog("data path: tx %lu rx %lu tx-ring-full %lu (conn %u)", tx, rx, full, d->Conn);
        d->LastTxFrames = tx; d->LastRxFrames = rx; d->LastTxRingFull = full;
    }
}

static EVT_PACKET_QUEUE_ADVANCE EvtTxAdvance;
static void EvtTxAdvance(NETPACKETQUEUE queue)
{
    JX_QUEUE *qc = JxGetQueue(queue);
    JX_DEVICE *d = qc->Dev ? qc->Dev : g_Dev;
    NET_RING_COLLECTION const *rings = NetTxQueueGetRingCollection(queue);
    NET_RING *pr = NetRingCollectionGetPacketRing(rings);
    NET_RING *fr = NetRingCollectionGetFragmentRing(rings);
    if (!d || !d->Regs || !d->Native) return;
    KIRQL irql;
    KeAcquireSpinLock(&d->TxLock, &irql);
    UINT32 pi = pr->BeginIndex;
    while (pi != pr->EndIndex) {
        NET_PACKET *packet = NetRingGetPacketAtIndex(pr, pi);
        if (!packet->Ignore) {
            ULONG consumer = Rd(d, JW_REG_TX_CONS);
            if (d->TxProducer - consumer >= JW_RING_COUNT) { InterlockedIncrement(&d->TxRingFull); break; }   // ring full: retry later
            ULONG length = 0;
            bool ok = true;
            UINT32 fi = packet->FragmentIndex;
            for (UINT16 n = 0; n < packet->FragmentCount; ++n, fi = NetRingIncrementIndex(fr, fi)) {
                NET_FRAGMENT *f = NetRingGetFragmentAtIndex(fr, fi);
                auto *va = NetExtensionGetFragmentVirtualAddress(&qc->Va, fi);
                if (length + f->ValidLength > JW_FRAME_MAX) { ok = false; break; }
                RtlCopyMemory(d->TxFrame + 4 + length, (PUCHAR)va->VirtualAddress + f->Offset, (SIZE_T)f->ValidLength);
                length += (ULONG)f->ValidLength;
            }
            if (ok && length >= 14) {
                NoteFrame(d, 0, d->TxFrame + 4, length);
                if (length < 60) { RtlZeroMemory(d->TxFrame + 4 + length, 60 - length); length = 60; }
                Put32(d->TxFrame, length);
                ULONG slot = JW_TX_BASE + (d->TxProducer % JW_RING_COUNT) * JW_SLOT_SIZE;
                WinWrite(d, slot, d->TxFrame, 4 + length);
                KeMemoryBarrier();
                Wr(d, JW_REG_N_TX_PROD, ++d->TxProducer);
            }
        }
        fr->BeginIndex = NetRingAdvanceIndex(fr, packet->FragmentIndex, packet->FragmentCount);
        pi = NetRingIncrementIndex(pr, pi);
    }
    pr->BeginIndex = pi;
    KeReleaseSpinLock(&d->TxLock, irql);
}

static EVT_PACKET_QUEUE_SET_NOTIFICATION_ENABLED EvtTxNotify;
static void EvtTxNotify(NETPACKETQUEUE queue, BOOLEAN enabled)
{
    { JX_DEVICE *nd = JxGetQueue(queue)->Dev ? JxGetQueue(queue)->Dev : g_Dev; if (nd) InterlockedExchange(&nd->TxNotify, enabled); }
}
static EVT_PACKET_QUEUE_CANCEL EvtTxCancel;
static void EvtTxCancel(NETPACKETQUEUE queue)
{
    NET_RING_COLLECTION const *rings = NetTxQueueGetRingCollection(queue);
    NET_RING *pr = NetRingCollectionGetPacketRing(rings);
    NET_RING *fr = NetRingCollectionGetFragmentRing(rings);
    UINT32 pi = pr->BeginIndex;
    while (pi != pr->EndIndex) {
        NET_PACKET *packet = NetRingGetPacketAtIndex(pr, pi);
        packet->Scratch = 1;
        fr->BeginIndex = NetRingAdvanceIndex(fr, packet->FragmentIndex, packet->FragmentCount);
        pi = NetRingIncrementIndex(pr, pi);
    }
    pr->BeginIndex = pi;
}

static EVT_PACKET_QUEUE_ADVANCE EvtRxAdvance;
static void EvtRxAdvance(NETPACKETQUEUE queue)
{
    JX_QUEUE *qc = JxGetQueue(queue);
    JX_DEVICE *d = qc->Dev ? qc->Dev : g_Dev;
    NET_RING_COLLECTION const *rings = NetRxQueueGetRingCollection(queue);
    NET_RING *pr = NetRingCollectionGetPacketRing(rings);
    NET_RING *fr = NetRingCollectionGetFragmentRing(rings);
    if (!d || !d->Regs || !d->Native) return;
    KIRQL irql;
    KeAcquireSpinLock(&d->RxLock, &irql);
    UINT32 pi = pr->BeginIndex, fi = fr->BeginIndex;
    ULONG producer = Rd(d, JW_REG_RX_PROD);
    if (producer - d->RxConsumer > JW_RING_COUNT) {          // host counter corruption
        d->RxConsumer = producer;
        Wr(d, JW_REG_N_RX_CONS, producer);
    }
    KeMemoryBarrier();
    while (pi != pr->EndIndex && fi != fr->EndIndex && d->RxConsumer != producer) {
        ULONG slot = JW_RX_BASE + (d->RxConsumer % JW_RING_COUNT) * JW_SLOT_SIZE;
        ULONG length = Rd(d, slot);
        NET_FRAGMENT *f = NetRingGetFragmentAtIndex(fr, fi);
        if (length < 14 || length > JW_FRAME_MAX || length > f->Capacity) {
            Wr(d, JW_REG_N_RX_CONS, ++d->RxConsumer);
            continue;
        }
        WinRead(d, slot, d->RxFrame, 4 + length);
        NoteFrame(d, 1, d->RxFrame + 4, length);
        Wr(d, JW_REG_N_RX_CONS, ++d->RxConsumer);
        auto *va = NetExtensionGetFragmentVirtualAddress(&qc->Va, fi);
        f->Offset = 0;
        RtlCopyMemory(va->VirtualAddress, d->RxFrame + 4, length);
        f->ValidLength = length;
        NET_PACKET *packet = NetRingGetPacketAtIndex(pr, pi);
        packet->FragmentIndex = fi;
        packet->FragmentCount = 1;
        packet->Layout = {};
        // The bridge delivers 802.3 frames: tell the stack so (an unspecified layer 2 type lets
        // the OS drop frames it cannot classify, which includes the EAPOL of the 4-way handshake).
        packet->Layout.Layer2Type = NetPacketLayer2TypeEthernet;
        packet->Layout.Layer2HeaderLength = 14;
        fi = NetRingIncrementIndex(fr, fi);
        pi = NetRingIncrementIndex(pr, pi);
    }
    fr->BeginIndex = fi;
    pr->BeginIndex = pi;
    KeReleaseSpinLock(&d->RxLock, irql);
}

static EVT_PACKET_QUEUE_SET_NOTIFICATION_ENABLED EvtRxNotify;
static void EvtRxNotify(NETPACKETQUEUE queue, BOOLEAN enabled)
{
    { JX_DEVICE *nd = JxGetQueue(queue)->Dev ? JxGetQueue(queue)->Dev : g_Dev; if (nd) InterlockedExchange(&nd->RxNotify, enabled); }
}
static EVT_PACKET_QUEUE_CANCEL EvtRxCancel;
static void EvtRxCancel(NETPACKETQUEUE queue)
{
    NET_RING_COLLECTION const *rings = NetRxQueueGetRingCollection(queue);
    NET_RING *pr = NetRingCollectionGetPacketRing(rings);
    NET_RING *fr = NetRingCollectionGetFragmentRing(rings);
    UINT32 pi = pr->BeginIndex;
    while (pi != pr->EndIndex) {
        NetRingGetPacketAtIndex(pr, pi)->Ignore = 1;
        pi = NetRingIncrementIndex(pr, pi);
    }
    pr->BeginIndex = pi;
    fr->BeginIndex = fr->EndIndex;
}

static EVT_WDF_TIMER EvtDataTimer;
static void EvtDataTimer(WDFTIMER timer)
{
    JX_DEVICE *d = JxGetDevice(WdfTimerGetParentObject(timer));
    if (!d->Native) return;
    KIRQL irql;
    KeAcquireSpinLock(&d->RxLock, &irql);
    if (d->RxQueue && d->RxNotify && Rd(d, JW_REG_RX_PROD) != d->RxConsumer &&
        InterlockedExchange(&d->RxNotify, 0))
        NetRxQueueNotifyMoreReceivedPacketsAvailable(d->RxQueue);
    KeReleaseSpinLock(&d->RxLock, irql);
    KeAcquireSpinLock(&d->TxLock, &irql);
    if (d->TxQueue && d->TxNotify && Rd(d, JW_REG_TX_CONS) != d->TxProducer &&
        InterlockedExchange(&d->TxNotify, 0))
        NetTxQueueNotifyMoreCompletedPacketsAvailable(d->TxQueue);
    KeReleaseSpinLock(&d->TxLock, irql);
    if (Rd(d, JW_REG_EV_PROD) != d->EvConsumer) KeSetEvent(&d->WorkEvent, IO_NO_INCREMENT, FALSE);
}

// Peer add/remove creates and destroys per-peer TX queues: never keep a stale queue handle.
static void EvtQueueDestroy(WDFOBJECT object)
{
    NETPACKETQUEUE queue = (NETPACKETQUEUE)object;
    JX_QUEUE *qc = JxGetQueue(queue);
    JX_DEVICE *d = qc->Dev ? qc->Dev : g_Dev;
    if (!d) return;
    KIRQL irql;
    if (qc->IsTx) {
        KeAcquireSpinLock(&d->TxLock, &irql);
        if (d->TxQueue == queue) d->TxQueue = nullptr;
        KeReleaseSpinLock(&d->TxLock, irql);
    } else {
        KeAcquireSpinLock(&d->RxLock, &irql);
        if (d->RxQueue == queue) d->RxQueue = nullptr;
        KeReleaseSpinLock(&d->RxLock, irql);
    }
}

static EVT_NET_ADAPTER_CREATE_TXQUEUE EvtCreateTxQueue;
static NTSTATUS EvtCreateTxQueue(NETADAPTER adapter, NETTXQUEUE_INIT *init)
{
    JX_DEVICE *d = JxGetAdapter(adapter)->Dev;
    JxLog("tx queue create requested (id %lu)", (ULONG)NetTxQueueInitGetQueueId(init));
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, JX_QUEUE);
    attributes.EvtDestroyCallback = EvtQueueDestroy;
    NET_PACKET_QUEUE_CONFIG config;
    NET_PACKET_QUEUE_CONFIG_INIT(&config, EvtTxAdvance, EvtTxNotify, EvtTxCancel);
    NETPACKETQUEUE queue;
    NTSTATUS st = NetTxQueueCreate(init, &attributes, &config, &queue);
    if (!NT_SUCCESS(st)) { JxLog("tx queue create failed 0x%08lx", st); return st; }
    JX_QUEUE *qc = JxGetQueue(queue);
    qc->Dev = d;
    qc->IsTx = true;
    NET_EXTENSION_QUERY q;
    NET_EXTENSION_QUERY_INIT(&q, NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_NAME,
                             NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_VERSION_1, NetExtensionTypeFragment);
    NetTxQueueGetExtension(queue, &q, &qc->Va);
    if (!qc->Va.Enabled) { JxLog("tx queue: virtual address extension not enabled"); return STATUS_NOT_SUPPORTED; }
    KIRQL irql;
    KeAcquireSpinLock(&d->TxLock, &irql);
    d->TxQueue = queue;
    KeReleaseSpinLock(&d->TxLock, irql);
    JxLog("tx queue created (peer %02x:%02x:%02x:%02x:%02x:%02x)", d->Peer[0], d->Peer[1], d->Peer[2], d->Peer[3], d->Peer[4], d->Peer[5]);
    return STATUS_SUCCESS;
}

static EVT_NET_ADAPTER_CREATE_RXQUEUE EvtCreateRxQueue;
static NTSTATUS EvtCreateRxQueue(NETADAPTER adapter, NETRXQUEUE_INIT *init)
{
    JX_DEVICE *d = JxGetAdapter(adapter)->Dev;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, JX_QUEUE);
    attributes.EvtDestroyCallback = EvtQueueDestroy;
    NET_PACKET_QUEUE_CONFIG config;
    NET_PACKET_QUEUE_CONFIG_INIT(&config, EvtRxAdvance, EvtRxNotify, EvtRxCancel);
    NETPACKETQUEUE queue;
    NTSTATUS st = NetRxQueueCreate(init, &attributes, &config, &queue);
    if (!NT_SUCCESS(st)) { JxLog("rx queue create failed 0x%08lx", st); return st; }
    JX_QUEUE *qc = JxGetQueue(queue);
    qc->Dev = d;
    qc->IsTx = false;
    NET_EXTENSION_QUERY q;
    NET_EXTENSION_QUERY_INIT(&q, NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_NAME,
                             NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_VERSION_1, NetExtensionTypeFragment);
    NetRxQueueGetExtension(queue, &q, &qc->Va);
    if (!qc->Va.Enabled) { JxLog("rx queue: virtual address extension not enabled"); return STATUS_NOT_SUPPORTED; }
    KIRQL irql;
    KeAcquireSpinLock(&d->RxLock, &irql);
    d->RxQueue = queue;
    KeReleaseSpinLock(&d->RxLock, irql);
    JxLog("rx queue created");
    return STATUS_SUCCESS;
}


// ---------------------------------------------------------------------------------------
// Adapter creation (WiFiCx calls this after PrepareHardware succeeds)
// ---------------------------------------------------------------------------------------
static EVT_NET_ADAPTER_SET_RECEIVE_FILTER EvtSetReceiveFilter;
static void EvtSetReceiveFilter(NETADAPTER, NETRECEIVEFILTER) {}

static EVT_WIFI_DEVICE_CREATE_ADAPTER EvtCreateAdapter;
static NTSTATUS EvtCreateAdapter(WDFDEVICE device, NETADAPTER_INIT *init)
{
    JX_DEVICE *d = JxGetDevice(device);
    if (WifiAdapterInitGetType(init) != WIFI_ADAPTER_EXTENSIBLE_STATION) return STATUS_NOT_SUPPORTED;
    // One TX queue per peer (the associated AP): WiFiCx creates it after WifiAdapterAddPeer.
    // A station adapter must declare Range 1: WifiAdapterInitialize asserts Range == 1 for
    // station types (WifiCx.sys+0xbe5bc, brk 0xf001 -> bugcheck 0x7E c0000420 in minidumps
    // 110826-23171/25656/26234 with 0.5.1; 0.5.0 used 1 and loaded).
    WIFI_ADAPTER_TX_DEMUX demux;
    WIFI_ADAPTER_TX_PEER_ADDRESS_DEMUX_INIT(&demux, JX_PEER_DEMUX_RANGE);
    WifiAdapterInitAddTxDemux(init, &demux);
    NET_ADAPTER_DATAPATH_CALLBACKS callbacks;
    NET_ADAPTER_DATAPATH_CALLBACKS_INIT(&callbacks, EvtCreateTxQueue, EvtCreateRxQueue);
    NetAdapterInitSetDatapathCallbacks(init, &callbacks);
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, JX_ADAPTER);
    NETADAPTER adapter;
    NTSTATUS st = NetAdapterCreate(init, &attributes, &adapter);
    if (!NT_SUCCESS(st)) { JxLog("adapter create failed 0x%08lx", st); return st; }
    st = WifiAdapterInitialize(adapter);
    if (!NT_SUCCESS(st)) { JxLog("adapter initialize failed 0x%08lx", st); return st; }
    JxGetAdapter(adapter)->Dev = d;
    d->Adapter = adapter;
    d->PortId = WifiAdapterGetPortId(adapter);

    NET_ADAPTER_TX_CAPABILITIES tx;
    NET_ADAPTER_TX_CAPABILITIES_INIT(&tx, 64);
    NET_ADAPTER_RX_CAPABILITIES rx;
    NET_ADAPTER_RX_CAPABILITIES_INIT_SYSTEM_MANAGED(&rx, 2048, 1);
    NetAdapterSetDataPathCapabilities(adapter, &tx, &rx);
    NET_ADAPTER_LINK_LAYER_CAPABILITIES link;
    NET_ADAPTER_LINK_LAYER_CAPABILITIES_INIT(&link, 300000000ULL, 300000000ULL);
    NetAdapterSetLinkLayerCapabilities(adapter, &link);
    NetAdapterSetLinkLayerMtuSize(adapter, JW_FRAME_MAX - 14);
    NET_ADAPTER_RECEIVE_FILTER_CAPABILITIES filter;
    NET_ADAPTER_RECEIVE_FILTER_CAPABILITIES_INIT(&filter, EvtSetReceiveFilter);
    filter.SupportedPacketFilters = NetPacketFilterFlagDirected | NetPacketFilterFlagMulticast |
                                    NetPacketFilterFlagAllMulticast | NetPacketFilterFlagBroadcast |
                                    NetPacketFilterFlagPromiscuous;
    filter.MaximumMulticastAddresses = 32;
    NetAdapterSetReceiveFilterCapabilities(adapter, &filter);
    NET_ADAPTER_LINK_LAYER_ADDRESS address;
    NET_ADAPTER_LINK_LAYER_ADDRESS_INIT(&address, 6, d->Mac);
    NetAdapterSetPermanentLinkLayerAddress(adapter, &address);
    NetAdapterSetCurrentLinkLayerAddress(adapter, &address);
    st = NetAdapterStart(adapter);
    JxLog("adapter start 0x%08lx", st);
    if (NT_SUCCESS(st)) SetLink(d, false);
    return st;
}

static EVT_WIFI_DEVICE_CREATE_WIFIDIRECTDEVICE EvtCreateWfd;
static NTSTATUS EvtCreateWfd(WDFDEVICE, WIFIDIRECT_DEVICE_INIT *) { return STATUS_NOT_SUPPORTED; }

// ---------------------------------------------------------------------------------------
// Capabilities
// ---------------------------------------------------------------------------------------
static NTSTATUS SetCapabilities(JX_DEVICE *d)
{
    WIFI_DEVICE_CAPABILITIES dc;
    WIFI_DEVICE_CAPABILITIES_INIT(&dc);
    dc.HardwareRadioState = TRUE;
    dc.SoftwareRadioState = TRUE;
    static const char fw[] = "BCM4387 (m1n1 host)";
    RtlCopyMemory(dc.FirmwareVersion, fw, sizeof(fw));
    dc.NumRxStreams = 2;
    dc.NumTxStreams = 2;
    dc.BluetoothCoexistenceSupport = WDI_BLUETOOTH_COEXISTENCE_PERFORMANCE_MAINTAINED;
    NTSTATUS st = WifiDeviceSetDeviceCapabilities(d->Device, &dc);
    if (!NT_SUCCESS(st)) return st;

    static DOT11_AUTH_CIPHER_PAIR unicast[] = {
        {DOT11_AUTH_ALGO_80211_OPEN, DOT11_CIPHER_ALGO_NONE},
        {DOT11_AUTH_ALGO_RSNA_PSK, DOT11_CIPHER_ALGO_CCMP},
    };
    static DOT11_AUTH_CIPHER_PAIR multicast[] = {
        {DOT11_AUTH_ALGO_80211_OPEN, DOT11_CIPHER_ALGO_NONE},
        {DOT11_AUTH_ALGO_RSNA_PSK, DOT11_CIPHER_ALGO_CCMP},
        {DOT11_AUTH_ALGO_RSNA_PSK, DOT11_CIPHER_ALGO_TKIP},
    };
    static DOT11_AUTH_CIPHER_PAIR mgmt[] = {
        {DOT11_AUTH_ALGO_80211_OPEN, DOT11_CIPHER_ALGO_NONE},
    };
    WIFI_STATION_CAPABILITIES sc;
    WIFI_STATION_CAPABILITIES_INIT(&sc);
    sc.ScanSSIDListSize = 4;
    sc.DesiredSSIDListSize = 1;
    sc.PrivacyExemptionListSize = 1;
    sc.KeyMappingTableSize = 4;
    sc.DefaultKeyTableSize = 4;
    sc.WEPKeyValueMaxLength = 0;
    sc.MaxNumPerSTA = 4;
    sc.NumSupportedUnicastAlgorithms = ARRAYSIZE(unicast);
    sc.UnicastAlgorithmsList = unicast;
    sc.NumSupportedMulticastDataAlgorithms = ARRAYSIZE(multicast);
    sc.MulticastDataAlgorithmsList = multicast;
    sc.NumSupportedMulticastMgmtAlgorithms = ARRAYSIZE(mgmt);
    sc.MulticastMgmtAlgorithmsList = mgmt;
    st = WifiDeviceSetStationCapabilities(d->Device, &sc);
    if (!NT_SUCCESS(st)) return st;

    static WDI_PHY_TYPE phy24[] = {WDI_PHY_TYPE_HRDSSS, WDI_PHY_TYPE_ERP, WDI_PHY_TYPE_HT, WDI_PHY_TYPE_HE};
    static WDI_PHY_TYPE phy5[] = {WDI_PHY_TYPE_OFDM, WDI_PHY_TYPE_HT, WDI_PHY_TYPE_VHT, WDI_PHY_TYPE_HE};
    static WDI_CHANNEL_MAPPING_ENTRY ch24[13];
    for (UINT32 i = 0; i < 13; ++i) ch24[i] = {i + 1, 2412 + 5 * i};
    static const UINT32 c5[] = {36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124,
                                128, 132, 136, 140, 144, 149, 153, 157, 161, 165};
    static WDI_CHANNEL_MAPPING_ENTRY ch5[ARRAYSIZE(c5)];
    for (UINT32 i = 0; i < ARRAYSIZE(c5); ++i) ch5[i] = {c5[i], 5000 + 5 * c5[i]};
    static UINT32 w24[] = {20, 40}, w5[] = {20, 40, 80};
    WIFI_BAND_INFO bands[2] = {};
    bands[0] = {WDI_BAND_ID_2400, TRUE, ARRAYSIZE(phy24), phy24, ARRAYSIZE(ch24), ch24, ARRAYSIZE(w24), w24};
    bands[1] = {WDI_BAND_ID_5000, TRUE, ARRAYSIZE(phy5), phy5, ARRAYSIZE(ch5), ch5, ARRAYSIZE(w5), w5};
    WIFI_BAND_CAPABILITIES bc = {};
    bc.Size = sizeof(bc);
    bc.NumBands = 2;
    bc.BandInfoList = bands;
    st = WifiDeviceSetBandCapabilities(d->Device, &bc);
    if (!NT_SUCCESS(st)) return st;

    static WIFI_PHY_INFO phys[5];
    static const WDI_PHY_TYPE types[] = {WDI_PHY_TYPE_HRDSSS, WDI_PHY_TYPE_ERP, WDI_PHY_TYPE_OFDM,
                                         WDI_PHY_TYPE_HT, WDI_PHY_TYPE_VHT};
    static const UINT16 rates[] = {2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108};
    for (ULONG i = 0; i < ARRAYSIZE(types); ++i) {
        RtlZeroMemory(&phys[i], sizeof(phys[i]));
        phys[i].PhyType = types[i];
        ULONG first = types[i] == WDI_PHY_TYPE_HRDSSS ? 0 : (types[i] == WDI_PHY_TYPE_ERP ? 0 : 4);
        ULONG last = types[i] == WDI_PHY_TYPE_HRDSSS ? 4 : ARRAYSIZE(rates);
        for (ULONG r = first; r < last; ++r)
            phys[i].DataRateList[phys[i].NumberDataRateEntries++] = {WDI_DATA_RATE_RX_RATE | WDI_DATA_RATE_TX_RATE, rates[r]};
    }
    WIFI_PHY_CAPABILITIES pc = {};
    pc.Size = sizeof(pc);
    pc.NumPhyTypes = ARRAYSIZE(types);
    pc.PhyInfoList = phys;
    return WifiDeviceSetPhyCapabilities(d->Device, &pc);
}

// ---------------------------------------------------------------------------------------
// PnP
// ---------------------------------------------------------------------------------------
static NTSTATUS EnterNative(JX_DEVICE *d)
{
    if (Rd(d, JW_REG_MAGIC) != JW_MAGIC || Rd(d, JW_REG_VERSION) != JW_VERSION)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    if (!(Rd(d, JW_REG_FEATURES) & JW_FEATURE_NATIVE)) return STATUS_NOT_SUPPORTED;
    // Wait for the host to finish firmware bring-up (READY), at most 20 s.
    for (ULONG i = 0; !(Rd(d, JW_REG_STATUS) & JW_STATUS_READY); ++i) {
        if (i >= 2000) return STATUS_DEVICE_NOT_READY;
        Sleep(10);
    }
    ULONG low = Rd(d, JW_REG_MAC_LOW), high = Rd(d, JW_REG_MAC_HIGH);
    for (ULONG i = 0; i < 4; ++i) d->Mac[i] = (UCHAR)(low >> (8 * i));
    d->Mac[4] = (UCHAR)high;
    d->Mac[5] = (UCHAR)(high >> 8);
    if ((d->Mac[0] & 1) || !(d->Mac[0] | d->Mac[1] | d->Mac[2] | d->Mac[3] | d->Mac[4] | d->Mac[5]))
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    // A previous instance in this boot may already have switched the host to native mode,
    // in which case the live counters are the native copies.
    bool already = Rd(d, JW_REG_MODE_ACK) == JW_MODE_NATIVE;
    d->TxProducer = Rd(d, already ? JW_REG_N_TX_PROD : JW_REG_TX_PROD);
    d->RxConsumer = Rd(d, already ? JW_REG_N_RX_CONS : JW_REG_RX_CONS);
    d->CtlSeq = Rd(d, JW_REG_CTL_SEQ);
    d->EvConsumer = Rd(d, JW_REG_EV_PROD);
    Wr(d, JW_REG_N_TX_PROD, d->TxProducer);
    Wr(d, JW_REG_N_RX_CONS, d->RxConsumer);
    Wr(d, JW_REG_EV_CONS, d->EvConsumer);
    KeMemoryBarrier();
    Wr(d, JW_REG_MODE, JW_MODE_NATIVE);
    for (ULONG i = 0; Rd(d, JW_REG_MODE_ACK) != JW_MODE_NATIVE; ++i) {
        if (i >= 500) return STATUS_IO_TIMEOUT;
        Sleep(10);
    }
    // A previous instance may have left a command in flight (removed mid-command): let the
    // host finish it, then continue after the last sequence number the guest issued, so the
    // next command can never be mistaken for, or answered with, the stale one.
    for (ULONG i = 0; Rd(d, JW_REG_CTL_DONE) != Rd(d, JW_REG_CTL_SEQ) && i < 300; ++i) Sleep(10);
    d->CtlSeq = Rd(d, JW_REG_CTL_SEQ);
    d->EvConsumer = Rd(d, JW_REG_EV_PROD);
    Wr(d, JW_REG_EV_CONS, d->EvConsumer);
    d->Native = true;
    return STATUS_SUCCESS;
}

static NTSTATUS FirmwareSetup(JX_DEVICE *d)
{
    // Drop any association the host made before guest entry; Windows owns the link now.
    (void)FwIoctl(d, BCM_C_DISASSOC, d->Ctl, 0, nullptr, nullptr);
    d->ScanV2 = NT_SUCCESS(FwVar(d, "scan_ver", nullptr, 0, true, 4));
    // Enable the events this driver consumes (in addition to the host's set).
    NTSTATUS st = FwVar(d, "event_msgs", nullptr, 0, true, 32);
    if (!NT_SUCCESS(st)) return st;
    UCHAR mask[32];
    RtlCopyMemory(mask, d->Ctl, sizeof(mask));
    static const UCHAR wanted[] = {BCM_E_SET_SSID, BCM_E_DEAUTH, BCM_E_DEAUTH_IND, BCM_E_DISASSOC,
                                   BCM_E_DISASSOC_IND, BCM_E_LINK, BCM_E_MIC_ERROR, BCM_E_ESCAN_RESULT};
    for (ULONG i = 0; i < sizeof(wanted); ++i) mask[wanted[i] / 8] |= (UCHAR)(1u << (wanted[i] % 8));
    st = FwVar(d, "event_msgs", mask, sizeof(mask), false);
    if (!NT_SUCCESS(st)) return st;
    (void)FwSetInt(d, "sup_wpa", 0);
    return STATUS_SUCCESS;
}

static EVT_WDF_DEVICE_PREPARE_HARDWARE EvtPrepareHardware;
static NTSTATUS EvtPrepareHardware(WDFDEVICE device, WDFCMRESLIST, WDFCMRESLIST translated)
{
    JX_DEVICE *d = JxGetDevice(device);
    for (ULONG i = 0; i < WdfCmResourceListGetCount(translated); ++i) {
        auto *r = WdfCmResourceListGetDescriptor(translated, i);
        if (r->Type == CmResourceTypeMemory && r->u.Memory.Length >= JW_SIZE && !d->Regs) {
            d->Regs = static_cast<PUCHAR>(MmMapIoSpaceEx(r->u.Memory.Start, JW_SIZE, PAGE_READWRITE | PAGE_NOCACHE));
            d->RegsLength = JW_SIZE;
        }
    }
    if (!d->Regs) { JxLog("prepare: no bridge window resource"); return STATUS_DEVICE_CONFIGURATION_ERROR; }
    NTSTATUS st = EnterNative(d);
    if (!NT_SUCCESS(st)) { JxLog("prepare: native mode failed 0x%08lx (host image without Wi-Fi or firmware not ready)", st); return st; }

    KeInitializeEvent(&d->WorkEvent, SynchronizationEvent, FALSE);
    d->Stop = 0;
    d->RadioOn = true;
    // The firmware setup runs before the worker exists; FwIoctl is single-threaded here.
    JxLog("J316WifiCx " JX_VERSION " native mode on, mac %02x:%02x:%02x:%02x:%02x:%02x, OS WDI 0x%lx", d->Mac[0], d->Mac[1], d->Mac[2],
          d->Mac[3], d->Mac[4], d->Mac[5], d->Tlv.PeerVersion);
    st = FirmwareSetup(d);
    if (!NT_SUCCESS(st)) { JxLog("prepare: firmware setup failed 0x%08lx", st); return st; }
    st = SetCapabilities(d);
    if (!NT_SUCCESS(st)) { JxLog("prepare: capabilities failed 0x%08lx", st); return st; }

    HANDLE thread;
    st = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, nullptr, nullptr, nullptr, Worker, d);
    if (!NT_SUCCESS(st)) return st;
    st = ObReferenceObjectByHandle(thread, THREAD_ALL_ACCESS, *PsThreadType, KernelMode, (PVOID *)&d->Worker, nullptr);
    ZwClose(thread);
    if (!NT_SUCCESS(st)) { d->Stop = 1; return st; }

    WDF_TIMER_CONFIG tc;
    WDF_TIMER_CONFIG_INIT_PERIODIC(&tc, EvtDataTimer, 1);
    tc.UseHighResolutionTimer = WdfTrue;
    WDF_OBJECT_ATTRIBUTES ta;
    WDF_OBJECT_ATTRIBUTES_INIT(&ta);
    ta.ParentObject = device;
    st = WdfTimerCreate(&tc, &ta, &d->DataTimer);
    if (!NT_SUCCESS(st)) return st;
    WdfTimerStart(d->DataTimer, WDF_REL_TIMEOUT_IN_MS(1));
    return STATUS_SUCCESS;
}

static EVT_WDF_DEVICE_RELEASE_HARDWARE EvtReleaseHardware;
static NTSTATUS EvtReleaseHardware(WDFDEVICE device, WDFCMRESLIST)
{
    JX_DEVICE *d = JxGetDevice(device);
    if (d->DataTimer) { WdfTimerStop(d->DataTimer, TRUE); }
    if (d->Worker) {
        InterlockedExchange(&d->Stop, 1);
        KeSetEvent(&d->WorkEvent, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject(d->Worker, Executive, KernelMode, FALSE, nullptr);
        ObDereferenceObject(d->Worker);
        d->Worker = nullptr;
    }
    if (d->Regs) {
        // Leave the radio idle; the host keeps the firmware running for the next guest. The
        // worker is gone, so clear Stop: FwIoctl gives up at once while it is set ("fw cmd 52
        // timed out" on every reinstall), which left a command in flight for the next instance.
        InterlockedExchange(&d->Stop, 0);
        if (d->Native && !d->FwFault) (void)FwIoctl(d, BCM_C_DISASSOC, d->Ctl, 0, nullptr, nullptr);
        InterlockedExchange(&d->Stop, 1);
        MmUnmapIoSpace(d->Regs, d->RegsLength);
        d->Regs = nullptr;
    }
    d->Native = false;
    return STATUS_SUCCESS;
}

static EVT_WDF_DRIVER_DEVICE_ADD EvtDeviceAdd;
static NTSTATUS EvtDeviceAdd(WDFDRIVER, PWDFDEVICE_INIT init)
{
    NTSTATUS st = NetDeviceInitConfig(init);
    if (!NT_SUCCESS(st)) return st;
    st = WifiDeviceInitConfig(init);
    if (!NT_SUCCESS(st)) return st;
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = EvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = EvtReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &pnp);
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, JX_DEVICE);
    WDFDEVICE device;
    st = WdfDeviceCreate(&init, &attributes, &device);
    if (!NT_SUCCESS(st)) return st;
    JX_DEVICE *d = JxGetDevice(device);
    d->WdfTriageInfoPtr = WdfGetTriageInfo();
    g_Dev = d;
    d->Device = device;
    KeInitializeSpinLock(&d->CmdLock);
    KeInitializeSpinLock(&d->TxLock);
    KeInitializeSpinLock(&d->RxLock);
    WIFI_DEVICE_CONFIG config;
    WIFI_DEVICE_CONFIG_INIT(&config, WDI_VERSION_LATEST, EvtSendCommand, EvtCreateAdapter, EvtCreateWfd);
    st = WifiDeviceInitialize(device, &config);
    if (!NT_SUCCESS(st)) return st;
    d->Tlv.AllocationContext = 0;
    d->Tlv.PeerVersion = WifiDeviceGetOsWdiVersion(device);
    return STATUS_SUCCESS;
}

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, EvtDeviceAdd);
    config.DriverPoolTag = JX_TAG;
    return WdfDriverCreate(driver, registry, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
