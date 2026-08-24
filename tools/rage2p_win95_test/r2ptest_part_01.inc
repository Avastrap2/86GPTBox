    pf.dwBBitMask = 0x000000FF;
    return pf;
}

static DDPIXELFORMAT pf_fourcc(DWORD fourcc)
{
    DDPIXELFORMAT pf;
    ZeroMemory(&pf, sizeof(pf));
    pf.dwSize = sizeof(pf);
    pf.dwFlags = DDPF_FOURCC;
    pf.dwFourCC = fourcc;
    return pf;
}

static HRESULT create_surface(int w, int h, DWORD caps, const DDPIXELFORMAT *pf,
                              LPDIRECTDRAWSURFACE *out)
{
    DDSURFACEDESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.dwSize = sizeof(sd);
    sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    sd.ddsCaps.dwCaps = caps;
    sd.dwWidth = (DWORD)w;
    sd.dwHeight = (DWORD)h;
    if (pf) {
        sd.dwFlags |= DDSD_PIXELFORMAT;
        sd.ddpfPixelFormat = *pf;
    }
    return IDirectDraw2_CreateSurface(g_app.dd, &sd, out, NULL);
}

static HRESULT create_primary(LPDIRECTDRAWSURFACE *out)
{
    DDSURFACEDESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.dwSize = sizeof(sd);
    sd.dwFlags = DDSD_CAPS;
    sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    return IDirectDraw2_CreateSurface(g_app.dd, &sd, out, NULL);
}

static HRESULT fill_surface_color(LPDIRECTDRAWSURFACE s, DWORD raw)
{
    DDBLTFX fx;
    ZeroMemory(&fx, sizeof(fx));
    fx.dwSize = sizeof(fx);
    fx.dwFillColor = raw;
    return IDirectDrawSurface_Blt(s, NULL, NULL, NULL,
                                  DDBLT_COLORFILL | DDBLT_WAIT, &fx);
}

static int read_raw_pixel(LPDIRECTDRAWSURFACE s, int x, int y, DWORD *value)
{
    DDSURFACEDESC sd;
    BYTE *row;
    ZeroMemory(&sd, sizeof(sd));
    sd.dwSize = sizeof(sd);
    if (FAILED(IDirectDrawSurface_Lock(s, NULL, &sd, DDLOCK_WAIT, NULL)))
        return 0;
    row = (BYTE *)sd.lpSurface + y * sd.lPitch;
    if (sd.ddpfPixelFormat.dwRGBBitCount == 16)
        *value = ((WORD *)row)[x];
    else if (sd.ddpfPixelFormat.dwRGBBitCount == 32)
        *value = ((DWORD *)row)[x];
    else if (sd.ddpfPixelFormat.dwRGBBitCount == 8)
        *value = row[x];
    else {
        IDirectDrawSurface_Unlock(s, NULL);
        return 0;
    }
    IDirectDrawSurface_Unlock(s, NULL);
    return 1;
}

static int write_quadrants(LPDIRECTDRAWSURFACE s, const DWORD colors[4])
{
    DDSURFACEDESC sd;
    int x, y;
    ZeroMemory(&sd, sizeof(sd));
    sd.dwSize = sizeof(sd);
    if (FAILED(IDirectDrawSurface_Lock(s, NULL, &sd, DDLOCK_WAIT, NULL)))
        return 0;
    for (y = 0; y < (int)sd.dwHeight; y++) {
        BYTE *row = (BYTE *)sd.lpSurface + y * sd.lPitch;
        for (x = 0; x < (int)sd.dwWidth; x++) {
            int q = (x >= (int)sd.dwWidth / 2 ? 1 : 0) |
                    (y >= (int)sd.dwHeight / 2 ? 2 : 0);
            if (sd.ddpfPixelFormat.dwRGBBitCount == 16)
                ((WORD *)row)[x] = (WORD)colors[q];
            else if (sd.ddpfPixelFormat.dwRGBBitCount == 32)
                ((DWORD *)row)[x] = colors[q];
        }
    }
    IDirectDrawSurface_Unlock(s, NULL);
    return 1;
}

