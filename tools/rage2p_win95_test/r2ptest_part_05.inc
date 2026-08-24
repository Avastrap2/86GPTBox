        "ATI Rage II+ Overlay Visual Test", MB_YESNOCANCEL | MB_ICONQUESTION);
    IDirectDrawSurface_UpdateOverlay(overlay, NULL, primary, NULL, DDOVER_HIDE, NULL);
    if (answer == IDYES) log_line(STATUS_PASS, "YUY2 overlay visual", "user confirmed correct colors/scaling");
    else if (answer == IDNO) log_line(STATUS_FAIL, "YUY2 overlay visual", "user observed incorrect output");
    else log_line(STATUS_SKIP, "YUY2 overlay visual", "cancelled by user");

done:
    if (overlay) IDirectDrawSurface_Release(overlay);
    if (primary) IDirectDrawSurface_Release(primary);
}

static void run_fullscreen_flip(void)
{
    DDSURFACEDESC sd;
    DDSCAPS caps;
    LPDIRECTDRAWSURFACE primary = NULL, back = NULL;
    HRESULT hr;
    int answer;

    if (FAILED(init_ddraw())) return;
    IDirectDraw2_SetCooperativeLevel(g_app.dd, g_app.hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    hr = IDirectDraw2_SetDisplayMode(g_app.dd, 640, 480, 16, 0, 0);
    if (FAILED(hr)) { log_hr(STATUS_SKIP, "Fullscreen 640x480x16", hr); goto restore; }
    ZeroMemory(&sd, sizeof(sd)); sd.dwSize = sizeof(sd);
    sd.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
    sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
    sd.dwBackBufferCount = 1;
    hr = IDirectDraw2_CreateSurface(g_app.dd, &sd, &primary, NULL);
    if (FAILED(hr)) { log_hr(STATUS_FAIL, "Primary flip chain create", hr); goto restore; }
    ZeroMemory(&caps, sizeof(caps)); caps.dwCaps = DDSCAPS_BACKBUFFER;
    hr = IDirectDrawSurface_GetAttachedSurface(primary, &caps, &back);
    if (FAILED(hr)) { log_hr(STATUS_FAIL, "Get flip backbuffer", hr); goto restore; }
    fill_surface_color(primary, 0x001F);
    fill_surface_color(back, 0xF800);
    hr = IDirectDrawSurface_Flip(primary, NULL, DDFLIP_WAIT);
    if (FAILED(hr)) { log_hr(STATUS_FAIL, "DirectDraw page flip", hr); goto restore; }
    answer = MessageBoxA(g_app.hwnd,
        "Fullscreen flip test: screen should now be RED.\n\nIs the full screen red and stable?",
        "ATI Rage II+ Page Flip", MB_YESNOCANCEL | MB_ICONQUESTION);
    if (answer == IDYES) log_line(STATUS_PASS, "Fullscreen page flip", "user confirmed red front buffer");
    else if (answer == IDNO) log_line(STATUS_FAIL, "Fullscreen page flip", "user observed wrong/unstable frame");
    else log_line(STATUS_SKIP, "Fullscreen page flip", "cancelled by user");

restore:
    if (back) IDirectDrawSurface_Release(back);
    if (primary) IDirectDrawSurface_Release(primary);
    IDirectDraw2_RestoreDisplayMode(g_app.dd);
    IDirectDraw2_SetCooperativeLevel(g_app.dd, g_app.hwnd, DDSCL_NORMAL);
    ShowWindow(g_app.hwnd, SW_RESTORE);
    SetForegroundWindow(g_app.hwnd);
}

static void save_log_copy(void)
{
    HANDLE f;
    DWORD written;
    int len = GetWindowTextLengthA(g_app.log_edit);
    char *buf;
    char path[MAX_PATH];
    if (len <= 0) return;
    buf = (char *)GlobalAlloc(GMEM_FIXED, len + 1);
    if (!buf) return;
    GetWindowTextA(g_app.log_edit, buf, len + 1);
    GetModuleFileNameA(NULL, path, sizeof(path));
    {
        char *p = path + lstrlenA(path);
        while (p > path && p[-1] != '\\' && p[-1] != '/') p--;
        lstrcpyA(p, LOG_FILE);
    }
    f = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        WriteFile(f, buf, (DWORD)len, &written, NULL);
        CloseHandle(f);
        MessageBoxA(g_app.hwnd, path, "Log saved", MB_OK | MB_ICONINFORMATION);
    } else {
        MessageBoxA(g_app.hwnd, "Unable to create R2PTEST.LOG", APP_TITLE, MB_OK | MB_ICONEXCLAMATION);
    }
    GlobalFree(buf);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_CREATE:
        g_app.log_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
            10, 50, 760, 480, hwnd, (HMENU)IDC_LOG, g_app.instance, NULL);
        CreateWindowA("BUTTON", "Run Automatic", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      10, 10, 120, 30, hwnd, (HMENU)IDC_RUN_AUTO, g_app.instance, NULL);
        CreateWindowA("BUTTON", "Overlay Visual", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      140, 10, 120, 30, hwnd, (HMENU)IDC_OVERLAY, g_app.instance, NULL);
        CreateWindowA("BUTTON", "Fullscreen Flip", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      270, 10, 120, 30, hwnd, (HMENU)IDC_FLIP, g_app.instance, NULL);
        CreateWindowA("BUTTON", "Save Log", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      400, 10, 100, 30, hwnd, (HMENU)IDC_SAVE, g_app.instance, NULL);
        CreateWindowA("BUTTON", "Exit", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      510, 10, 80, 30, hwnd, (HMENU)IDC_EXIT, g_app.instance, NULL);
        return 0;
    case WM_SIZE:
        if (g_app.log_edit)
            MoveWindow(g_app.log_edit, 10, 50, LOWORD(lparam) - 20, HIWORD(lparam) - 60, TRUE);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDC_RUN_AUTO: run_automatic_tests(); break;
        case IDC_OVERLAY: run_overlay_visual(); break;
        case IDC_FLIP: run_fullscreen_flip(); break;
        case IDC_SAVE: save_log_copy(); break;
        case IDC_EXIT: DestroyWindow(hwnd); break;
        }
        return 0;
    case WM_DESTROY:
        release_ddraw();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE prev, LPSTR cmdline, int show)
{
    WNDCLASSA wc;
    MSG msg;
    (void)prev; (void)cmdline;
    ZeroMemory(&g_app, sizeof(g_app));
    g_app.instance = instance;
    ZeroMemory(&wc, sizeof(wc));
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "R2PTEST_W95";
    if (!RegisterClassA(&wc)) return 1;
    g_app.hwnd = CreateWindowA(wc.lpszClassName, APP_TITLE,
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
        NULL, NULL, instance, NULL);
    if (!g_app.hwnd) return 1;
    ShowWindow(g_app.hwnd, show);
    UpdateWindow(g_app.hwnd);
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return (int)msg.wParam;
}
