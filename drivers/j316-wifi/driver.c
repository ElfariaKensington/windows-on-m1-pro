/* SPDX-License-Identifier: MIT
 * Classic NDIS 6 Ethernet endpoint for the host-owned J316 Wi-Fi bridge.
 * No physical radio, DMA, interrupts, firmware or native WLAN interfaces here.
 */
#include <ndis.h>
#include "bridge.h"

#define JW_TAG 'fWJJ'
#define JW_MTU (JW_FRAME_MAX - 14u)
#define JW_MULTICAST_MAX 32u
#define JW_SPEED 1000000000ULL /* Synthetic interface capacity, not radio rate. */
/* Read-only vendor diagnostics; values are raw bridge register snapshots. */
#define OID_JW_PHASE ((NDIS_OID)0xff000201)
#define OID_JW_ERROR ((NDIS_OID)0xff000202)
#define OID_JW_STATUS ((NDIS_OID)0xff000203)
#define OID_JW_COMMAND_STATUS ((NDIS_OID)0xff000204)
#define JW_FILTERS (NDIS_PACKET_TYPE_DIRECTED | NDIS_PACKET_TYPE_MULTICAST | \
                    NDIS_PACKET_TYPE_ALL_MULTICAST | NDIS_PACKET_TYPE_BROADCAST | \
                    NDIS_PACKET_TYPE_PROMISCUOUS)
#define JW_STATS_VALID (NDIS_STATISTICS_FLAGS_VALID_DIRECTED_FRAMES_RCV | \
    NDIS_STATISTICS_FLAGS_VALID_MULTICAST_FRAMES_RCV | NDIS_STATISTICS_FLAGS_VALID_BROADCAST_FRAMES_RCV | \
    NDIS_STATISTICS_FLAGS_VALID_BYTES_RCV | NDIS_STATISTICS_FLAGS_VALID_RCV_DISCARDS | \
    NDIS_STATISTICS_FLAGS_VALID_RCV_ERROR | NDIS_STATISTICS_FLAGS_VALID_DIRECTED_FRAMES_XMIT | \
    NDIS_STATISTICS_FLAGS_VALID_MULTICAST_FRAMES_XMIT | NDIS_STATISTICS_FLAGS_VALID_BROADCAST_FRAMES_XMIT | \
    NDIS_STATISTICS_FLAGS_VALID_BYTES_XMIT | NDIS_STATISTICS_FLAGS_VALID_XMIT_ERROR | \
    NDIS_STATISTICS_FLAGS_VALID_XMIT_DISCARDS | NDIS_STATISTICS_FLAGS_VALID_DIRECTED_BYTES_RCV | \
    NDIS_STATISTICS_FLAGS_VALID_MULTICAST_BYTES_RCV | NDIS_STATISTICS_FLAGS_VALID_BROADCAST_BYTES_RCV | \
    NDIS_STATISTICS_FLAGS_VALID_DIRECTED_BYTES_XMIT | NDIS_STATISTICS_FLAGS_VALID_MULTICAST_BYTES_XMIT | \
    NDIS_STATISTICS_FLAGS_VALID_BROADCAST_BYTES_XMIT)
#define JW_STATS_SUPPORTED (NDIS_STATISTICS_DIRECTED_FRAMES_RCV_SUPPORTED | \
    NDIS_STATISTICS_MULTICAST_FRAMES_RCV_SUPPORTED | NDIS_STATISTICS_BROADCAST_FRAMES_RCV_SUPPORTED | \
    NDIS_STATISTICS_BYTES_RCV_SUPPORTED | NDIS_STATISTICS_RCV_DISCARDS_SUPPORTED | \
    NDIS_STATISTICS_RCV_ERROR_SUPPORTED | NDIS_STATISTICS_DIRECTED_FRAMES_XMIT_SUPPORTED | \
    NDIS_STATISTICS_MULTICAST_FRAMES_XMIT_SUPPORTED | NDIS_STATISTICS_BROADCAST_FRAMES_XMIT_SUPPORTED | \
    NDIS_STATISTICS_BYTES_XMIT_SUPPORTED | NDIS_STATISTICS_XMIT_ERROR_SUPPORTED | \
    NDIS_STATISTICS_XMIT_DISCARDS_SUPPORTED | NDIS_STATISTICS_DIRECTED_BYTES_RCV_SUPPORTED | \
    NDIS_STATISTICS_MULTICAST_BYTES_RCV_SUPPORTED | NDIS_STATISTICS_BROADCAST_BYTES_RCV_SUPPORTED | \
    NDIS_STATISTICS_DIRECTED_BYTES_XMIT_SUPPORTED | NDIS_STATISTICS_MULTICAST_BYTES_XMIT_SUPPORTED | \
    NDIS_STATISTICS_BROADCAST_BYTES_XMIT_SUPPORTED | NDIS_STATISTICS_XMIT_OK_SUPPORTED | \
    NDIS_STATISTICS_RCV_OK_SUPPORTED | NDIS_STATISTICS_RCV_NO_BUFFER_SUPPORTED | \
    NDIS_STATISTICS_GEN_STATISTICS_SUPPORTED)

C_ASSERT(JW_TX_BASE + JW_RING_COUNT * JW_SLOT_SIZE <= JW_RX_BASE);
C_ASSERT(JW_RX_BASE + JW_RING_COUNT * JW_SLOT_SIZE <= JW_SIZE);
C_ASSERT(JW_FRAME_MAX + sizeof(ULONG) <= JW_SLOT_SIZE);
/* Slot = [u32 length][frame]; copied as whole aligned u64 words. */
#define JW_SLOT_WORDS(length) ((sizeof(ULONG) + (length) + 7u) / 8u)
C_ASSERT(JW_SLOT_WORDS(JW_FRAME_MAX) * 8u <= JW_SLOT_SIZE);
C_ASSERT((JW_TX_BASE % 8u) == 0 && (JW_RX_BASE % 8u) == 0 && (JW_SLOT_SIZE % 8u) == 0);
/* One poll expiration may hold DISPATCH_LEVEL for at most this long (100 ns units).
 * The poll period is 5 ms; work left over is taken by the next expiration. A DPC that
 * outlasted its period kept the CPU at DISPATCH indefinitely under sustained receive
 * (DPC_WATCHDOG_VIOLATION 0x133/1, J316Wifi poll on the stack). */
#define JW_POLL_BUDGET_100NS 10000u

typedef struct _JW_FRAME {
    ULONG Length;
    UCHAR Data[JW_FRAME_MAX];
} JW_FRAME;

typedef struct _JW_ADAPTER {
    NDIS_HANDLE Handle, Timer, ReceivePool;
    PUCHAR Registers;
    NDIS_SPIN_LOCK Lock;
    KEVENT SendIdle;
    ULONG ActiveSends;
    volatile LONG PollActive, Shutdown;
    BOOLEAN Running, Removed, Faulted, LinkUp, LinkIndicated, MacValid, RingsStarted;
    ULONG Phase, Error, BridgeStatus, CommandStatus;
    NDIS_DEVICE_POWER_STATE Power;
    ULONG TxProducer, RxConsumer;
    ULONG QueueHead, QueueCount;
    JW_FRAME Queue[JW_RING_COUNT];
    UCHAR Mac[6], Multicast[JW_MULTICAST_MAX][6];
    ULONG MulticastCount, PacketFilter, Lookahead;
    NDIS_STATISTICS_INFO Stats;
    UCHAR ReceiveData[JW_FRAME_MAX];
    ULONG64 Staging[JW_SLOT_SIZE / 8u];
    LARGE_INTEGER PerfFrequency;
    PMDL ReceiveMdl;
    PNET_BUFFER_LIST ReceiveNbl;
    BOOLEAN JoinPending, JoinInFlight;
    ULONG SsidLength, PskLength;
    UCHAR Ssid[32], Psk[64];
} JW_ADAPTER;

