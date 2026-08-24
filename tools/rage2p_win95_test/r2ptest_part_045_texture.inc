/* Extended Direct3D 5 texture rendering tests. */

static DDPIXELFORMAT pf_p8(void)
{
    DDPIXELFORMAT pf;
    ZeroMemory(&pf, sizeof(pf));
    pf.dwSize = sizeof(pf);
    pf.dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8;
    pf.dwRGBBitCount = 8;
    return pf;
}

static DDPIXELFORMAT pf_p4(void)
{
    DDPIXELFORMAT pf;
    ZeroMemory(&pf, sizeof(pf));
    pf.dwSize = sizeof(pf);
    pf.dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED4;
    pf.dwRGBBitCount = 4;
    return pf;
}

static void make_test_palette(PALETTEENTRY *entries, int count)
{
    int i;
    for (i = 0; i < count; i++) {
        entries[i].peRed = 0;
        entries[i].peGreen = 0;
        entries[i].peBlue = 0;
        entries[i].peFlags = 0;
    }
    if (count > 1) { entries[1].peRed = 255; entries[1].peGreen = 0;   entries[1].peBlue = 0; }
    if (count > 2) { entries[2].peRed = 0;   entries[2].peGreen = 255; entries[2].peBlue = 0; }
    if (count > 3) { entries[3].peRed = 0;   entries[3].peGreen = 0;   entries[3].peBlue = 255; }
    if (count > 4) { entries[4].peRed = 255; entries[4].peGreen = 255; entries[4].peBlue = 255; }
}

static int fill_texture_quadrants(LPDIRECTDRAWSURFACE surface, int mode)
{
    static const WORD rgb565[4] = { 0xF800, 0x07E0, 0x001F, 0xFFFF };
    DDSURFACEDESC sd;
    int x, y;

    ZeroMemory(&sd, sizeof(sd));
    sd.dwSize = sizeof(sd);
    if (FAILED(IDirectDrawSurface_Lock(surface, NULL, &sd, DDLOCK_WAIT, NULL)))
        return 0;

    for (y = 0; y < (int)sd.dwHeight; y++) {
        BYTE *row = (BYTE *)sd.lpSurface + y * sd.lPitch;
        if (mode == 2) {
            /* P4: both nibbles receive the same index in each byte. This
             * makes the validation independent of host nibble ordering while
             * still exercising the driver's CI4 texture path. */
            for (x = 0; x < (int)sd.dwWidth; x += 2) {
                int q = (x >= (int)sd.dwWidth / 2 ? 1 : 0) |
                        (y >= (int)sd.dwHeight / 2 ? 2 : 0);
                BYTE idx = (BYTE)(q + 1);
                row[x >> 1] = (BYTE)((idx << 4) | idx);
            }
        } else {
            for (x = 0; x < (int)sd.dwWidth; x++) {
                int q = (x >= (int)sd.dwWidth / 2 ? 1 : 0) |
                        (y >= (int)sd.dwHeight / 2 ? 2 : 0);
                if (mode == 0)
                    ((WORD *)row)[x] = rgb565[q];
                else
                    row[x] = (BYTE)(q + 1);
            }
        }
    }
    IDirectDrawSurface_Unlock(surface, NULL);
    return 1;
}

static int verify_texture_quadrants(LPDIRECTDRAWSURFACE target)
{
    static const WORD expected[4] = { 0xF800, 0x07E0, 0x001F, 0xFFFF };
    static const int sx[4] = { TEST_W / 4, (TEST_W * 3) / 4,
                               TEST_W / 4, (TEST_W * 3) / 4 };
    static const int sy[4] = { TEST_H / 4, TEST_H / 4,
                               (TEST_H * 3) / 4, (TEST_H * 3) / 4 };
    int i;
    for (i = 0; i < 4; i++) {
        DWORD raw = 0;
        int er, eg, eb, ar, ag, ab;
        int distance;
        if (!read_raw_pixel(target, sx[i], sy[i], &raw))
            return 0;
        er = ((expected[i] >> 11) & 31) * 255 / 31;
        eg = ((expected[i] >> 5) & 63) * 255 / 63;
        eb = (expected[i] & 31) * 255 / 31;
        ar = ((raw >> 11) & 31) * 255 / 31;
        ag = ((raw >> 5) & 63) * 255 / 63;
        ab = (raw & 31) * 255 / 31;
        distance = abs(er - ar) + abs(eg - ag) + abs(eb - ab);
        if (distance > 96)
            return 0;
    }
    return 1;
}

