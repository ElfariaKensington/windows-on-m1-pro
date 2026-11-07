/* SPDX-License-Identifier: MIT */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
void mainCRTStartup(void)
{
    DWORD keys = 0, moves = 0, written; POINT previous, point;
    GetCursorPos(&previous);
    for (unsigned i = 0; i < 1500; ++i) {
        if (GetAsyncKeyState('A') & 0x8000) ++keys;
        if (GetCursorPos(&point) && (point.x != previous.x || point.y != previous.y)) ++moves;
        previous = point; Sleep(10);
    }
    char text[100]; wsprintfA(text, "keyboard_down_samples=%lu cursor_changes=%lu\r\n", keys, moves);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, (DWORD)lstrlenA(text), &written, NULL);
    ExitProcess(keys && moves ? 0 : 1);
}