static NDIS_HANDLE JwDriver;
DRIVER_INITIALIZE DriverEntry;
static MINIPORT_INITIALIZE JwInitialize;
static MINIPORT_HALT JwHalt;
static MINIPORT_UNLOAD JwUnload;
static MINIPORT_PAUSE JwPause;
static MINIPORT_RESTART JwRestart;
static MINIPORT_OID_REQUEST JwOid;
static MINIPORT_SEND_NET_BUFFER_LISTS JwSend;
static MINIPORT_RETURN_NET_BUFFER_LISTS JwReturn;
static MINIPORT_CANCEL_SEND JwCancelSend;
static MINIPORT_CHECK_FOR_HANG JwCheckHang;
static MINIPORT_RESET JwReset;
static MINIPORT_DEVICE_PNP_EVENT_NOTIFY JwPnp;
static MINIPORT_SHUTDOWN JwShutdown;
static MINIPORT_CANCEL_OID_REQUEST JwCancelOid;
static NDIS_TIMER_FUNCTION JwPoll;

static NDIS_OID JwOids[] = {
    OID_GEN_SUPPORTED_LIST, OID_GEN_HARDWARE_STATUS, OID_GEN_MEDIA_SUPPORTED,
    OID_GEN_MEDIA_IN_USE, OID_GEN_MAXIMUM_LOOKAHEAD, OID_GEN_MAXIMUM_FRAME_SIZE,
    OID_GEN_LINK_SPEED, OID_GEN_TRANSMIT_BUFFER_SPACE, OID_GEN_RECEIVE_BUFFER_SPACE,
    OID_GEN_TRANSMIT_BLOCK_SIZE, OID_GEN_RECEIVE_BLOCK_SIZE, OID_GEN_VENDOR_ID,
    OID_GEN_VENDOR_DESCRIPTION, OID_GEN_CURRENT_PACKET_FILTER, OID_GEN_CURRENT_LOOKAHEAD,
    OID_GEN_DRIVER_VERSION, OID_GEN_MAXIMUM_TOTAL_SIZE, OID_GEN_MAC_OPTIONS,
    OID_GEN_MEDIA_CONNECT_STATUS, OID_GEN_MAXIMUM_SEND_PACKETS, OID_GEN_VENDOR_DRIVER_VERSION,
    OID_GEN_PHYSICAL_MEDIUM, OID_GEN_STATISTICS, OID_GEN_XMIT_OK, OID_GEN_RCV_OK,
    OID_GEN_XMIT_ERROR, OID_GEN_RCV_ERROR, OID_GEN_RCV_NO_BUFFER,
    OID_GEN_INTERRUPT_MODERATION,
    OID_802_3_PERMANENT_ADDRESS, OID_802_3_CURRENT_ADDRESS, OID_802_3_MULTICAST_LIST,
    OID_802_3_MAXIMUM_LIST_SIZE, OID_802_3_RCV_ERROR_ALIGNMENT,
    OID_802_3_XMIT_ONE_COLLISION, OID_802_3_XMIT_MORE_COLLISIONS,
    OID_PNP_CAPABILITIES, OID_PNP_QUERY_POWER, OID_PNP_SET_POWER,
    OID_JW_PHASE, OID_JW_ERROR, OID_JW_STATUS, OID_JW_COMMAND_STATUS
};

static ULONG JwRead(JW_ADAPTER *a, ULONG offset)
{
    return READ_REGISTER_ULONG((PULONG)(a->Registers + offset));
}

static VOID JwWrite(JW_ADAPTER *a, ULONG offset, ULONG value)
{
    WRITE_REGISTER_ULONG((PULONG)(a->Registers + offset), value);
}

/* Lock held. Counters use modulo-2^32 arithmetic, never signed comparison. */
/* Uncached device accesses dominate the poll cost: move whole u64 words (8x fewer than
 * byte accesses) through a cached staging buffer. Slots are 8-byte aligned. */
static VOID JwSlotRead(JW_ADAPTER *a, ULONG slot, ULONG length)
{
    READ_REGISTER_BUFFER_ULONG64((PULONG64)(a->Registers + slot), a->Staging, JW_SLOT_WORDS(length));
}

static VOID JwSlotWrite(JW_ADAPTER *a, ULONG slot, const UCHAR *data, ULONG length)
{
    PUCHAR staging = (PUCHAR)a->Staging;
    ULONG words = JW_SLOT_WORDS(length);
    *(ULONG UNALIGNED *)staging = length;
    RtlCopyMemory(staging + sizeof(ULONG), data, length);
    RtlZeroMemory(staging + sizeof(ULONG) + length, words * 8u - sizeof(ULONG) - length);
    WRITE_REGISTER_BUFFER_ULONG64((PULONG64)(a->Registers + slot), a->Staging, words);
}

static BOOLEAN JwOverBudget(JW_ADAPTER *a, LONGLONG start)
{
    LONGLONG elapsed = KeQueryPerformanceCounter(NULL).QuadPart - start;
    return elapsed * 10000000LL >= (LONGLONG)JW_POLL_BUDGET_100NS * a->PerfFrequency.QuadPart;
}

static BOOLEAN JwCounters(JW_ADAPTER *a, ULONG *txConsumer, ULONG *rxProducer)
{
    *txConsumer = JwRead(a, JW_REG_TX_CONS);
    *rxProducer = JwRead(a, JW_REG_RX_PROD);
    if (JwRead(a, JW_REG_TX_PROD) != a->TxProducer ||
        JwRead(a, JW_REG_RX_CONS) != a->RxConsumer ||
        (ULONG)(a->TxProducer - *txConsumer) > JW_RING_COUNT ||
        (ULONG)(*rxProducer - a->RxConsumer) > JW_RING_COUNT) {
        a->Faulted = TRUE;
        return FALSE; /* Do not reset counters owned by the peer. */
    }
    KeMemoryBarrier();
    return TRUE;
}

static VOID JwDiscardQueue(JW_ADAPTER *a)
{
    a->Stats.ifOutDiscards += a->QueueCount;
    a->QueueCount = a->QueueHead = 0;
}

/* Full NDIS statistics describe traffic accepted/published by this endpoint. */
static VOID JwAccount(JW_ADAPTER *a, const UCHAR *data, ULONG length, BOOLEAN receive)
{
    BOOLEAN broadcast = TRUE;
    ULONG i;
    for (i = 0; i < 6; ++i) if (data[i] != 0xff) broadcast = FALSE;
    if (receive) {
        a->Stats.ifHCInOctets += length;
        if (broadcast) { ++a->Stats.ifHCInBroadcastPkts; a->Stats.ifHCInBroadcastOctets += length; }
        else if (data[0] & 1) { ++a->Stats.ifHCInMulticastPkts; a->Stats.ifHCInMulticastOctets += length; }
        else { ++a->Stats.ifHCInUcastPkts; a->Stats.ifHCInUcastOctets += length; }
    } else {
        a->Stats.ifHCOutOctets += length;
        if (broadcast) { ++a->Stats.ifHCOutBroadcastPkts; a->Stats.ifHCOutBroadcastOctets += length; }
        else if (data[0] & 1) { ++a->Stats.ifHCOutMulticastPkts; a->Stats.ifHCOutMulticastOctets += length; }
        else { ++a->Stats.ifHCOutUcastPkts; a->Stats.ifHCOutUcastOctets += length; }
    }
}

