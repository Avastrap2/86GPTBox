/*
 * R2PTEST - ATI 3D Rage II+ / Mach64 GTB Windows 95 validation utility
 *
 * Target: Win32 / Windows 95, DirectX 5-era DirectDraw + Direct3D interfaces.
 * The program deliberately uses IDirectDraw2 / IDirect3D2 / IDirect3DDevice2.
 */
#define WINVER 0x0400
#define DIRECTDRAW_VERSION 0x0500
#define DIRECT3D_VERSION 0x0500
#define COBJMACROS
#define INITGUID

#include <windows.h>
#include <ddraw.h>
#include <d3d.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define APP_TITLE "ATI Rage II+ Validation Suite"
#define LOG_FILE  "R2PTEST.LOG"
#define TEST_W 320
#define TEST_H 240
#define IDC_RUN_AUTO 1001
#define IDC_OVERLAY  1002
#define IDC_FLIP     1003
#define IDC_SAVE     1004
#define IDC_EXIT     1005
#define IDC_LOG      1010

#ifndef DDBLT_WAIT
#define DDBLT_WAIT 0x01000000L
#endif

#define STATUS_PASS 1
#define STATUS_FAIL 2
#define STATUS_SKIP 3
#define STATUS_INFO 4

typedef HRESULT (WINAPI *PFN_DirectDrawCreate)(GUID *, LPDIRECTDRAW *, IUnknown *);

typedef struct AppState {
    HINSTANCE instance;
    HWND hwnd;
    HWND log_edit;
    HMODULE ddraw_module;
    PFN_DirectDrawCreate pDirectDrawCreate;
    LPDIRECTDRAW dd1;
    LPDIRECTDRAW2 dd;
    int pass_count;
    int fail_count;
    int skip_count;
    int running;
} AppState;

static AppState g_app;

static void append_log_text(const char *text)
{
    int len;
    if (!g_app.log_edit)
        return;
    len = GetWindowTextLengthA(g_app.log_edit);
    SendMessageA(g_app.log_edit, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageA(g_app.log_edit, EM_REPLACESEL, FALSE, (LPARAM)text);
}

static void log_line(int status, const char *name, const char *fmt, ...)
{
    char msg[768];
    char line[1024];
    const char *tag;
    va_list args;

    if (status == STATUS_PASS) {
        tag = "PASS";
        g_app.pass_count++;
    } else if (status == STATUS_FAIL) {
        tag = "FAIL";
        g_app.fail_count++;
    } else if (status == STATUS_SKIP) {
        tag = "SKIP";
        g_app.skip_count++;
    } else {
        tag = "INFO";
    }

    msg[0] = 0;
    if (fmt && *fmt) {
        va_start(args, fmt);
        vsprintf(msg, fmt, args);
        va_end(args);
    }
    if (msg[0])
        wsprintfA(line, "[%s] %-28s %s\r\n", tag, name, msg);
    else
        wsprintfA(line, "[%s] %s\r\n", tag, name);
    append_log_text(line);
}

static void log_hr(int status, const char *name, HRESULT hr)
{
    log_line(status, name, "HRESULT=%08lX", (DWORD)hr);
}

static void release_ddraw(void)
{
    if (g_app.dd) {
        IDirectDraw2_Release(g_app.dd);
        g_app.dd = NULL;
    }
    if (g_app.dd1) {
        IDirectDraw_Release(g_app.dd1);
        g_app.dd1 = NULL;
    }
    if (g_app.ddraw_module) {
        FreeLibrary(g_app.ddraw_module);
        g_app.ddraw_module = NULL;
    }
    g_app.pDirectDrawCreate = NULL;
}

static HRESULT init_ddraw(void)
{
    HRESULT hr;
    if (g_app.dd)
        return DD_OK;

    g_app.ddraw_module = LoadLibraryA("DDRAW.DLL");
    if (!g_app.ddraw_module)
        return E_FAIL;
    g_app.pDirectDrawCreate = (PFN_DirectDrawCreate)GetProcAddress(g_app.ddraw_module, "DirectDrawCreate");
    if (!g_app.pDirectDrawCreate)
        return E_FAIL;

    hr = g_app.pDirectDrawCreate(NULL, &g_app.dd1, NULL);
    if (FAILED(hr))
        return hr;
    hr = IDirectDraw_QueryInterface(g_app.dd1, &IID_IDirectDraw2, (void **)&g_app.dd);
    if (FAILED(hr))
        return hr;
    hr = IDirectDraw2_SetCooperativeLevel(g_app.dd, g_app.hwnd, DDSCL_NORMAL);
    return hr;
}

static DDPIXELFORMAT pf_rgb565(void)
{
    DDPIXELFORMAT pf;
    ZeroMemory(&pf, sizeof(pf));
    pf.dwSize = sizeof(pf);
    pf.dwFlags = DDPF_RGB;
    pf.dwRGBBitCount = 16;
    pf.dwRBitMask = 0xF800;
    pf.dwGBitMask = 0x07E0;
    pf.dwBBitMask = 0x001F;
    return pf;
}

static DDPIXELFORMAT pf_rgb555(void)
{
    DDPIXELFORMAT pf;
    ZeroMemory(&pf, sizeof(pf));
    pf.dwSize = sizeof(pf);
    pf.dwFlags = DDPF_RGB;
    pf.dwRGBBitCount = 16;
    pf.dwRBitMask = 0x7C00;
    pf.dwGBitMask = 0x03E0;
    pf.dwBBitMask = 0x001F;
    return pf;
}

static DDPIXELFORMAT pf_rgb32(void)
{
    DDPIXELFORMAT pf;
    ZeroMemory(&pf, sizeof(pf));
    pf.dwSize = sizeof(pf);
    pf.dwFlags = DDPF_RGB;
    pf.dwRGBBitCount = 32;
    pf.dwRBitMask = 0x00FF0000;
    pf.dwGBitMask = 0x0000FF00;
    pf.dwBBitMask = 0x000000FF;
