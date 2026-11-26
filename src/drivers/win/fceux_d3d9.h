#ifndef WIN_FCEUX_D3D9_H
#define WIN_FCEUX_D3D9_H

#include <windows.h>
#include <d3d9.h>

// Optional Direct3D 9 presentation backend.
// The runtime is loaded dynamically from the system d3d9.dll; no D3DX DLLs
// and no d3d9.lib import dependency are required by this module.
int  D3D9_Init(HWND hwnd, int vsync);
void D3D9_Shutdown(void);
void D3D9_SetVsync(int vsync);
int  D3D9_Draw(const unsigned long* src, int w, int h,
               const RECT* target, unsigned long border);
int  D3D9_IsInitialized(void);
void D3D9_Log(const char* fmt, ...);

#endif