static BOOLEAN JwAccept(JW_ADAPTER *a, const UCHAR *data)
{
    ULONG i;
    BOOLEAN broadcast = TRUE;
    if (a->PacketFilter & NDIS_PACKET_TYPE_PROMISCUOUS) return TRUE;
    for (i = 0; i < 6; ++i) if (data[i] != 0xff) broadcast = FALSE;
    if (broadcast) return (a->PacketFilter & NDIS_PACKET_TYPE_BROADCAST) != 0;
    if (!(data[0] & 1)) return (a->PacketFilter & NDIS_PACKET_TYPE_DIRECTED) &&
        RtlEqualMemory(data, a->Mac, 6);
    if (a->PacketFilter & NDIS_PACKET_TYPE_ALL_MULTICAST) return TRUE;
    if (a->PacketFilter & NDIS_PACKET_TYPE_MULTICAST)
        for (i = 0; i < a->MulticastCount; ++i)
            if (RtlEqualMemory(data, a->Multicast[i], 6)) return TRUE;
    return FALSE;
}

static VOID JwIndicateLink(JW_ADAPTER *a, BOOLEAN up)
{
    NDIS_LINK_STATE link;
    NDIS_STATUS_INDICATION indication;
    NdisZeroMemory(&link, sizeof(link));
    link.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    link.Header.Revision = NDIS_LINK_STATE_REVISION_1;
    link.Header.Size = NDIS_SIZEOF_LINK_STATE_REVISION_1;
    link.MediaConnectState = up ? MediaConnectStateConnected : MediaConnectStateDisconnected;
    link.MediaDuplexState = MediaDuplexStateFull;
    link.XmitLinkSpeed = link.RcvLinkSpeed = up ? JW_SPEED : 0;
    link.PauseFunctions = NdisPauseFunctionsUnsupported;
    NdisZeroMemory(&indication, sizeof(indication));
    indication.Header.Type = NDIS_OBJECT_TYPE_STATUS_INDICATION;
    indication.Header.Revision = NDIS_STATUS_INDICATION_REVISION_1;
    indication.Header.Size = NDIS_SIZEOF_STATUS_INDICATION_REVISION_1;
    indication.SourceHandle = a->Handle;
    indication.StatusCode = NDIS_STATUS_LINK_STATE;
    indication.StatusBuffer = &link;
    indication.StatusBufferSize = sizeof(link);
    NdisMIndicateStatusEx(a->Handle, &indication);
}

static VOID JwJoin(JW_ADAPTER *a)
{
    /* Host clears COMMAND after copying; erase mailbox only after that ACK. */
    WRITE_REGISTER_BUFFER_UCHAR(a->Registers + JW_CONFIG_SSID, a->Ssid, sizeof(a->Ssid));
    WRITE_REGISTER_BUFFER_UCHAR(a->Registers + JW_CONFIG_PSK, a->Psk, sizeof(a->Psk));
    JwWrite(a, JW_CONFIG_SSID_LEN, a->SsidLength);
    JwWrite(a, JW_CONFIG_PSK_LEN, a->PskLength);
    KeMemoryBarrier();
    JwWrite(a, JW_REG_COMMAND, JW_COMMAND_JOIN);
    RtlSecureZeroMemory(a->Psk, sizeof(a->Psk));
    RtlSecureZeroMemory(a->Ssid, sizeof(a->Ssid));
    a->PskLength = a->SsidLength = 0;
    a->JoinPending = FALSE;
    a->JoinInFlight = TRUE;
}

static VOID JwPoll(PVOID system1, PVOID context, PVOID system2, PVOID system3)
{
    JW_ADAPTER *a = context;
    ULONG txConsumer = 0, rxProducer = 0, status = 0, i;
    BOOLEAN up = FALSE, indicate;
    LONGLONG start = KeQueryPerformanceCounter(NULL).QuadPart;
    UNREFERENCED_PARAMETER(system1);
    UNREFERENCED_PARAMETER(system2);
    UNREFERENCED_PARAMETER(system3);
    /* A slow indication may overlap the next periodic expiration. */
    if (InterlockedCompareExchange(&a->PollActive, 1, 0) != 0) return;
    NdisAcquireSpinLock(&a->Lock);
    if (!a->Running || a->Shutdown) goto done;
    if (!a->Removed && a->Power == NdisDeviceStateD0 && !a->Faulted) {
        if (JwRead(a, JW_REG_MAGIC) != JW_MAGIC || JwRead(a, JW_REG_VERSION) != JW_VERSION)
            a->Faulted = TRUE;
        else {
            a->Phase = JwRead(a, JW_REG_PHASE);
            a->Error = JwRead(a, JW_REG_ERROR);
            a->BridgeStatus = status = JwRead(a, JW_REG_STATUS);
            a->CommandStatus = JwRead(a, JW_REG_COMMAND_STATUS);
            if (a->JoinInFlight && JwRead(a, JW_REG_COMMAND) == 0) {
                UCHAR zero[64] = {0};
                WRITE_REGISTER_BUFFER_UCHAR(a->Registers + JW_CONFIG_PSK, zero, sizeof(zero));
                WRITE_REGISTER_BUFFER_UCHAR(a->Registers + JW_CONFIG_SSID, zero, sizeof(a->Ssid));
                JwWrite(a, JW_CONFIG_PSK_LEN, 0);
                JwWrite(a, JW_CONFIG_SSID_LEN, 0);
                a->JoinInFlight = FALSE;
            }
            /* Unready firmware may reset its rings. Inspect ring invariants
             * only once READY promises that the peer has started its rings.
             */
            if ((status & JW_STATUS_READY) && !a->RingsStarted) {
                a->TxProducer = JwRead(a, JW_REG_TX_PROD);
                a->RxConsumer = JwRead(a, JW_REG_RX_CONS);
                a->RingsStarted = TRUE;
            }
            if ((status & JW_STATUS_READY) && JwCounters(a, &txConsumer, &rxProducer)) {
                if (a->JoinPending && !a->JoinInFlight && JwRead(a, JW_REG_COMMAND) == 0) JwJoin(a);
                up = a->MacValid && (status & JW_STATUS_LINK) != 0;
            }
        }
    }
    indicate = !a->LinkIndicated || a->LinkUp != up;
    a->LinkUp = up;
    a->LinkIndicated = TRUE;
    if (!up) JwDiscardQueue(a);
    if (indicate) {
        NdisReleaseSpinLock(&a->Lock);
        JwIndicateLink(a, up);
        NdisAcquireSpinLock(&a->Lock);
    }
    if (!a->Running || a->Removed || a->Shutdown || a->Power != NdisDeviceStateD0 || !up) goto done;
    /* At most 64 publications per expiration; no host progress is required. */
    for (i = 0; i < JW_RING_COUNT && a->QueueCount &&
         (ULONG)(a->TxProducer - txConsumer) < JW_RING_COUNT && !(i && JwOverBudget(a, start)); ++i) {
        JW_FRAME *frame = &a->Queue[a->QueueHead];
        ULONG slot = JW_TX_BASE + (a->TxProducer % JW_RING_COUNT) * JW_SLOT_SIZE;
        /* Length and frame land together; the host reads them only after TX_PROD. */
        JwSlotWrite(a, slot, frame->Data, frame->Length);
        KeMemoryBarrier();
        JwWrite(a, JW_REG_TX_PROD, ++a->TxProducer);
        JwAccount(a, frame->Data, frame->Length, FALSE);
        a->QueueHead = (a->QueueHead + 1) % JW_RING_COUNT;
        --a->QueueCount;
    }
    for (i = 0; i < JW_RING_COUNT && a->RxConsumer != rxProducer; ++i) {
        ULONG slot, length;
        BOOLEAN accepted = FALSE;
        if (!a->Running || a->Removed || a->Shutdown || a->Power != NdisDeviceStateD0) break;
        if (i && JwOverBudget(a, start)) break;
        slot = JW_RX_BASE + (a->RxConsumer % JW_RING_COUNT) * JW_SLOT_SIZE;
        length = JwRead(a, slot);
        if (length >= 14 && length <= JW_FRAME_MAX) {
            JwSlotRead(a, slot, length);
            /* The length re-read with the frame must match the validated one. */
            if (*(ULONG UNALIGNED *)a->Staging != length) length = 0; /* counted below */
            else RtlCopyMemory(a->ReceiveData, (PUCHAR)a->Staging + sizeof(ULONG), length);
        }
        if (length >= 14 && length <= JW_FRAME_MAX) {
            accepted = JwAccept(a, a->ReceiveData);
            if (accepted) JwAccount(a, a->ReceiveData, length, TRUE);
        } else ++a->Stats.ifInErrors;
        /* Copy (or reject) before releasing the slot to the host. */
        KeMemoryBarrier();
        JwWrite(a, JW_REG_RX_CONS, ++a->RxConsumer);
        if (!accepted) { ++a->Stats.ifInDiscards; continue; }
        NET_BUFFER_DATA_LENGTH(NET_BUFFER_LIST_FIRST_NB(a->ReceiveNbl)) = length;
        NET_BUFFER_LIST_NEXT_NBL(a->ReceiveNbl) = NULL;
        NET_BUFFER_LIST_STATUS(a->ReceiveNbl) = NDIS_STATUS_SUCCESS;
        NdisReleaseSpinLock(&a->Lock);
        NdisMIndicateReceiveNetBufferLists(a->Handle, a->ReceiveNbl, NDIS_DEFAULT_PORT_NUMBER, 1,
            NDIS_RECEIVE_FLAGS_DISPATCH_LEVEL | NDIS_RECEIVE_FLAGS_RESOURCES);
        NdisAcquireSpinLock(&a->Lock);
    }
done:
    NdisReleaseSpinLock(&a->Lock);
    InterlockedExchange(&a->PollActive, 0);
}

