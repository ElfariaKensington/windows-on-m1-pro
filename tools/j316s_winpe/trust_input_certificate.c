/* SPDX-License-Identifier: MIT */

#include <Windows.h>
#include <wincrypt.h>

/* The development certificate is embedded when building this WinPE helper.
 * It runs only from the RAM-backed WinPE image. */
static const BYTE input_certificate[] = {
#include "apple_input_certificate.inc"
};

static BOOL trust_certificate(LPCWSTR name)
{
    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                                    CERT_SYSTEM_STORE_LOCAL_MACHINE, name);
    if (!store)
        return FALSE;

    BOOL added = CertAddEncodedCertificateToStore(
        store, X509_ASN_ENCODING, input_certificate, sizeof(input_certificate),
        CERT_STORE_ADD_REPLACE_EXISTING, NULL);
    CertCloseStore(store, 0);
    return added;
}

void mainCRTStartup(void)
{
    if (GetFileAttributesW(L"X:\\Windows\\System32\\wpeutil.exe") == INVALID_FILE_ATTRIBUTES)
        ExitProcess(1);
    HKEY winpe;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\MiniNT",
                     0, KEY_READ, &winpe) != ERROR_SUCCESS)
        ExitProcess(1);
    RegCloseKey(winpe);
    ExitProcess(trust_certificate(L"ROOT") && trust_certificate(L"TrustedPublisher") ? 0 : 2);
}