static HRESULT draw_texture_quad(LPDIRECT3DDEVICE2 dev,
                                 LPDIRECT3DVIEWPORT2 vp,
                                 LPDIRECT3DTexture2 texture,
                                 int linear)
{
    D3DTEXTUREHANDLE handle = 0;
    D3DTLVERTEX v[6];
    HRESULT hr;

    hr = IDirect3DTexture2_GetHandle(texture, dev, &handle);
    if (FAILED(hr))
        return hr;

    IDirect3DDevice2_SetRenderState(dev, D3DRENDERSTATE_ZENABLE, FALSE);
    IDirect3DDevice2_SetRenderState(dev, D3DRENDERSTATE_ALPHABLENDENABLE, FALSE);
    IDirect3DDevice2_SetRenderState(dev, D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice2_SetRenderState(dev, D3DRENDERSTATE_TEXTUREHANDLE, handle);
    IDirect3DDevice2_SetRenderState(dev, D3DRENDERSTATE_TEXTUREMAPBLEND, D3DTBLEND_COPY);
    IDirect3DDevice2_SetRenderState(dev, D3DRENDERSTATE_TEXTUREMAG,
                                    linear ? D3DFILTER_LINEAR : D3DFILTER_NEAREST);
    IDirect3DDevice2_SetRenderState(dev, D3DRENDERSTATE_TEXTUREMIN,
                                    linear ? D3DFILTER_LINEAR : D3DFILTER_NEAREST);

    clear_view(vp, D3DCLEAR_TARGET);
    set_tl(&v[0], 0,          0,          0.5f, 0xFFFFFFFFUL, 0.0f, 0.0f);
    set_tl(&v[1], TEST_W - 1, 0,          0.5f, 0xFFFFFFFFUL, 1.0f, 0.0f);
    set_tl(&v[2], 0,          TEST_H - 1, 0.5f, 0xFFFFFFFFUL, 0.0f, 1.0f);
    v[3] = v[2];
    v[4] = v[1];
    set_tl(&v[5], TEST_W - 1, TEST_H - 1, 0.5f, 0xFFFFFFFFUL, 1.0f, 1.0f);

    hr = IDirect3DDevice2_BeginScene(dev);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice2_DrawPrimitive(dev, D3DPT_TRIANGLELIST,
                                             D3DVT_TLVERTEX, v, 6, D3DDP_WAIT);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice2_EndScene(dev);
    IDirect3DDevice2_SetRenderState(dev, D3DRENDERSTATE_TEXTUREHANDLE, 0);
    return hr;
}

static void test_texture_render_one(LPDIRECT3DDEVICE2 dev,
                                    LPDIRECT3DVIEWPORT2 vp,
                                    LPDIRECTDRAWSURFACE target,
                                    int mode, int advertised,
                                    const char *label)
{
    DDPIXELFORMAT pf;
    LPDIRECTDRAWSURFACE surface = NULL;
    LPDIRECT3DTEXTURE2 texture = NULL;
    LPDIRECTDRAWPALETTE palette = NULL;
    PALETTEENTRY entries[256];
    HRESULT hr;
    DWORD palette_flags = 0;
    int palette_count = 0;

    if (!advertised) {
        log_line(STATUS_SKIP, label, "format not advertised by HAL");
        return;
    }
    if (mode == 0) pf = pf_rgb565();
    else if (mode == 1) pf = pf_p8();
    else pf = pf_p4();

    hr = create_surface(32, 32, DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY,
                        &pf, &surface);
    if (FAILED(hr)) { log_hr(STATUS_FAIL, label, hr); goto done; }

    if (mode != 0) {
        if (mode == 1) {
            palette_flags = DDPCAPS_8BIT | DDPCAPS_ALLOW256;
            palette_count = 256;
        } else {
            palette_flags = DDPCAPS_4BIT;
            palette_count = 16;
        }
        make_test_palette(entries, palette_count);
        hr = IDirectDraw2_CreatePalette(g_app.dd, palette_flags, entries,
                                         &palette, NULL);
        if (FAILED(hr)) { log_hr(STATUS_FAIL, label, hr); goto done; }
        hr = IDirectDrawSurface_SetPalette(surface, palette);
        if (FAILED(hr)) { log_hr(STATUS_FAIL, label, hr); goto done; }
    }

    if (!fill_texture_quadrants(surface, mode)) {
        log_line(STATUS_FAIL, label, "texture Lock/fill failed");
        goto done;
    }
    hr = IDirectDrawSurface_QueryInterface(surface, &IID_IDirect3DTexture2,
                                            (void **)&texture);
    if (FAILED(hr)) { log_hr(STATUS_FAIL, label, hr); goto done; }

    hr = draw_texture_quad(dev, vp, texture, 0);
    if (FAILED(hr) || !verify_texture_quadrants(target))
        log_hr(STATUS_FAIL, label, hr);
    else
        log_line(STATUS_PASS, label, "point-filtered quadrant sampling correct");

    hr = draw_texture_quad(dev, vp, texture, 1);
    if (FAILED(hr) || !verify_texture_quadrants(target))
        log_hr(STATUS_FAIL, mode == 0 ? "RGB565 bilinear texture" :
               (mode == 1 ? "CI8 bilinear texture" : "CI4 bilinear texture"), hr);
    else
        log_line(STATUS_PASS, mode == 0 ? "RGB565 bilinear texture" :
                 (mode == 1 ? "CI8 bilinear texture" : "CI4 bilinear texture"),
                 "bilinear sampling preserved block interiors");

done:
    if (texture) IDirect3DTexture2_Release(texture);
    if (palette) IDirectDrawPalette_Release(palette);
    if (surface) IDirectDrawSurface_Release(surface);
}

static void test_d3d_texture_rendering(void)
{
    LPDIRECT3D2 d3d = NULL;
    LPDIRECTDRAWSURFACE target = NULL;
    LPDIRECT3DDEVICE2 dev = NULL;
    LPDIRECT3DVIEWPORT2 vp = NULL;
    D3DEnumState es;
    TextureCapsState tc;
    HRESULT hr;

    ZeroMemory(&tc, sizeof(tc));
    if (!init_d3d(&d3d, &target, &dev, &vp, &es))
        goto done;
    hr = IDirect3DDevice2_EnumTextureFormats(dev, enum_texture_formats, &tc);
    if (FAILED(hr)) {
        log_hr(STATUS_FAIL, "Texture render enumeration", hr);
        goto done;
    }

    test_texture_render_one(dev, vp, target, 0, tc.rgb565,
                            "RGB565 texture render");
    test_texture_render_one(dev, vp, target, 1, tc.p8,
                            "CI8/P8 texture render");
    test_texture_render_one(dev, vp, target, 2, tc.p4,
                            "CI4/P4 texture render");

done:
    release_d3d(d3d, target, dev, vp);
}

static void run_automatic_tests(void)
{
    char summary[256];
    int before_fail;

    r2p_run_automatic_tests_base();
    if (!g_app.dd)
        return;

    before_fail = g_app.fail_count;
    append_log_text("\r\n--- Extended D3D texture rendering ---\r\n");
    test_d3d_texture_rendering();
    wsprintfA(summary,
              "Texture render tests complete. Total PASS=%d FAIL=%d SKIP=%d\r\n",
              g_app.pass_count, g_app.fail_count, g_app.skip_count);
    append_log_text(summary);
    MessageBoxA(g_app.hwnd, summary, APP_TITLE,
                MB_OK | ((g_app.fail_count > before_fail) ?
                         MB_ICONEXCLAMATION : MB_ICONINFORMATION));
}