static VOID JwSend(NDIS_HANDLE context, PNET_BUFFER_LIST lists, NDIS_PORT_NUMBER port, ULONG flags)
{
    JW_ADAPTER *a = context;
    PNET_BUFFER_LIST nbl, next;
    BOOLEAN active;
    ULONG completionFlags = (flags & NDIS_SEND_FLAGS_DISPATCH_LEVEL) ? NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL : 0;
    NdisAcquireSpinLock(&a->Lock);
    active = a->Running && !a->Shutdown && !a->Removed && !a->Faulted && a->Power == NdisDeviceStateD0;
    if (active && ++a->ActiveSends == 1) KeClearEvent(&a->SendIdle);
    NdisReleaseSpinLock(&a->Lock);
    for (nbl = lists; nbl; nbl = next) {
        PNET_BUFFER nb;
        ULONG count = 0, index;
        NDIS_STATUS result = NDIS_STATUS_SUCCESS;
        next = NET_BUFFER_LIST_NEXT_NBL(nbl);
        NET_BUFFER_LIST_NEXT_NBL(nbl) = NULL;
        NdisAcquireSpinLock(&a->Lock);
        if (a->Removed || a->Shutdown || a->Faulted) result = NDIS_STATUS_ADAPTER_NOT_READY;
        else if (a->Power != NdisDeviceStateD0) result = NDIS_STATUS_LOW_POWER_STATE;
        else if (!active || !a->Running) result = NDIS_STATUS_PAUSED;
        else if (!a->LinkUp) result = NDIS_STATUS_MEDIA_DISCONNECTED;
        else if (port != NDIS_DEFAULT_PORT_NUMBER ||
                 NET_BUFFER_LIST_INFO(nbl, Ieee8021QNetBufferListInfo) != NULL)
            result = NDIS_STATUS_NOT_SUPPORTED;
        if (result == NDIS_STATUS_SUCCESS) {
            /* Reserve the entire NBL; never partially enqueue a multi-NB send. */
            for (nb = NET_BUFFER_LIST_FIRST_NB(nbl); nb; nb = NET_BUFFER_NEXT_NB(nb)) {
                if (++count > JW_RING_COUNT || NET_BUFFER_DATA_LENGTH(nb) < 14 ||
                    NET_BUFFER_DATA_LENGTH(nb) > JW_FRAME_MAX) { result = NDIS_STATUS_INVALID_LENGTH; break; }
            }
            if (!count) result = NDIS_STATUS_INVALID_DATA;
            if (result == NDIS_STATUS_SUCCESS && count > JW_RING_COUNT - a->QueueCount)
                result = NDIS_STATUS_RESOURCES;
        }
        index = (a->QueueHead + a->QueueCount) % JW_RING_COUNT;
        if (result == NDIS_STATUS_SUCCESS) {
            for (nb = NET_BUFFER_LIST_FIRST_NB(nbl); nb; nb = NET_BUFFER_NEXT_NB(nb)) {
                JW_FRAME *frame = &a->Queue[index];
                ULONG length = NET_BUFFER_DATA_LENGTH(nb);
                PUCHAR data = NdisGetDataBuffer(nb, length, frame->Data, 1, 0);
                if (!data) { result = NDIS_STATUS_RESOURCES; break; }
                if (data != frame->Data) NdisMoveMemory(frame->Data, data, length);
                frame->Length = length < 60 ? 60 : length;
                if (length < 60) NdisZeroMemory(frame->Data + length, 60 - length);
                index = (index + 1) % JW_RING_COUNT;
            }
            if (result == NDIS_STATUS_SUCCESS) a->QueueCount += count;
        }
        if (result != NDIS_STATUS_SUCCESS) {
            ++a->Stats.ifOutDiscards;
            if (result == NDIS_STATUS_INVALID_LENGTH || result == NDIS_STATUS_INVALID_DATA)
                ++a->Stats.ifOutErrors;
        }
        NdisReleaseSpinLock(&a->Lock);
        /* SUCCESS only after an independent bounded copy of every NB exists. */
        NET_BUFFER_LIST_STATUS(nbl) = result;
        NdisMSendNetBufferListsComplete(a->Handle, nbl, completionFlags);
    }
    if (active) {
        NdisAcquireSpinLock(&a->Lock);
        if (--a->ActiveSends == 0) KeSetEvent(&a->SendIdle, IO_NO_INCREMENT, FALSE);
        NdisReleaseSpinLock(&a->Lock);
    }
}

/* PASSIVE_LEVEL only. NDIS periodic timer cancellation flushes queued DPCs. */
static VOID JwStop(JW_ADAPTER *a)
{
    NdisAcquireSpinLock(&a->Lock);
    a->Running = FALSE;
    NdisReleaseSpinLock(&a->Lock);
    if (a->Timer) (VOID)NdisCancelTimerObject(a->Timer);
    (VOID)KeWaitForSingleObject(&a->SendIdle, Executive, KernelMode, FALSE, NULL);
    NdisAcquireSpinLock(&a->Lock);
    JwDiscardQueue(a);
    NdisReleaseSpinLock(&a->Lock);
}

static NDIS_STATUS JwPause(NDIS_HANDLE context, PNDIS_MINIPORT_PAUSE_PARAMETERS parameters)
{
    UNREFERENCED_PARAMETER(parameters);
    JwStop(context);
    return NDIS_STATUS_SUCCESS;
}

