
static void test_d3d_z_alpha(void)
{
    LPDIRECT3D2 d3d = NULL;
    LPDIRECTDRAWSURFACE target = NULL, zsurf = NULL;
    LPDIRECT3DDEVICE2 dev = NULL;
    LPDIRECT3DVIEWPORT2 vp = NULL;
    D3DEnumState es;
    DDPIXELFORMAT zpf;
    D3DTLVERTEX a[3], b[3];
    HRESULT hr;
    DWORD raw = 0;

    if (!init_d3d(&d3d, &target, &dev, &vp, &es)) goto done;
    ZeroMemory(&zpf, sizeof(zpf)); zpf.dwSize = sizeof(zpf);
    zpf.dwFlags = DDPF_ZBUFFER; zpf.dwZBufferBitDepth = 16;
    zpf.dwZBitMask = 0xFFFF;
    hr = create_surface(TEST_W, TEST_H, DDSCAPS_ZBUFFER | DDSCAPS_VIDEOMEMORY, &zpf, &zsurf);
    if (FAILED(hr)) {
        log_hr(STATUS_SKIP, "D3D 16-bit Z surface", hr);
    } else {
        hr = IDirectDrawSurface_AddAttachedSurface(target, zsurf);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice2_SetRenderState(dev, D3DRENDERSTATE_ZENABLE, TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice2_SetRenderState(dev, D3DRENDERSTATE_ZWRITEENABLE, TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice2_SetRenderState(dev, D3DRENDERSTATE_ZFUNC, D3DCMP_LESSEQUAL);
        if (SUCCEEDED(hr)) hr = clear_view(vp, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER);
        set_tl(&a[0], 40,200,0.8f,0xFFFF0000UL,0,0); set_tl(&a[1],160,20,0.8f,0xFFFF0000UL,0,0); set_tl(&a[2],280,200,0.8f,0xFFFF0000UL,0,0);
        set_tl(&b[0], 60,190,0.2f,0xFF00FF00UL,0,0); set_tl(&b[1],160,40,0.2f,0xFF00FF00UL,0,0); set_tl(&b[2],260,190,0.2f,0xFF00FF00UL,0,0);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice2_BeginScene(dev);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice2_DrawPrimitive(dev,D3DPT_TRIANGLELIST,D3DVT_TLVERTEX,a,3,D3DDP_WAIT);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice2_DrawPrimitive(dev,D3DPT_TRIANGLELIST,D3DVT_TLVERTEX,b,3,D3DDP_WAIT);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice2_EndScene(dev);
        if (FAILED(hr) || !read_raw_pixel(target,160,110,&raw) || ((raw >> 5) & 63) < 30)
            log_hr(STATUS_FAIL, "D3D Z16 depth test", hr);
        else
            log_line(STATUS_PASS, "D3D Z16 depth test", "near green won=%04lX", raw);
    }

    IDirect3DDevice2_SetRenderState(dev, D3DRENDERSTATE_ZENABLE, FALSE);
    IDirect3DDevice2_SetRenderState(dev, D3DRENDERSTATE_ALPHABLENDENABLE, TRUE);
    IDirect3DDevice2_SetRenderState(dev, D3DRENDERSTATE_SRCBLEND, D3DBLEND_SRCALPHA);
    IDirect3DDevice2_SetRenderState(dev, D3DRENDERSTATE_DESTBLEND, D3DBLEND_INVSRCALPHA);
    clear_view(vp, D3DCLEAR_TARGET);
    /* Blue opaque base then 50% red. */
    set_tl(&a[0],20,220,0.5f,0xFF0000FFUL,0,0); set_tl(&a[1],160,10,0.5f,0xFF0000FFUL,0,0); set_tl(&a[2],300,220,0.5f,0xFF0000FFUL,0,0);
    set_tl(&b[0],40,200,0.4f,0x80FF0000UL,0,0); set_tl(&b[1],160,30,0.4f,0x80FF0000UL,0,0); set_tl(&b[2],280,200,0.4f,0x80FF0000UL,0,0);
    hr = IDirect3DDevice2_BeginScene(dev);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice2_DrawPrimitive(dev,D3DPT_TRIANGLELIST,D3DVT_TLVERTEX,a,3,D3DDP_WAIT);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice2_DrawPrimitive(dev,D3DPT_TRIANGLELIST,D3DVT_TLVERTEX,b,3,D3DDP_WAIT);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice2_EndScene(dev);
    if (FAILED(hr) || !read_raw_pixel(target,160,110,&raw) || (raw & 0xF800) == 0 || (raw & 0x001F) == 0)
        log_hr(STATUS_FAIL, "D3D alpha blend", hr);
    else
        log_line(STATUS_PASS, "D3D alpha blend", "mixed RGB565=%04lX", raw);

done:
    if (zsurf) IDirectDrawSurface_Release(zsurf);
    release_d3d(d3d, target, dev, vp);
}

typedef struct TextureCapsState {
    int rgb332, argb1555, rgb565, argb4444, rgb32, p8, p4;
} TextureCapsState;

static HRESULT CALLBACK enum_texture_formats(LPDDSURFACEDESC sd, LPVOID context)
{
    TextureCapsState *s = (TextureCapsState *)context;
    DDPIXELFORMAT *pf = &sd->ddpfPixelFormat;
    if (pf->dwFlags & DDPF_PALETTEINDEXED8) s->p8 = 1;
    if (pf->dwFlags & DDPF_PALETTEINDEXED4) s->p4 = 1;
    if (pf->dwFlags & DDPF_RGB) {
        if (pf->dwRGBBitCount == 8 && pf->dwRBitMask == 0xE0 && pf->dwGBitMask == 0x1C && pf->dwBBitMask == 0x03) s->rgb332 = 1;
        if (pf->dwRGBBitCount == 16 && pf->dwRBitMask == 0x7C00 && pf->dwGBitMask == 0x03E0 && pf->dwBBitMask == 0x001F) s->argb1555 = 1;
        if (pf->dwRGBBitCount == 16 && pf->dwRBitMask == 0xF800 && pf->dwGBitMask == 0x07E0 && pf->dwBBitMask == 0x001F) s->rgb565 = 1;
        if (pf->dwRGBBitCount == 16 && pf->dwRBitMask == 0x0F00 && pf->dwGBitMask == 0x00F0 && pf->dwBBitMask == 0x000F) s->argb4444 = 1;
        if (pf->dwRGBBitCount == 32 && pf->dwRBitMask == 0x00FF0000 && pf->dwGBitMask == 0x0000FF00 && pf->dwBBitMask == 0x000000FF) s->rgb32 = 1;
    }
    return D3DENUMRET_OK;
}

static void test_d3d_texture_caps(void)
{
    LPDIRECT3D2 d3d = NULL;
    LPDIRECTDRAWSURFACE target = NULL;
    LPDIRECT3DDEVICE2 dev = NULL;
    LPDIRECT3DVIEWPORT2 vp = NULL;
    D3DEnumState es;
    TextureCapsState tc;
    HRESULT hr;

    ZeroMemory(&tc, sizeof(tc));
    if (!init_d3d(&d3d, &target, &dev, &vp, &es)) goto done;
    hr = IDirect3DDevice2_EnumTextureFormats(dev, enum_texture_formats, &tc);
    if (FAILED(hr)) { log_hr(STATUS_FAIL, "D3D EnumTextureFormats", hr); goto done; }
    log_line(STATUS_PASS, "D3D EnumTextureFormats", "completed");
    log_line(tc.rgb332 ? STATUS_PASS : STATUS_FAIL, "Texture RGB332 advertised", "");
    log_line(tc.argb1555 ? STATUS_PASS : STATUS_FAIL, "Texture ARGB1555 advertised", "");
    log_line(tc.rgb565 ? STATUS_PASS : STATUS_FAIL, "Texture RGB565 advertised", "");
    log_line(tc.argb4444 ? STATUS_PASS : STATUS_FAIL, "Texture ARGB4444 advertised", "");
    log_line(tc.rgb32 ? STATUS_PASS : STATUS_FAIL, "Texture RGB32 advertised", "");
    log_line(tc.p8 ? STATUS_PASS : STATUS_FAIL, "Texture CI8/P8 advertised", "");
    log_line(tc.p4 ? STATUS_PASS : STATUS_FAIL, "Texture CI4/P4 advertised", "");

    hr = IDirect3DDevice2_SetRenderState(dev, D3DRENDERSTATE_DITHERENABLE, TRUE);
    log_hr(SUCCEEDED(hr) ? STATUS_PASS : STATUS_FAIL, "D3D dither state", hr);
    hr = IDirect3DDevice2_SetRenderState(dev, D3DRENDERSTATE_FOGENABLE, TRUE);
    log_hr(SUCCEEDED(hr) ? STATUS_PASS : STATUS_FAIL, "D3D fog state", hr);
    IDirect3DDevice2_SetRenderState(dev, D3DRENDERSTATE_FOGENABLE, FALSE);

done:
    release_d3d(d3d, target, dev, vp);
}

static void run_automatic_tests(void)
{
    HRESULT hr;
    DDPIXELFORMAT p565 = pf_rgb565(), p555 = pf_rgb555(), p32 = pf_rgb32();
    const DWORD c565[4] = { 0xF800, 0x07E0, 0x001F, 0xFFFF };
    const DWORD c555[4] = { 0x7C00, 0x03E0, 0x001F, 0x7FFF };
    const DWORD c32[4]  = { 0x00FF0000, 0x0000FF00, 0x000000FF, 0x00FFFFFF };
    char summary[256];

    if (g_app.running) return;
    g_app.running = 1;
    SetWindowTextA(g_app.log_edit, "");
    g_app.pass_count = g_app.fail_count = g_app.skip_count = 0;

    log_line(STATUS_INFO, "R2PTEST", "ATI Rage II+ / Mach64 GTB Windows 95 suite");
    log_line(STATUS_INFO, "Baseline", "Recommended: Windows 95, 800x600, 16 bpp");
    hr = init_ddraw();
    if (FAILED(hr)) {
        log_hr(STATUS_FAIL, "DirectDraw initialization", hr);
        g_app.running = 0;
        return;
    }
    log_line(STATUS_PASS, "DirectDraw initialization", "IDirectDraw2 ready");

    test_caps();
    test_2d_fill_copy();
    test_2d_rop();
    test_2d_colorkey();
    test_rgb_scaler_format("RGB565 StretchBlt", p565, c565);
    test_rgb_scaler_format("RGB555 StretchBlt", p555, c555);
    test_rgb_scaler_format("RGB32 StretchBlt", p32, c32);
    test_yuv_scaler();
    test_d3d_primitives();
    test_d3d_z_alpha();
    test_d3d_texture_caps();

    wsprintfA(summary, "Automatic tests complete. PASS=%d FAIL=%d SKIP=%d\r\n",
              g_app.pass_count, g_app.fail_count, g_app.skip_count);
    append_log_text("\r\n"); append_log_text(summary);
    MessageBoxA(g_app.hwnd, summary, APP_TITLE,
                MB_OK | (g_app.fail_count ? MB_ICONEXCLAMATION : MB_ICONINFORMATION));
    g_app.running = 0;
}

static void run_overlay_visual(void)
{
    LPDIRECTDRAWSURFACE primary = NULL, overlay = NULL;
    DDPIXELFORMAT ypf = pf_fourcc(MAKEFOURCC('Y','U','Y','2'));
    RECT src, dst, client;
    POINT pt;
    HRESULT hr;
    int answer;

    if (FAILED(init_ddraw())) return;
    hr = create_primary(&primary);
    if (FAILED(hr)) { log_hr(STATUS_FAIL, "Overlay primary", hr); goto done; }
    hr = create_surface(160, 120, DDSCAPS_OVERLAY | DDSCAPS_VIDEOMEMORY, &ypf, &overlay);
    if (FAILED(hr)) { log_hr(STATUS_SKIP, "YUY2 overlay create", hr); goto done; }
    if (!fill_yuy2_pattern(overlay, 0)) { log_line(STATUS_FAIL, "YUY2 overlay pattern", "lock failed"); goto done; }
    GetClientRect(g_app.hwnd, &client);
    pt.x = client.left; pt.y = client.top; ClientToScreen(g_app.hwnd, &pt);
    SetRect(&src, 0, 0, 160, 120);
    SetRect(&dst, pt.x + 20, pt.y + 20, pt.x + 340, pt.y + 260);
    hr = IDirectDrawSurface_UpdateOverlay(overlay, &src, primary, &dst, DDOVER_SHOW, NULL);
    if (FAILED(hr)) { log_hr(STATUS_FAIL, "YUY2 overlay UpdateOverlay", hr); goto done; }
    answer = MessageBoxA(g_app.hwnd,
        "Overlay visual check:\n\nDo you see four vertical color blocks (red, green, blue, light gray/white) scaled to roughly 320x240, without purple/green stripes or a one-pixel chroma shift?",
