#include "fceux_d3d9.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static HMODULE g_d3d9Module = NULL;
static IDirect3D9* g_d3d = NULL;
static IDirect3DDevice9* g_dev = NULL;
static IDirect3DTexture9* g_tex = NULL;
static D3DPRESENT_PARAMETERS g_pp;
static HWND g_hwnd = NULL;
static int g_vsync = 1;
static int g_texW = 0;
static int g_texH = 0;
static int g_backW = 0;
static int g_backH = 0;

typedef IDirect3D9* (WINAPI* PFN_Direct3DCreate9)(UINT);
static PFN_Direct3DCreate9 g_Direct3DCreate9 = NULL;

struct TVertex
{
    float x, y, z, rhw;
    float u, v;
};

static const DWORD FVF_TVERTEX = D3DFVF_XYZRHW | D3DFVF_TEX1;

void D3D9_Log(const char* fmt, ...)
{
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return;

    char* slash = strrchr(path, '\\');
    if (!slash) return;
    strcpy(slash + 1, "fceux_d3d9.log");

    FILE* f = fopen(path, "a");
    if (!f) return;

    fprintf(f, "[%8lu ms] ", GetTickCount());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static void ReleaseTexture(void)
{
    if (g_tex)
    {
        g_tex->Release();
        g_tex = NULL;
    }
    g_texW = 0;
    g_texH = 0;
}

static void UnloadD3D9Runtime(void)
{
    g_Direct3DCreate9 = NULL;
    if (g_d3d9Module)
    {
        FreeLibrary(g_d3d9Module);
        g_d3d9Module = NULL;
    }
}

static int LoadD3D9Runtime(void)
{
    if (g_Direct3DCreate9) return 1;

    g_d3d9Module = LoadLibraryW(L"d3d9.dll");
    if (!g_d3d9Module)
    {
        D3D9_Log("LoadLibraryW(d3d9.dll) failed err=%lu", GetLastError());
        return 0;
    }

    g_Direct3DCreate9 = (PFN_Direct3DCreate9)GetProcAddress(g_d3d9Module, "Direct3DCreate9");
    if (!g_Direct3DCreate9)
    {
        D3D9_Log("GetProcAddress(Direct3DCreate9) failed err=%lu", GetLastError());
        UnloadD3D9Runtime();
        return 0;
    }
    return 1;
}

static void MakePresentParams(D3DPRESENT_PARAMETERS* pp)
{
    ZeroMemory(pp, sizeof(*pp));
    RECT cr = { 0, 0, 0, 0 };
    if (g_hwnd) GetClientRect(g_hwnd, &cr);
    g_backW = cr.right > 0 ? cr.right : 1;
    g_backH = cr.bottom > 0 ? cr.bottom : 1;
    pp->Windowed = TRUE;
    pp->BackBufferWidth = g_backW;
    pp->BackBufferHeight = g_backH;
    pp->SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp->BackBufferFormat = D3DFMT_UNKNOWN;
    pp->BackBufferCount = 1;
    pp->hDeviceWindow = g_hwnd;
    pp->EnableAutoDepthStencil = FALSE;
    pp->PresentationInterval = g_vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
}

static void ApplyDeviceState(void)
{
    if (!g_dev) return;
    g_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
}

static int CreateDeviceObject(void)
{
    if (!LoadD3D9Runtime()) return 0;

    g_d3d = g_Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_d3d)
    {
        D3D9_Log("Direct3DCreate9 failed");
        UnloadD3D9Runtime();
        return 0;
    }

    MakePresentParams(&g_pp);

    const DWORD commonFlags = D3DCREATE_NOWINDOWCHANGES | D3DCREATE_FPU_PRESERVE;
    HRESULT hr = g_d3d->CreateDevice(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_hwnd,
        commonFlags | D3DCREATE_HARDWARE_VERTEXPROCESSING,
        &g_pp, &g_dev);

    if (FAILED(hr))
    {
        D3D9_Log("hardware VP CreateDevice failed hr=0x%08lX; retry software VP", hr);
        hr = g_d3d->CreateDevice(
            D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_hwnd,
            commonFlags | D3DCREATE_SOFTWARE_VERTEXPROCESSING,
            &g_pp, &g_dev);
    }

    if (FAILED(hr))
    {
        D3D9_Log("CreateDevice failed hr=0x%08lX", hr);
        g_d3d->Release();
        g_d3d = NULL;
        UnloadD3D9Runtime();
        return 0;
    }

    ApplyDeviceState();

    D3D9_Log("CreateDevice OK hwnd=%p vsync=%d", (void*)g_hwnd, g_vsync);
    return 1;
}