static NDIS_STATUS JwRestart(NDIS_HANDLE context, PNDIS_MINIPORT_RESTART_PARAMETERS parameters)
{
    JW_ADAPTER *a = context;
    LARGE_INTEGER due;
    UNREFERENCED_PARAMETER(parameters);
    NdisAcquireSpinLock(&a->Lock);
    if (a->Removed || a->Shutdown || a->Faulted) {
        NdisReleaseSpinLock(&a->Lock);
        return NDIS_STATUS_ADAPTER_NOT_READY;
    }
    a->Running = TRUE;
    a->LinkIndicated = FALSE;
    NdisReleaseSpinLock(&a->Lock);
    due.QuadPart = -50000; /* 5ms, periodic (required for draining cancellation). */
    (VOID)NdisSetTimerObject(a->Timer, due, 5, NULL);
    return NDIS_STATUS_SUCCESS;
}

/* Strict UTF-16 -> UTF-8; SSIDs are at most 32 octets, never NUL-terminated ABI. */
static BOOLEAN JwSsid(const NDIS_STRING *string, UCHAR out[32], ULONG *length)
{
    ULONG i, used = 0;
    if ((string->Length & 1) || !string->Buffer) return FALSE;
    for (i = 0; i < string->Length / sizeof(WCHAR); ++i) {
        ULONG code = string->Buffer[i], bytes, j;
        if (!code) return FALSE;
        if (code >= 0xd800 && code <= 0xdbff) {
            ULONG low;
            if (++i >= string->Length / sizeof(WCHAR)) return FALSE;
            low = string->Buffer[i];
            if (low < 0xdc00 || low > 0xdfff) return FALSE;
            code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
        } else if (code >= 0xdc00 && code <= 0xdfff) return FALSE;
        bytes = code < 0x80 ? 1 : code < 0x800 ? 2 : code < 0x10000 ? 3 : 4;
        if (bytes > 32 - used) return FALSE;
        if (bytes == 1) out[used] = (UCHAR)code;
        else {
            for (j = bytes - 1; j; --j) { out[used + j] = (UCHAR)(0x80 | (code & 0x3f)); code >>= 6; }
            out[used] = (UCHAR)((bytes == 2 ? 0xc0 : bytes == 3 ? 0xe0 : 0xf0) | code);
        }
        used += bytes;
    }
    *length = used;
    return used != 0;
}

static NDIS_STATUS JwConfiguration(JW_ADAPTER *a)
{
    NDIS_CONFIGURATION_OBJECT object;
    NDIS_HANDLE config;
    PNDIS_CONFIGURATION_PARAMETER value;
    NDIS_STRING ssid = RTL_CONSTANT_STRING(L"SSID");
    NDIS_STRING psk = RTL_CONSTANT_STRING(L"PSK");
    NDIS_STATUS result, readStatus;
    ULONG i, length;
    NdisZeroMemory(&object, sizeof(object));
    object.Header.Type = NDIS_OBJECT_TYPE_CONFIGURATION_OBJECT;
    object.Header.Revision = NDIS_CONFIGURATION_OBJECT_REVISION_1;
    object.Header.Size = NDIS_SIZEOF_CONFIGURATION_OBJECT_REVISION_1;
    object.NdisHandle = a->Handle;
    result = NdisOpenConfigurationEx(&object, &config);
    if (result != NDIS_STATUS_SUCCESS) return result;
    NdisReadConfiguration(&readStatus, &value, config, &ssid, NdisParameterString);
    result = NDIS_STATUS_SUCCESS;
    if (readStatus != NDIS_STATUS_SUCCESS) goto close; /* No JOIN requested. */
    if (!JwSsid(&value->ParameterData.StringData, a->Ssid, &a->SsidLength)) goto invalid;
    NdisReadConfiguration(&readStatus, &value, config, &psk, NdisParameterString);
    if (readStatus != NDIS_STATUS_SUCCESS) {
        RtlSecureZeroMemory(a->Ssid, sizeof(a->Ssid));
        a->SsidLength = 0;
        goto close; /* Temporary PSK absent: no JOIN. */
    }
    {
        const NDIS_STRING *string = &value->ParameterData.StringData;
        if ((string->Length & 1) || (!string->Buffer && string->Length)) goto invalid;
        length = string->Length / sizeof(WCHAR);
        if (length < 8 || length > 63) goto invalid;
        for (i = 0; i < length; ++i) {
            WCHAR c = string->Buffer[i];
            if (c < 0x20 || c > 0x7e) goto invalid;
            a->Psk[i] = (UCHAR)c;
        }
        a->PskLength = length;
    }
    a->JoinPending = TRUE;
    goto close;
invalid:
    result = NDIS_STATUS_INVALID_DATA;
close:
    NdisCloseConfiguration(config);
    return result;
}

static VOID JwFree(JW_ADAPTER *a)
{
    if (a->Timer) NdisFreeTimerObject(a->Timer);
    if (a->ReceiveNbl) NdisFreeNetBufferList(a->ReceiveNbl);
    if (a->ReceiveMdl) NdisFreeMdl(a->ReceiveMdl);
    if (a->ReceivePool) NdisFreeNetBufferListPool(a->ReceivePool);
    if (a->Registers) NdisMUnmapIoSpace(a->Handle, a->Registers, JW_SIZE);
    NdisFreeSpinLock(&a->Lock);
    RtlSecureZeroMemory(a, sizeof(*a));
    NdisFreeMemory(a, 0, 0);
}

