#ifndef J316S_WINPE_WINDOWS_ABI_H
#define J316S_WINPE_WINDOWS_ABI_H

#include <guiddef.h>

typedef unsigned char UCHAR;
typedef char CHAR;
typedef unsigned short USHORT;
typedef unsigned long ULONG;
typedef unsigned long DWORD;
typedef long LONG;
typedef unsigned long long ULONGLONG;
typedef long long LONGLONG;
typedef unsigned long long SIZE_T;
typedef unsigned long long ULONG_PTR;
typedef unsigned short WCHAR;
typedef int BOOL;
typedef void VOID;
typedef void *HANDLE;
typedef void *HDEVINFO;
typedef void *LPVOID;
typedef const void *LPCVOID;
typedef const WCHAR *LPCWSTR;
typedef const char *PCSTR;
typedef DWORD *LPDWORD;
typedef long long LONG_PTR;

#define WINAPI __stdcall
#define NULL ((void *)0)
#define INVALID_HANDLE_VALUE ((HANDLE)(LONG_PTR)-1)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define DIGCF_PRESENT 0x00000002
#define DIGCF_DEVICEINTERFACE 0x00000010
#define GENERIC_READ 0x80000000UL
#define FILE_SHARE_READ 0x00000001
#define FILE_SHARE_WRITE 0x00000002
#define OPEN_EXISTING 3
#define FILE_ATTRIBUTE_NORMAL 0x00000080
#define HEAP_ZERO_MEMORY 0x00000008
#define FILE_DEVICE_UNKNOWN 0x00000022
#define METHOD_BUFFERED 0
#define FILE_READ_DATA 0x0001
#define CTL_CODE(DeviceType, Function, Method, Access) \
    (((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method))

typedef struct _SP_DEVICE_INTERFACE_DATA {
    DWORD cbSize;
    GUID InterfaceClassGuid;
    DWORD Flags;
    ULONG_PTR Reserved;
} SP_DEVICE_INTERFACE_DATA, *PSP_DEVICE_INTERFACE_DATA;

typedef struct _SP_DEVICE_INTERFACE_DETAIL_DATA_W {
    DWORD cbSize;
    WCHAR DevicePath[1];
} SP_DEVICE_INTERFACE_DETAIL_DATA_W, *PSP_DEVICE_INTERFACE_DETAIL_DATA_W;

__declspec(dllimport) HANDLE WINAPI GetStdHandle(DWORD);
__declspec(dllimport) DWORD WINAPI GetLastError(VOID);
__declspec(dllimport) BOOL WINAPI WriteFile(HANDLE, LPCVOID, DWORD, LPDWORD, LPVOID);
__declspec(dllimport) HANDLE WINAPI GetProcessHeap(VOID);
__declspec(dllimport) LPVOID WINAPI HeapAlloc(HANDLE, DWORD, SIZE_T);
__declspec(dllimport) BOOL WINAPI HeapFree(HANDLE, DWORD, LPVOID);
__declspec(dllimport) HANDLE WINAPI CreateFileW(LPCWSTR, DWORD, DWORD, LPVOID,
                                                 DWORD, DWORD, HANDLE);
__declspec(dllimport) BOOL WINAPI DeviceIoControl(HANDLE, DWORD, LPVOID, DWORD,
                                                   LPVOID, DWORD, LPDWORD, LPVOID);
__declspec(dllimport) BOOL WINAPI CloseHandle(HANDLE);
__declspec(dllimport) __declspec(noreturn) VOID WINAPI ExitProcess(DWORD);
__declspec(dllimport) HDEVINFO WINAPI SetupDiGetClassDevsW(
    const GUID *, LPCWSTR, HANDLE, DWORD);
__declspec(dllimport) BOOL WINAPI SetupDiEnumDeviceInterfaces(
    HDEVINFO, LPVOID, const GUID *, DWORD, PSP_DEVICE_INTERFACE_DATA);
__declspec(dllimport) BOOL WINAPI SetupDiGetDeviceInterfaceDetailW(
    HDEVINFO, PSP_DEVICE_INTERFACE_DATA, PSP_DEVICE_INTERFACE_DETAIL_DATA_W,
    DWORD, LPDWORD, LPVOID);
__declspec(dllimport) BOOL WINAPI SetupDiDestroyDeviceInfoList(HDEVINFO);

#endif
