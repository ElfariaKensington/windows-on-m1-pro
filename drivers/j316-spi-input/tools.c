/* SPDX-License-Identifier: MIT */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#include <winioctl.h>
#include "certificate.h"

static void print(const char *s)
{ DWORD done; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, (DWORD)lstrlenA(s), &done, NULL); }
void mainCRTStartup(void)
{
    const WCHAR *cmd = GetCommandLineW();
    while (*cmd && *cmd != L'/') ++cmd;
    DWORD result = 0;
    if (*cmd && cmd[1] == L't') {
        PCCERT_CONTEXT cert = CertCreateCertificateContext(X509_ASN_ENCODING,
                                                           ji_certificate, sizeof(ji_certificate));
        if (!cert) ExitProcess(GetLastError());
        const WCHAR *stores[] = {L"ROOT", L"TrustedPublisher"};
        for (unsigned i = 0; i < 2; ++i) {
            HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                                             CERT_SYSTEM_STORE_LOCAL_MACHINE, stores[i]);
            if (!store || !CertAddCertificateContextToStore(store, cert,
                                     CERT_STORE_ADD_REPLACE_EXISTING, NULL)) result = GetLastError();
            if (store) CertCloseStore(store, 0);
        }
        CertFreeCertificateContext(cert);
        print(result ? "Certificate import failed\r\n" : "Development certificate imported\r\n");
    } else {
        HANDLE device = CreateFileW(L"\\\\.\\J316SpiInput", GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (device == INVALID_HANDLE_VALUE) {
            char line[80]; wsprintfA(line, "J316SpiInput open error %lu\r\n", GetLastError()); print(line);
            ExitProcess(1);
        }
        DWORD values[17], bytes;
        if (!DeviceIoControl(device, CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED, FILE_READ_ACCESS),
                             NULL, 0, values, sizeof(values), &bytes, NULL)) result = GetLastError();
        else {
            const char *names[] = {"version","phase","status","booted","vendor","product",
                "packets","malformed","timeouts","keyboard","mouse","submit_errors","resets",
                "spi_control","spi_fifo","gpio_input","descriptor_size"};
            for (unsigned i = 0; i < 17; ++i) {
                char line[100]; wsprintfA(line, "%s=%lu (0x%08lx)\r\n", names[i], values[i], values[i]); print(line);
            }
        }
        CloseHandle(device);
    }
    ExitProcess(result);
}
