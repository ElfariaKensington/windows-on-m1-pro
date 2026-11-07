/* SPDX-License-Identifier: MIT */
/* Shared by the hardware source and the resource-free Windows VM probe. */
static VOID ji_vhf_binding(VHF_CONFIG *config, PWSTR instance, USHORT bytes)
{
    static WCHAR hardware_ids[] = L"J316SPI\\VHF_INPUT\0";
    config->HardwareIDs = hardware_ids;
    config->HardwareIDsLength = sizeof(hardware_ids);
    config->InstanceID = instance;
    config->InstanceIDLength = bytes;
}