static NDIS_STATUS JwInitialize(NDIS_HANDLE handle, NDIS_HANDLE driverContext,
                              PNDIS_MINIPORT_INIT_PARAMETERS parameters)
{
    JW_ADAPTER *a;
    NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES registration;
    NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES general;
    NDIS_PM_CAPABILITIES power;
    NET_BUFFER_LIST_POOL_PARAMETERS pool;
    NDIS_TIMER_CHARACTERISTICS timer;
    NDIS_PHYSICAL_ADDRESS address;
    NDIS_STATUS result;
    ULONG i, low, high, txConsumer, rxProducer;
    BOOLEAN resource = FALSE, nonzero = FALSE;
    UNREFERENCED_PARAMETER(driverContext);
    if (parameters->AllocatedResources)
        for (i = 0; i < parameters->AllocatedResources->Count; ++i) {
            PCM_PARTIAL_RESOURCE_DESCRIPTOR r = &parameters->AllocatedResources->PartialDescriptors[i];
            if (r->Type == CmResourceTypeMemory && (ULONGLONG)r->u.Memory.Start.QuadPart == JW_BASE &&
                r->u.Memory.Length >= JW_SIZE) resource = TRUE;
        }
    if (!resource) return NDIS_STATUS_RESOURCE_CONFLICT;
    a = NdisAllocateMemoryWithTagPriority(handle, sizeof(*a), JW_TAG, NormalPoolPriority);
    if (!a) return NDIS_STATUS_RESOURCES;
    NdisZeroMemory(a, sizeof(*a));
    a->Handle = handle;
    a->Power = NdisDeviceStateD0;
    a->Lookahead = JW_MTU;
    NdisAllocateSpinLock(&a->Lock);
    KeInitializeEvent(&a->SendIdle, NotificationEvent, TRUE);
    a->Stats.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    a->Stats.Header.Revision = NDIS_STATISTICS_INFO_REVISION_1;
    a->Stats.Header.Size = NDIS_SIZEOF_STATISTICS_INFO_REVISION_1;
    a->Stats.SupportedStatistics = JW_STATS_VALID;
    NdisZeroMemory(&registration, sizeof(registration));
    registration.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES;
    registration.Header.Revision = NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1;
    registration.Header.Size = NDIS_SIZEOF_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1;
    registration.MiniportAdapterContext = a;
    registration.AttributeFlags = NDIS_MINIPORT_ATTRIBUTES_HARDWARE_DEVICE;
    registration.CheckForHangTimeInSeconds = 0;
    registration.InterfaceType = NdisInterfaceInternal;
    result = NdisMSetMiniportAttributes(handle, (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&registration);
    if (result != NDIS_STATUS_SUCCESS) goto fail;
    address.QuadPart = JW_BASE;
    (VOID)KeQueryPerformanceCounter(&a->PerfFrequency);
    result = NdisMMapIoSpace((PVOID *)&a->Registers, handle, address, JW_SIZE);
    if (result != NDIS_STATUS_SUCCESS) goto fail;
    if (JwRead(a, JW_REG_MAGIC) != JW_MAGIC || JwRead(a, JW_REG_VERSION) != JW_VERSION) {
        result = NDIS_STATUS_ADAPTER_NOT_FOUND; goto fail;
    }
    a->TxProducer = JwRead(a, JW_REG_TX_PROD);
    a->RxConsumer = JwRead(a, JW_REG_RX_CONS);
    a->BridgeStatus = JwRead(a, JW_REG_STATUS);
    a->RingsStarted = (a->BridgeStatus & JW_STATUS_READY) != 0;
    a->Phase = JwRead(a, JW_REG_PHASE);
    a->Error = JwRead(a, JW_REG_ERROR);
    a->CommandStatus = JwRead(a, JW_REG_COMMAND_STATUS);
    if ((a->BridgeStatus & JW_STATUS_READY) && !JwCounters(a, &txConsumer, &rxProducer)) {
        result = NDIS_STATUS_INVALID_DATA; goto fail;
    }
    low = JwRead(a, JW_REG_MAC_LOW);
    high = JwRead(a, JW_REG_MAC_HIGH);
    for (i = 0; i < 4; ++i) a->Mac[i] = (UCHAR)(low >> (i * 8));
    a->Mac[4] = (UCHAR)high; a->Mac[5] = (UCHAR)(high >> 8);
    for (i = 0; i < 6; ++i) if (a->Mac[i]) nonzero = TRUE;
    a->MacValid = nonzero && !(a->Mac[0] & 1);
    if (!a->MacValid) {
        /* Diagnostic-only identity; never send using a guessed radio MAC.
         * Reinitialize the adapter after host firmware initializes its MAC.
         */
        const UCHAR diagnosticMac[6] = {0x02, 0x4a, 0x57, 0x03, 0x16, 0x00};
        NdisMoveMemory(a->Mac, diagnosticMac, sizeof(a->Mac));
    }
    NdisZeroMemory(&pool, sizeof(pool));
    pool.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    pool.Header.Revision = NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
    pool.Header.Size = NDIS_SIZEOF_NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
    pool.ProtocolId = NDIS_PROTOCOL_ID_DEFAULT;
    pool.fAllocateNetBuffer = TRUE;
    pool.PoolTag = JW_TAG;
    a->ReceivePool = NdisAllocateNetBufferListPool(handle, &pool);
    if (!a->ReceivePool) { result = NDIS_STATUS_RESOURCES; goto fail; }
    a->ReceiveMdl = NdisAllocateMdl(handle, a->ReceiveData, sizeof(a->ReceiveData));
    if (!a->ReceiveMdl) { result = NDIS_STATUS_RESOURCES; goto fail; }
    a->ReceiveNbl = NdisAllocateNetBufferAndNetBufferList(a->ReceivePool, 0, 0,
                                                       a->ReceiveMdl, 0, 0);
    if (!a->ReceiveNbl) { result = NDIS_STATUS_RESOURCES; goto fail; }
    a->ReceiveNbl->SourceHandle = handle;
    NdisZeroMemory(&timer, sizeof(timer));
    timer.Header.Type = NDIS_OBJECT_TYPE_TIMER_CHARACTERISTICS;
    timer.Header.Revision = NDIS_TIMER_CHARACTERISTICS_REVISION_1;
    timer.Header.Size = NDIS_SIZEOF_TIMER_CHARACTERISTICS_REVISION_1;
    timer.AllocationTag = JW_TAG;
    timer.TimerFunction = JwPoll;
    timer.FunctionContext = a;
    result = NdisAllocateTimerObject(handle, &timer, &a->Timer);
    if (result != NDIS_STATUS_SUCCESS) goto fail;
    result = JwConfiguration(a);
    if (result != NDIS_STATUS_SUCCESS) goto fail;
    NdisZeroMemory(&general, sizeof(general));
    general.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES;
    general.Header.Revision = NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_2;
    general.Header.Size = NDIS_SIZEOF_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_2;
    NdisZeroMemory(&power, sizeof(power));
    power.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    power.Header.Revision = NDIS_PM_CAPABILITIES_REVISION_2;
    power.Header.Size = NDIS_SIZEOF_NDIS_PM_CAPABILITIES_REVISION_2;
    general.PowerManagementCapabilitiesEx = &power;
    general.MediaType = NdisMedium802_3;
    general.PhysicalMediumType = NdisPhysicalMediumUnspecified;
    general.MtuSize = JW_MTU;
    general.MaxXmitLinkSpeed = general.MaxRcvLinkSpeed = JW_SPEED;
    general.XmitLinkSpeed = general.RcvLinkSpeed = 0;
    general.MediaConnectState = MediaConnectStateDisconnected;
    general.MediaDuplexState = MediaDuplexStateFull;
    general.LookaheadSize = JW_MTU;
    general.MacOptions = NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA | NDIS_MAC_OPTION_TRANSFERS_NOT_PEND |
        NDIS_MAC_OPTION_NO_LOOPBACK;
    general.SupportedPacketFilters = JW_FILTERS;
    general.MaxMulticastListSize = JW_MULTICAST_MAX;
    general.MacAddressLength = 6;
    NdisMoveMemory(general.PermanentMacAddress, a->Mac, 6);
    NdisMoveMemory(general.CurrentMacAddress, a->Mac, 6);
    general.AccessType = NET_IF_ACCESS_BROADCAST;
    general.DirectionType = NET_IF_DIRECTION_SENDRECEIVE;
    general.ConnectionType = NET_IF_CONNECTION_DEDICATED;
    general.IfType = IF_TYPE_ETHERNET_CSMACD;
    general.IfConnectorPresent = FALSE;
    general.SupportedStatistics = JW_STATS_SUPPORTED;
    general.SupportedPauseFunctions = NdisPauseFunctionsUnsupported;
    general.SupportedOidList = JwOids;
    general.SupportedOidListLength = sizeof(JwOids);
    result = NdisMSetMiniportAttributes(handle, (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&general);
    if (result == NDIS_STATUS_SUCCESS) return result; /* Initially paused. */
fail:
    JwFree(a);
    return result;
}

static VOID JwHalt(NDIS_HANDLE context, NDIS_HALT_ACTION action)
{
    JW_ADAPTER *a = context;
    UNREFERENCED_PARAMETER(action);
    JwStop(a);
    JwFree(a);
}

static NDIS_STATUS JwQuery(JW_ADAPTER *a, PNDIS_OID_REQUEST request)
{
    ULONG value = 0, length = sizeof(value);
    ULONG64 large = 0;
    USHORT small = 0x061e; /* NDIS 6.30 (minor is decimal). */
    NDIS_STATISTICS_INFO stats;
    NDIS_PNP_CAPABILITIES power;
    NDIS_INTERRUPT_MODERATION_PARAMETERS moderation;
    const CHAR vendor[] = "J316 synthetic internal Wi-Fi Ethernet bridge";
    const VOID *data = &value;
    NDIS_OID oid = request->DATA.QUERY_INFORMATION.Oid;
    NdisZeroMemory(&power, sizeof(power));
    switch (oid) {
    case OID_GEN_SUPPORTED_LIST: data = JwOids; length = sizeof(JwOids); break;
    case OID_GEN_HARDWARE_STATUS: value = a->Faulted || !a->MacValid || !(a->BridgeStatus & JW_STATUS_READY) ?
        NdisHardwareStatusNotReady : NdisHardwareStatusReady; break;
    case OID_JW_PHASE: value = a->Phase; break;
    case OID_JW_ERROR: value = a->Error; break;
    case OID_JW_STATUS: value = a->BridgeStatus; break;
    case OID_JW_COMMAND_STATUS: value = a->CommandStatus; break;
    case OID_GEN_MEDIA_SUPPORTED: case OID_GEN_MEDIA_IN_USE: value = NdisMedium802_3; break;
    case OID_GEN_PHYSICAL_MEDIUM: value = NdisPhysicalMediumUnspecified; break;
    case OID_GEN_MAXIMUM_LOOKAHEAD: case OID_GEN_MAXIMUM_FRAME_SIZE: value = JW_MTU; break;
    case OID_GEN_CURRENT_LOOKAHEAD: value = a->Lookahead; break;
    case OID_GEN_LINK_SPEED: value = a->LinkUp ? (ULONG)(JW_SPEED / 100) : 0; break;
    case OID_GEN_TRANSMIT_BUFFER_SPACE: case OID_GEN_RECEIVE_BUFFER_SPACE: value = JW_RING_COUNT * JW_FRAME_MAX; break;
    case OID_GEN_TRANSMIT_BLOCK_SIZE: case OID_GEN_RECEIVE_BLOCK_SIZE:
    case OID_GEN_MAXIMUM_TOTAL_SIZE: value = JW_FRAME_MAX; break;
    case OID_GEN_VENDOR_ID: value = 0xffffff; break;
    case OID_GEN_VENDOR_DESCRIPTION: data = vendor; length = sizeof(vendor); break;
    case OID_GEN_CURRENT_PACKET_FILTER: value = a->PacketFilter; break;
    case OID_GEN_DRIVER_VERSION: data = &small; length = sizeof(small); break;
    case OID_GEN_VENDOR_DRIVER_VERSION: value = 0x00010000; break;
    case OID_GEN_MAC_OPTIONS: value = NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA |
        NDIS_MAC_OPTION_TRANSFERS_NOT_PEND | NDIS_MAC_OPTION_NO_LOOPBACK; break;
    case OID_GEN_MEDIA_CONNECT_STATUS: value = a->LinkUp ? NdisMediaStateConnected : NdisMediaStateDisconnected; break;
    case OID_GEN_MAXIMUM_SEND_PACKETS: value = JW_RING_COUNT; break;
    case OID_802_3_PERMANENT_ADDRESS: case OID_802_3_CURRENT_ADDRESS: data = a->Mac; length = 6; break;
    case OID_802_3_MAXIMUM_LIST_SIZE: value = JW_MULTICAST_MAX; break;
    case OID_802_3_MULTICAST_LIST: data = a->Multicast; length = a->MulticastCount * 6; break;
    case OID_GEN_STATISTICS: stats = a->Stats; data = &stats; length = sizeof(stats); break;
    case OID_GEN_XMIT_OK: large = a->Stats.ifHCOutUcastPkts + a->Stats.ifHCOutMulticastPkts + a->Stats.ifHCOutBroadcastPkts; goto counter;
    case OID_GEN_RCV_OK: large = a->Stats.ifHCInUcastPkts + a->Stats.ifHCInMulticastPkts + a->Stats.ifHCInBroadcastPkts; goto counter;
    case OID_GEN_XMIT_ERROR: large = a->Stats.ifOutErrors; goto counter;
    case OID_GEN_RCV_ERROR: large = a->Stats.ifInErrors; goto counter;
    case OID_GEN_RCV_NO_BUFFER: large = 0; goto counter;
    case OID_802_3_RCV_ERROR_ALIGNMENT: case OID_802_3_XMIT_ONE_COLLISION:
    case OID_802_3_XMIT_MORE_COLLISIONS: large = 0;
counter:
        /* NDIS statistics counters allow a legacy four-byte query. */
        if (request->DATA.QUERY_INFORMATION.InformationBufferLength == sizeof(ULONG)) value = (ULONG)large;
        else { data = &large; length = sizeof(large); }
        break;
    case OID_PNP_CAPABILITIES: data = &power; length = sizeof(power); break;
    case OID_GEN_INTERRUPT_MODERATION:
        NdisZeroMemory(&moderation, sizeof(moderation));
        moderation.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
        moderation.Header.Revision = NDIS_INTERRUPT_MODERATION_PARAMETERS_REVISION_1;
        moderation.Header.Size = NDIS_SIZEOF_INTERRUPT_MODERATION_PARAMETERS_REVISION_1;
        moderation.InterruptModeration = NdisInterruptModerationNotSupported;
        data = &moderation; length = sizeof(moderation); break;
    case OID_PNP_QUERY_POWER:
        length = sizeof(NDIS_DEVICE_POWER_STATE);
        if (request->DATA.QUERY_INFORMATION.InformationBufferLength < length) goto shortBuffer;
        NdisMoveMemory(&value, request->DATA.QUERY_INFORMATION.InformationBuffer, length);
        if (value < NdisDeviceStateD0 || value > NdisDeviceStateD3) return NDIS_STATUS_INVALID_DATA;
        request->DATA.QUERY_INFORMATION.BytesWritten = 0;
        return NDIS_STATUS_SUCCESS;
    default: return NDIS_STATUS_NOT_SUPPORTED;
    }
    if (request->DATA.QUERY_INFORMATION.InformationBufferLength < length) goto shortBuffer;
    if (length) NdisMoveMemory(request->DATA.QUERY_INFORMATION.InformationBuffer, data, length);
    request->DATA.QUERY_INFORMATION.BytesWritten = length;
    return NDIS_STATUS_SUCCESS;
shortBuffer:
    request->DATA.QUERY_INFORMATION.BytesNeeded = length;
    return NDIS_STATUS_BUFFER_TOO_SHORT;
}

static NDIS_STATUS JwSet(JW_ADAPTER *a, PNDIS_OID_REQUEST request)
{
    ULONG length = request->DATA.SET_INFORMATION.InformationBufferLength;
    PVOID data = request->DATA.SET_INFORMATION.InformationBuffer;
    ULONG value, i;
    NDIS_OID oid = request->DATA.SET_INFORMATION.Oid;
    if (oid == OID_GEN_INTERRUPT_MODERATION) return NDIS_STATUS_INVALID_DATA;
    if (oid == OID_802_3_MULTICAST_LIST) {
        if (length % 6) { request->DATA.SET_INFORMATION.BytesNeeded = (length / 6 + 1) * 6; return NDIS_STATUS_INVALID_LENGTH; }
        if (length > sizeof(a->Multicast)) { request->DATA.SET_INFORMATION.BytesNeeded = sizeof(a->Multicast); return NDIS_STATUS_MULTICAST_FULL; }
        for (i = 0; i < length; i += 6) if (!(((PUCHAR)data)[i] & 1)) return NDIS_STATUS_INVALID_DATA;
        if (length) NdisMoveMemory(a->Multicast, data, length);
        a->MulticastCount = length / 6;
        request->DATA.SET_INFORMATION.BytesRead = length;
        return NDIS_STATUS_SUCCESS;
    }
    if (oid != OID_GEN_CURRENT_PACKET_FILTER && oid != OID_GEN_CURRENT_LOOKAHEAD && oid != OID_PNP_SET_POWER)
        return NDIS_STATUS_NOT_SUPPORTED;
    if (length != sizeof(value)) { request->DATA.SET_INFORMATION.BytesNeeded = sizeof(value); return NDIS_STATUS_INVALID_LENGTH; }
    NdisMoveMemory(&value, data, sizeof(value));
    switch (oid) {
    case OID_GEN_CURRENT_PACKET_FILTER:
        if (value & ~JW_FILTERS) return NDIS_STATUS_NOT_SUPPORTED;
        a->PacketFilter = value; break;
    case OID_GEN_CURRENT_LOOKAHEAD:
        if (value > JW_MTU) return NDIS_STATUS_INVALID_DATA;
        a->Lookahead = value; break;
    case OID_PNP_SET_POWER:
        if (value < NdisDeviceStateD0 || value > NdisDeviceStateD3) return NDIS_STATUS_INVALID_DATA;
        a->Power = (NDIS_DEVICE_POWER_STATE)value;
        if (a->Power != NdisDeviceStateD0) { a->LinkUp = FALSE; a->LinkIndicated = FALSE; JwDiscardQueue(a); }
        break;
    default: return NDIS_STATUS_NOT_SUPPORTED;
    }
    request->DATA.SET_INFORMATION.BytesRead = sizeof(value);
    return NDIS_STATUS_SUCCESS;
}

static NDIS_STATUS JwOid(NDIS_HANDLE context, PNDIS_OID_REQUEST request)
{
    JW_ADAPTER *a = context;
    NDIS_STATUS result;
    BOOLEAN powerRequest = request->RequestType == NdisRequestSetInformation &&
        request->DATA.SET_INFORMATION.Oid == OID_PNP_SET_POWER;
    NDIS_DEVICE_POWER_STATE power;
    BOOLEAN running;
    NdisAcquireSpinLock(&a->Lock);
    switch (request->RequestType) {
    case NdisRequestQueryInformation: case NdisRequestQueryStatistics:
        request->DATA.QUERY_INFORMATION.BytesWritten = request->DATA.QUERY_INFORMATION.BytesNeeded = 0;
        result = JwQuery(a, request); break;
    case NdisRequestSetInformation:
        request->DATA.SET_INFORMATION.BytesRead = request->DATA.SET_INFORMATION.BytesNeeded = 0;
        result = JwSet(a, request); break;
    default: result = NDIS_STATUS_NOT_SUPPORTED; break;
    }
    power = a->Power;
    running = a->Running;
    NdisReleaseSpinLock(&a->Lock);
    /* Regular OIDs run at PASSIVE_LEVEL. Drain even if power is requested
     * while Running: no receive indication or accepted send outlives D3.
     * Neither the lock nor an NDIS callback is held across cancellation.
     */
    if (powerRequest && result == NDIS_STATUS_SUCCESS) {
        if (power != NdisDeviceStateD0) {
            (VOID)NdisCancelTimerObject(a->Timer);
            (VOID)KeWaitForSingleObject(&a->SendIdle, Executive, KernelMode, FALSE, NULL);
        } else if (running) {
            LARGE_INTEGER due;
            due.QuadPart = -50000;
            (VOID)NdisSetTimerObject(a->Timer, due, 5, NULL);
        }
    }
    return result;
}

static VOID JwReturn(NDIS_HANDLE context, PNET_BUFFER_LIST lists, ULONG flags)
{
    /* All receives use RESOURCES; NDIS must not return those NBLs later. */
    UNREFERENCED_PARAMETER(context); UNREFERENCED_PARAMETER(lists); UNREFERENCED_PARAMETER(flags);
}

static VOID JwCancelSend(NDIS_HANDLE context, PVOID cancelId)
{
    /* No NDIS send is retained: private copies belong to already-completed sends. */
    UNREFERENCED_PARAMETER(context); UNREFERENCED_PARAMETER(cancelId);
}

static VOID JwCancelOid(NDIS_HANDLE context, PVOID requestId)
{
    UNREFERENCED_PARAMETER(context); UNREFERENCED_PARAMETER(requestId); /* All OIDs synchronous. */
}

static BOOLEAN JwCheckHang(NDIS_HANDLE context)
{
    UNREFERENCED_PARAMETER(context);
    return FALSE; /* Host recovery belongs to m1n1, not an NDIS reset loop. */
}

static NDIS_STATUS JwReset(NDIS_HANDLE context, PBOOLEAN addressingReset)
{
    JW_ADAPTER *a = context;
    NDIS_STATUS result;
    *addressingReset = FALSE;
    NdisAcquireSpinLock(&a->Lock);
    JwDiscardQueue(a);
    result = (a->Faulted || a->Removed) ? NDIS_STATUS_HARD_ERRORS : NDIS_STATUS_SUCCESS;
    NdisReleaseSpinLock(&a->Lock);
    return result;
}

static VOID JwPnp(NDIS_HANDLE context, PNET_DEVICE_PNP_EVENT event)
{
    JW_ADAPTER *a = context;
    if (event->DevicePnPEvent == NdisDevicePnPEventSurpriseRemoved) {
        NdisAcquireSpinLock(&a->Lock);
        a->Removed = TRUE;
        a->LinkUp = FALSE;
        a->LinkIndicated = FALSE;
        JwDiscardQueue(a);
        NdisReleaseSpinLock(&a->Lock);
    }
}

static VOID JwShutdown(NDIS_HANDLE context, NDIS_SHUTDOWN_ACTION action)
{
    JW_ADAPTER *a = context;
    UNREFERENCED_PARAMETER(action);
    /* May run at DIRQL/bugcheck: no waits, locks or register accesses. */
    InterlockedExchange(&a->Shutdown, 1);
}

static VOID JwUnload(PDRIVER_OBJECT driver)
{
    UNREFERENCED_PARAMETER(driver);
    NdisMDeregisterMiniportDriver(JwDriver);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registryPath)
{
    NDIS_MINIPORT_DRIVER_CHARACTERISTICS c;
    NdisZeroMemory(&c, sizeof(c));
    c.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_DRIVER_CHARACTERISTICS;
    c.Header.Revision = NDIS_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_2;
    c.Header.Size = NDIS_SIZEOF_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_2;
    c.MajorNdisVersion = 6;
    c.MinorNdisVersion = 30; /* ARM64 headers require NDIS >= 6.30. */
    c.MajorDriverVersion = 1;
    c.InitializeHandlerEx = JwInitialize;
    c.HaltHandlerEx = JwHalt;
    c.UnloadHandler = JwUnload;
    c.PauseHandler = JwPause;
    c.RestartHandler = JwRestart;
    c.OidRequestHandler = JwOid;
    c.SendNetBufferListsHandler = JwSend;
    c.ReturnNetBufferListsHandler = JwReturn;
    c.CancelSendHandler = JwCancelSend;
    c.CheckForHangHandlerEx = JwCheckHang;
    c.ResetHandlerEx = JwReset;
    c.DevicePnPEventNotifyHandler = JwPnp;
    c.ShutdownHandlerEx = JwShutdown;
    c.CancelOidRequestHandler = JwCancelOid;
    return NdisMRegisterMiniportDriver(driver, registryPath, NULL, &c, &JwDriver);
}