static int verify_quadrant_samples(LPDIRECTDRAWSURFACE s,
                                   const DWORD colors[4], int w, int h)
{
    static const int qx[4] = { 1, 3, 1, 3 };
    static const int qy[4] = { 1, 1, 3, 3 };
    int i;
    for (i = 0; i < 4; i++) {
        DWORD got = 0;
        int x = (w * qx[i]) / 4;
        int y = (h * qy[i]) / 4;
        if (!read_raw_pixel(s, x, y, &got) || got != colors[i])
            return 0;
    }
    return 1;
}

static void test_caps(void)
{
    DDCAPS hw, hel;
    HRESULT hr;
    ZeroMemory(&hw, sizeof(hw));
    ZeroMemory(&hel, sizeof(hel));
    hw.dwSize = sizeof(hw);
    hel.dwSize = sizeof(hel);
    hr = IDirectDraw2_GetCaps(g_app.dd, &hw, &hel);
    if (FAILED(hr)) {
        log_hr(STATUS_FAIL, "DirectDraw GetCaps", hr);
        return;
    }
    log_line(STATUS_PASS, "DirectDraw GetCaps", "caps=%08lX caps2=%08lX", hw.dwCaps, hw.dwCaps2);
    log_line((hw.dwCaps & DDCAPS_BLT) ? STATUS_PASS : STATUS_FAIL,
             "Hardware BLT capability", "dwCaps=%08lX", hw.dwCaps);
    log_line((hw.dwCaps & DDCAPS_BLTSTRETCH) ? STATUS_PASS : STATUS_FAIL,
             "Hardware StretchBlt capability", "dwCaps=%08lX", hw.dwCaps);
    log_line((hw.dwCaps & DDCAPS_COLORKEY) ? STATUS_PASS : STATUS_FAIL,
             "Hardware color key capability", "dwCaps=%08lX", hw.dwCaps);
    log_line((hw.dwCaps & DDCAPS_OVERLAY) ? STATUS_PASS : STATUS_SKIP,
             "Overlay capability", "dwCaps=%08lX", hw.dwCaps);
    log_line((hw.dwCaps & DDCAPS_3D) ? STATUS_PASS : STATUS_FAIL,
             "Direct3D capability", "dwCaps=%08lX", hw.dwCaps);
}

static void test_2d_fill_copy(void)
{
    DDPIXELFORMAT pf = pf_rgb565();
    LPDIRECTDRAWSURFACE a = NULL, b = NULL;
    HRESULT hr;
    DWORD got;

    hr = create_surface(64, 64, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY, &pf, &a);
    if (FAILED(hr)) {
        log_hr(STATUS_FAIL, "2D create video surface", hr);
        goto done;
    }
    hr = create_surface(64, 64, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY, &pf, &b);
    if (FAILED(hr)) {
        log_hr(STATUS_FAIL, "2D create destination", hr);
        goto done;
    }

    hr = fill_surface_color(a, 0xF800);
    if (FAILED(hr) || !read_raw_pixel(a, 31, 31, &got) || got != 0xF800)
        log_hr(STATUS_FAIL, "2D color fill", hr);
    else
        log_line(STATUS_PASS, "2D color fill", "RGB565 red=%04lX", got);

    hr = fill_surface_color(b, 0x001F);
    if (SUCCEEDED(hr))
        hr = IDirectDrawSurface_Blt(b, NULL, a, NULL, DDBLT_WAIT, NULL);
    if (FAILED(hr) || !read_raw_pixel(b, 31, 31, &got) || got != 0xF800)
        log_hr(STATUS_FAIL, "2D SRCCOPY BLT", hr);
    else
        log_line(STATUS_PASS, "2D SRCCOPY BLT", "pixel=%04lX", got);

done:
    if (b) IDirectDrawSurface_Release(b);
    if (a) IDirectDrawSurface_Release(a);
}

static void test_2d_rop(void)
{
    DDPIXELFORMAT pf = pf_rgb565();
    LPDIRECTDRAWSURFACE src = NULL, dst = NULL;
