/* SPDX-License-Identifier: MIT */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#include <winioctl.h>
#include "certificate.h"
#include "diagnostics.h"

static void print(const char *s)
{ DWORD done; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, (DWORD)lstrlenA(s), &done, NULL); }
void mainCRTStartup(void)
{
    const WCHAR *cmd = GetCommandLineW();
    while (*cmd && *cmd != L'/') ++cmd;
    if (*cmd && (cmd[1] == L'w' || cmd[1] == L'd')) {
        Sleep(cmd[1] == L'w' ? 20000 : 3000);
        ExitProcess(0);
    }
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
        JI_DIAG diag; DWORD bytes = sizeof(diag);
        BOOL live = device != INVALID_HANDLE_VALUE && DeviceIoControl(device,
                CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED, FILE_READ_ACCESS),
                NULL, 0, &diag, sizeof(diag), &bytes, NULL);
        if (device != INVALID_HANDLE_VALUE) CloseHandle(device);
        if (!live) {
            bytes = sizeof(diag);
            result = RegGetValueW(HKEY_LOCAL_MACHINE,
                L"SYSTEM\\CurrentControlSet\\Services\\J316SpiInput\\Parameters",
                L"Snapshot", RRF_RT_REG_BINARY, NULL, &diag, &bytes);
        }
        if (!result && bytes == sizeof(diag) && diag.version == 2) {
            print(live ? "Live internal SPI diagnostics\r\n" : "Saved internal SPI startup diagnostics\r\n");
            if (diag.phase != 10) print("INPUT HAS NOT REACHED RUNNING STATE\r\n");
            const char *names[] = {
#define JI_NAME(name) #name,
                JI_DIAGNOSTIC_FIELDS(JI_NAME)
#undef JI_NAME
            };
            const uint32_t *values = (const uint32_t *)&diag;
            for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
                char line[100]; wsprintfA(line, "%s=%lu (0x%08lx)\r\n", names[i], values[i], values[i]); print(line);
            }
        } else {
            char line[100]; wsprintfA(line, "No saved startup diagnostics; registry error %lu\r\n", result); print(line);
            result = result ? result : ERROR_INVALID_DATA;
        }
    }
    ExitProcess(result);
}
