#include "windows_abi.h"
#include "apple_input_ioctl.h"

static HANDLE g_Output;

static VOID AiWrite(PCSTR Text)
{
    DWORD written;
    SIZE_T length = 0;
    while (Text[length] != '\0')
        ++length;
    if (length != 0)
        WriteFile(g_Output, Text, (DWORD)length, &written, NULL);
}

static VOID AiWriteUnsigned(ULONGLONG Value)
{
    CHAR digits[24];
    SIZE_T count = 0;
    do {
        digits[count++] = (CHAR)('0' + (Value % 10));
        Value /= 10;
    } while (Value != 0 && count < sizeof(digits));
    while (count != 0) {
        CHAR digit[2] = {digits[--count], '\0'};
        AiWrite(digit);
    }
}

static VOID AiWriteHex(ULONG Value)
{
    static const CHAR hex[] = "0123456789ABCDEF";
    CHAR digits[8];
    for (ULONG index = 0; index < 8; ++index)
        digits[7 - index] = hex[(Value >> (index * 4)) & 0xf];
    AiWrite("0x");
    for (ULONG index = 0; index < 8; ++index) {
        CHAR digit[2] = {digits[index], '\0'};
        AiWrite(digit);
    }
}

static VOID AiWriteSigned(LONG Value)
{
    if (Value < 0) {
        AiWrite("-");
        AiWriteUnsigned((ULONGLONG)(-(LONGLONG)Value));
    } else {
        AiWriteUnsigned((ULONGLONG)Value);
    }
}

static VOID AiWriteDigest(const UCHAR *Bytes)
{
    static const CHAR hex[] = "0123456789abcdef";
    for (ULONG index = 0; index < AI_SHA256_DIGEST_SIZE; ++index) {
        CHAR pair[3] = {hex[Bytes[index] >> 4], hex[Bytes[index] & 0xf], '\0'};
        AiWrite(pair);
    }
}

#define AI_FIELD_U64(Name, Field) do { AiWrite(Name "="); AiWriteUnsigned(snapshot.Field); AiWrite("\r\n"); } while (0)
#define AI_FIELD_U32(Name, Field) do { AiWrite(Name "="); AiWriteUnsigned(snapshot.Field); AiWrite("\r\n"); } while (0)