int D3D9_Init(HWND hwnd, int vsync)
{
    D3D9_Shutdown();
    g_hwnd = hwnd;
    g_vsync = !!vsync;
    return CreateDeviceObject();
}

void D3D9_Shutdown(void)
{
    ReleaseTexture();
    if (g_dev)
    {
        g_dev->Release();
        g_dev = NULL;
    }
    if (g_d3d)
    {
        g_d3d->Release();
        g_d3d = NULL;
    }
    UnloadD3D9Runtime();
}

int D3D9_IsInitialized(void)
{
    return g_dev != NULL;
}

void D3D9_SetVsync(int vsync)
{
    vsync = !!vsync;
    if (vsync == g_vsync) return;
    g_vsync = vsync;
    if (!g_dev) return;

    ReleaseTexture();
    D3DPRESENT_PARAMETERS pp;
    MakePresentParams(&pp);
    if (SUCCEEDED(g_dev->Reset(&pp)))
    {
        g_pp = pp;
        ApplyDeviceState();
    }
}

static int EnsureTexture(int w, int h)
{
    if (g_tex && g_texW == w && g_texH == h) return 1;
    ReleaseTexture();

    HRESULT hr = g_dev->CreateTexture(
        w, h, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8,
        D3DPOOL_DEFAULT, &g_tex, NULL);
    if (FAILED(hr))
    {
        D3D9_Log("CreateTexture failed %dx%d hr=0x%08lX", w, h, hr);
        return 0;
    }

    g_texW = w;
    g_texH = h;
    return 1;
}

int D3D9_Draw(const unsigned long* src, int w, int h,
              const RECT* target, unsigned long border)
{
    if (!g_dev || !src || !target || w <= 0 || h <= 0) return 0;

    RECT cr = { 0, 0, 0, 0 };
    GetClientRect(g_hwnd, &cr);
    const int clientW = cr.right > 0 ? cr.right : 1;
    const int clientH = cr.bottom > 0 ? cr.bottom : 1;
    if (clientW != g_backW || clientH != g_backH)
    {
        ReleaseTexture();
        D3DPRESENT_PARAMETERS pp;
        MakePresentParams(&pp);
        HRESULT hr = g_dev->Reset(&pp);
        if (FAILED(hr)) return 0;
        g_pp = pp;
        ApplyDeviceState();
    }

    HRESULT coop = g_dev->TestCooperativeLevel();
    if (coop == D3DERR_DEVICELOST)
        return 0;

    if (coop == D3DERR_DEVICENOTRESET)
    {
        ReleaseTexture();
        D3DPRESENT_PARAMETERS pp;
        MakePresentParams(&pp);
        HRESULT hr = g_dev->Reset(&pp);
        if (FAILED(hr)) return 0;
        g_pp = pp;
        ApplyDeviceState();
    }

    if (!EnsureTexture(w, h)) return 0;

    D3DLOCKED_RECT locked;
    if (FAILED(g_tex->LockRect(0, &locked, NULL, D3DLOCK_DISCARD)))
        return 0;

    const size_t rowBytes = (size_t)w * 4;
    for (int y = 0; y < h; ++y)
    {
        const unsigned char* srow = (const unsigned char*)src + (size_t)y * rowBytes;
        unsigned char* drow = (unsigned char*)locked.pBits + (size_t)y * locked.Pitch;
        memcpy(drow, srow, rowBytes);
    }
    g_tex->UnlockRect(0);

    g_dev->Clear(0, NULL, D3DCLEAR_TARGET, border, 0.0f, 0);

    if (SUCCEEDED(g_dev->BeginScene()))
    {
        g_dev->SetTexture(0, g_tex);
        g_dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        g_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        g_dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        g_dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        g_dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        g_dev->SetFVF(FVF_TVERTEX);

        // D3D9 pre-transformed coordinates use a half-pixel offset for exact texel alignment.
        const float L = (float)target->left - 0.5f;
        const float T = (float)target->top - 0.5f;
        const float R = (float)target->right - 0.5f;
        const float B = (float)target->bottom - 0.5f;

        TVertex verts[4] = {
            { L, T, 0.0f, 1.0f, 0.0f, 0.0f },
            { R, T, 0.0f, 1.0f, 1.0f, 0.0f },
            { L, B, 0.0f, 1.0f, 0.0f, 1.0f },
            { R, B, 0.0f, 1.0f, 1.0f, 1.0f },
        };

        g_dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, verts, sizeof(TVertex));
        g_dev->EndScene();
    }

    return SUCCEEDED(g_dev->Present(NULL, NULL, NULL, NULL)) ? 1 : 0;
}