VOID mainCRTStartup(VOID)
{
    AI_DIAGNOSTIC_SNAPSHOT_V4 snapshot;
    volatile UCHAR *snapshot_bytes = (volatile UCHAR *)&snapshot;
    SP_DEVICE_INTERFACE_DATA interface_data;
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail = NULL;
    DWORD required = 0;
    DWORD returned = 0;
    HDEVINFO devices;
    HANDLE device;
    DWORD error;

    for (SIZE_T index = 0; index < sizeof(snapshot); ++index)
        snapshot_bytes[index] = 0;

    g_Output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (g_Output == NULL || g_Output == INVALID_HANDLE_VALUE)
        ExitProcess(2);

    devices = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_APPLE_INPUT_DIAGNOSTIC,
                                   NULL, NULL,
                                   DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) {
        AiWrite("diagnostic_interface=unavailable error=");
        AiWriteUnsigned(GetLastError());
        AiWrite("\r\n");
        ExitProcess(1);
    }

    interface_data.cbSize = sizeof(interface_data);
    if (!SetupDiEnumDeviceInterfaces(devices, NULL,
                                     &GUID_DEVINTERFACE_APPLE_INPUT_DIAGNOSTIC,
                                     0, &interface_data)) {
        error = GetLastError();
        SetupDiDestroyDeviceInfoList(devices);
        AiWrite("diagnostic_interface=not_present error=");
        AiWriteUnsigned(error);
        AiWrite("\r\n");
        ExitProcess(1);
    }

    SetupDiGetDeviceInterfaceDetailW(devices, &interface_data, NULL, 0,
                                     &required, NULL);
    if (required < sizeof(*detail)) {
        error = GetLastError();
        SetupDiDestroyDeviceInfoList(devices);
        AiWrite("diagnostic_interface=bad_path_size error=");
        AiWriteUnsigned(error);
        AiWrite("\r\n");
        ExitProcess(1);
    }
    detail = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, required);
    if (detail == NULL) {
        SetupDiDestroyDeviceInfoList(devices);
        AiWrite("diagnostic_interface=out_of_memory\r\n");
        ExitProcess(1);
    }
    detail->cbSize = sizeof(*detail);
    if (!SetupDiGetDeviceInterfaceDetailW(devices, &interface_data, detail,
                                          required, NULL, NULL)) {
        error = GetLastError();
        HeapFree(GetProcessHeap(), 0, detail);
        SetupDiDestroyDeviceInfoList(devices);
        AiWrite("diagnostic_interface=path_failed error=");
        AiWriteUnsigned(error);
        AiWrite("\r\n");
        ExitProcess(1);
    }
    device = CreateFileW(detail->DevicePath, GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    error = GetLastError();
    HeapFree(GetProcessHeap(), 0, detail);
    SetupDiDestroyDeviceInfoList(devices);
    if (device == INVALID_HANDLE_VALUE) {
        AiWrite("diagnostic_interface=open_failed error=");
        AiWriteUnsigned(error);
        AiWrite("\r\n");
        ExitProcess(1);
    }

    if (!DeviceIoControl(device, IOCTL_AI_GET_SNAPSHOT, NULL, 0,
                         &snapshot, sizeof(snapshot), &returned, NULL)) {
        error = GetLastError();
        CloseHandle(device);
        AiWrite("snapshot=failed error=");
        AiWriteUnsigned(error);
        AiWrite("\r\n");
        ExitProcess(1);
    }
    CloseHandle(device);
    if (returned != sizeof(snapshot) ||
        snapshot.Version != AI_DIAGNOSTIC_SNAPSHOT_VERSION_4 ||
        snapshot.Size != sizeof(snapshot)) {
        AiWrite("snapshot=invalid returned=");
        AiWriteUnsigned(returned);
        AiWrite(" version=");
        AiWriteUnsigned(snapshot.Version);
        AiWrite(" size=");
        AiWriteUnsigned(snapshot.Size);
        AiWrite("\r\n");
        ExitProcess(1);
    }

    AiWrite("snapshot=ok version=4\r\n");
    AI_FIELD_U32("header_write_index", HeaderWriteIndex);
    AI_FIELD_U32("transport_phase", TransportPhase);
    AI_FIELD_U64("interrupts", InterruptCount);
    AI_FIELD_U64("workers_queued", WorkerQueuedCount);
    AI_FIELD_U64("workers_completed", WorkerCompletedCount);
    AI_FIELD_U64("spi_transfers", SpiTransferCount);
    AI_FIELD_U64("spi_timeouts", SpiTimeoutCount);
    AI_FIELD_U64("packet_crc_failures", PacketCrcFailureCount);
    AI_FIELD_U64("message_crc_failures", MessageCrcFailureCount);
    AI_FIELD_U64("fragment_failures", FragmentFailureCount);
    AI_FIELD_U64("keyboard_reports", KeyboardReportCount);
    AI_FIELD_U64("trackpad_reports", TrackpadReportCount);
    AI_FIELD_U64("resets", ResetCount);
    AI_FIELD_U64("offline", OfflineCount);
    AI_FIELD_U32("message_phase", MessagePhase);
    AI_FIELD_U32("message_type", MessageType);
    AI_FIELD_U32("message_report", MessageReport);
    AI_FIELD_U32("message_device", MessageDevice);
    AI_FIELD_U32("message_id", MessageId);
    AI_FIELD_U32("message_response_length", MessageResponseLength);
    AI_FIELD_U32("message_payload_length", MessagePayloadLength);
    AI_FIELD_U32("keyboard_descriptor_length", KeyboardDescriptorLength);
    AI_FIELD_U32("trackpad_descriptor_length", TrackpadDescriptorLength);
    AI_FIELD_U32("descriptor_digest_status", DescriptorDigestStatus);
    AiWrite("keyboard_descriptor_sha256=");
    AiWriteDigest(snapshot.KeyboardDescriptorSha256);
    AiWrite("\r\ntrackpad_descriptor_sha256=");
    AiWriteDigest(snapshot.TrackpadDescriptorSha256);
    AiWrite("\r\n");
    AI_FIELD_U32("keyboard_contract_valid", KeyboardContractValid);
    AI_FIELD_U32("keyboard_vhf_state", KeyboardVhfState);
    AI_FIELD_U64("keyboard_reports_accepted", KeyboardReportAcceptedCount);
    AI_FIELD_U64("keyboard_reports_rejected", KeyboardReportRejectedCount);
    AI_FIELD_U64("keyboard_reports_submitted", KeyboardReportSubmittedCount);
    AI_FIELD_U64("keyboard_vhf_submission_failures", KeyboardVhfSubmissionFailureCount);
    AI_FIELD_U64("keyboard_vhf_start_failures", KeyboardVhfStartFailureCount);
    AiWrite("keyboard_vhf_last_status=");
    AiWriteHex((ULONG)snapshot.KeyboardVhfLastStatus);
    AiWrite("\r\n");
    AI_FIELD_U32("trackpad_init_phase", TrackpadInitPhase);
    AI_FIELD_U32("trackpad_init_retries", TrackpadInitRetryCount);
    AI_FIELD_U32("trackpad_init_attempts", TrackpadInitAttemptCount);
    AI_FIELD_U32("trackpad_axis_x_valid", TrackpadAxisXValid);
    AI_FIELD_U32("trackpad_axis_y_valid", TrackpadAxisYValid);
    AI_FIELD_U32("trackpad_active_contacts", TrackpadActiveCount);
    AI_FIELD_U32("trackpad_admitted_contacts", TrackpadAdmittedCount);
    AI_FIELD_U32("trackpad_suppressed_contacts", TrackpadSuppressedCount);
    AiWrite("trackpad_logical_x="); AiWriteSigned(snapshot.TrackpadLogicalXMinimum);
    AiWrite(","); AiWriteSigned(snapshot.TrackpadLogicalXMaximum);
    AiWrite("\r\ntrackpad_logical_y="); AiWriteSigned(snapshot.TrackpadLogicalYMinimum);
    AiWrite(","); AiWriteSigned(snapshot.TrackpadLogicalYMaximum);
    AiWrite("\r\ntrackpad_physical_x="); AiWriteSigned(snapshot.TrackpadPhysicalXMinimum);
    AiWrite(","); AiWriteSigned(snapshot.TrackpadPhysicalXMaximum);
    AiWrite("\r\ntrackpad_physical_y="); AiWriteSigned(snapshot.TrackpadPhysicalYMinimum);
    AiWrite(","); AiWriteSigned(snapshot.TrackpadPhysicalYMaximum);
    AiWrite("\r\n");
    AI_FIELD_U32("trackpad_unit", TrackpadUnit);
    AiWrite("trackpad_unit_exponent="); AiWriteSigned(snapshot.TrackpadUnitExponent);
    AiWrite("\r\n");
    AI_FIELD_U32("trackpad_vhf_state", TrackpadVhfState);
    AI_FIELD_U32("trackpad_last_rejection", TrackpadLastRejection);
    AI_FIELD_U64("trackpad_reports_decoded", TrackpadReportDecodedCount);
    AI_FIELD_U64("trackpad_reports_rejected", TrackpadReportRejectedCount);
    AI_FIELD_U64("trackpad_reports_submitted", TrackpadReportSubmittedCount);
    AI_FIELD_U64("trackpad_vhf_submission_failures", TrackpadVhfSubmissionFailureCount);
    AI_FIELD_U64("trackpad_vhf_start_failures", TrackpadVhfStartFailureCount);
    AI_FIELD_U64("trackpad_get_feature_count", TrackpadGetFeatureCount);
    AI_FIELD_U64("trackpad_set_feature_count", TrackpadSetFeatureCount);
    AiWrite("trackpad_feature_last_status=");
    AiWriteHex((ULONG)snapshot.TrackpadFeatureLastStatus);
    AiWrite("\r\n");
    AiWrite("trackpad_vhf_last_status=");
    AiWriteHex((ULONG)snapshot.TrackpadVhfLastStatus);
    AiWrite("\r\n");
    for (ULONG index = 0; index < AI_PACKET_HEADER_RING_CAPACITY; ++index) {
        const AI_PACKET_HEADER_V1 *header = &snapshot.Headers[index];
        AiWrite("packet_header["); AiWriteUnsigned(index); AiWrite("]=seq:");
        AiWriteUnsigned(header->Sequence); AiWrite(",result:");
        AiWriteUnsigned(header->Result); AiWrite(",offset:");
        AiWriteUnsigned(header->Offset); AiWrite(",remaining:");
        AiWriteUnsigned(header->Remaining); AiWrite(",length:");
        AiWriteUnsigned(header->Length); AiWrite(",flags:");
        AiWriteUnsigned(header->Flags); AiWrite(",device:");
        AiWriteUnsigned(header->Device); AiWrite("\r\n");
    }
    ExitProcess(0);
}
