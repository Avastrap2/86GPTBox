    RECT sr, dr;
    HRESULT hr;

    hr = create_surface(32, 16, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY, &ypf, &src);
    if (FAILED(hr)) {
        log_hr(STATUS_SKIP, label, hr);
        goto done;
    }
    hr = create_surface(dw, dh, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY, &rgb, &dst);
    if (FAILED(hr)) { log_hr(STATUS_FAIL, label, hr); goto done; }
    if (!fill_yuy2_pattern(src, uyvy)) { log_line(STATUS_FAIL, label, "source lock failed"); goto done; }
    if (odd_crop) SetRect(&sr, 1, 0, 31, 16); else SetRect(&sr, 0, 0, 32, 16);
    SetRect(&dr, 0, 0, dw, dh);
    hr = IDirectDrawSurface_Blt(dst, &dr, src, &sr, DDBLT_WAIT, NULL);
    if (FAILED(hr)) {
        log_hr(odd_crop ? STATUS_SKIP : STATUS_FAIL, label, hr);
        goto done;
    }
    if (odd_crop) {
        /* The exact phase is driver-defined for odd packed-YUV crop. Successful
         * accelerated conversion without a fault is the regression criterion. */
        log_line(STATUS_PASS, label, "odd X crop accepted");
    } else if (!verify_yuv_blocks(dst, dw, dh)) {
        log_line(STATUS_FAIL, label, "RGB result does not match YUV block colors");
    } else {
        log_line(STATUS_PASS, label, "%dx16 -> %dx%d", 32, dw, dh);
    }

done:
    if (dst) IDirectDrawSurface_Release(dst);
    if (src) IDirectDrawSurface_Release(src);
}

static void test_yuv_scaler(void)
{
    DWORD yuy2 = MAKEFOURCC('Y','U','Y','2');
    DWORD uyvy = MAKEFOURCC('U','Y','V','Y');
    test_yuv_scaler_one(yuy2, 0, "YUY2 conversion 1:1", 32, 16, 0);
    test_yuv_scaler_one(yuy2, 0, "YUY2 scaler 2x", 64, 32, 0);
    test_yuv_scaler_one(yuy2, 0, "YUY2 scaler noninteger", 50, 25, 0);
    test_yuv_scaler_one(yuy2, 0, "YUY2 odd-X crop", 60, 32, 1);
    test_yuv_scaler_one(uyvy, 1, "UYVY conversion/scaler", 64, 32, 0);
}

typedef struct D3DEnumState {
    int found;
    GUID guid;
    char name[128];
    D3DDEVICEDESC hw;
} D3DEnumState;

static HRESULT CALLBACK enum_d3d_devices(GUID *guid, LPSTR desc, LPSTR name,
                                         LPD3DDEVICEDESC hw,
                                         LPD3DDEVICEDESC hel, LPVOID context)
{
    D3DEnumState *st = (D3DEnumState *)context;
    (void)desc; (void)hel;
    if (st->found || !hw || !hw->dwFlags)
        return D3DENUMRET_OK;
    st->found = 1;
    st->guid = *guid;
    st->hw = *hw;
    lstrcpynA(st->name, name ? name : "HAL", sizeof(st->name));
    return D3DENUMRET_CANCEL;
}

static int init_d3d(LPDIRECT3D2 *out_d3d, LPDIRECTDRAWSURFACE *out_target,
                    LPDIRECT3DDEVICE2 *out_dev, LPDIRECT3DVIEWPORT2 *out_vp,
                    D3DEnumState *enum_state)
{
    HRESULT hr;
    DDPIXELFORMAT pf = pf_rgb565();
    D3DVIEWPORT2 vpdata;
    *out_d3d = NULL; *out_target = NULL; *out_dev = NULL; *out_vp = NULL;
    ZeroMemory(enum_state, sizeof(*enum_state));

    hr = IDirectDraw2_QueryInterface(g_app.dd, &IID_IDirect3D2, (void **)out_d3d);
    if (FAILED(hr)) { log_hr(STATUS_FAIL, "Direct3D2 QueryInterface", hr); return 0; }
    hr = IDirect3D2_EnumDevices(*out_d3d, enum_d3d_devices, enum_state);
    if (FAILED(hr) || !enum_state->found) {
        log_hr(STATUS_FAIL, "Direct3D HAL enumeration", hr);
        return 0;
    }
    log_line(STATUS_PASS, "Direct3D HAL enumeration", "%s", enum_state->name);

    hr = create_surface(TEST_W, TEST_H,
                        DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY,
                        &pf, out_target);
    if (FAILED(hr)) { log_hr(STATUS_FAIL, "D3D render target", hr); return 0; }
    hr = IDirect3D2_CreateDevice(*out_d3d, &enum_state->guid, *out_target, out_dev);
    if (FAILED(hr)) { log_hr(STATUS_FAIL, "D3D HAL CreateDevice", hr); return 0; }
    log_line(STATUS_PASS, "D3D HAL CreateDevice", "320x240 RGB565 target");

    hr = IDirect3D2_CreateViewport(*out_d3d, out_vp, NULL);
    if (FAILED(hr)) { log_hr(STATUS_FAIL, "D3D CreateViewport", hr); return 0; }
    hr = IDirect3DDevice2_AddViewport(*out_dev, *out_vp);
    if (FAILED(hr)) { log_hr(STATUS_FAIL, "D3D AddViewport", hr); return 0; }
    ZeroMemory(&vpdata, sizeof(vpdata));
    vpdata.dwSize = sizeof(vpdata);
    vpdata.dwX = 0; vpdata.dwY = 0;
    vpdata.dwWidth = TEST_W; vpdata.dwHeight = TEST_H;
    vpdata.dvClipX = -1.0f; vpdata.dvClipY = 1.0f;
    vpdata.dvClipWidth = 2.0f; vpdata.dvClipHeight = 2.0f;
    vpdata.dvMinZ = 0.0f; vpdata.dvMaxZ = 1.0f;
    hr = IDirect3DViewport2_SetViewport2(*out_vp, &vpdata);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice2_SetCurrentViewport(*out_dev, *out_vp);
    if (FAILED(hr)) { log_hr(STATUS_FAIL, "D3D SetViewport", hr); return 0; }
    return 1;
}

static void release_d3d(LPDIRECT3D2 d3d, LPDIRECTDRAWSURFACE target,
                        LPDIRECT3DDEVICE2 dev, LPDIRECT3DVIEWPORT2 vp)
{
    if (dev && vp) IDirect3DDevice2_DeleteViewport(dev, vp);
    if (vp) IDirect3DViewport2_Release(vp);
    if (dev) IDirect3DDevice2_Release(dev);
    if (target) IDirectDrawSurface_Release(target);
    if (d3d) IDirect3D2_Release(d3d);
}

static void set_tl(D3DTLVERTEX *v, float x, float y, float z,
                   DWORD color, float tu, float tv)
{
    ZeroMemory(v, sizeof(*v));
    v->sx = x; v->sy = y; v->sz = z; v->rhw = 1.0f;
    v->color = color; v->specular = 0; v->tu = tu; v->tv = tv;
}

static HRESULT clear_view(LPDIRECT3DVIEWPORT2 vp, DWORD flags)
{
    D3DRECT r;
    r.x1 = 0; r.y1 = 0; r.x2 = TEST_W; r.y2 = TEST_H;
    return IDirect3DViewport2_Clear(vp, 1, &r, flags);
}

static void test_d3d_primitives(void)
{
    LPDIRECT3D2 d3d = NULL;
    LPDIRECTDRAWSURFACE target = NULL;
    LPDIRECT3DDEVICE2 dev = NULL;
    LPDIRECT3DVIEWPORT2 vp = NULL;
    D3DEnumState es;
    D3DTLVERTEX tri[3], line[2], point[1];
    HRESULT hr;
    DWORD raw = 0;

    if (!init_d3d(&d3d, &target, &dev, &vp, &es)) goto done;
    IDirect3DDevice2_SetRenderState(dev, D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice2_SetRenderState(dev, D3DRENDERSTATE_SHADEMODE, D3DSHADE_GOURAUD);
    clear_view(vp, D3DCLEAR_TARGET);
    set_tl(&tri[0], 40, 200, 0.5f, 0xFFFF0000UL, 0, 0);
    set_tl(&tri[1], 160, 20, 0.5f, 0xFF00FF00UL, 0, 0);
    set_tl(&tri[2], 280, 200, 0.5f, 0xFF0000FFUL, 0, 0);
    hr = IDirect3DDevice2_BeginScene(dev);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice2_DrawPrimitive(dev, D3DPT_TRIANGLELIST,
                                                            D3DVT_TLVERTEX, tri, 3, D3DDP_WAIT);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice2_EndScene(dev);
    if (FAILED(hr) || !read_raw_pixel(target, 160, 110, &raw) || raw == 0)
        log_hr(STATUS_FAIL, "D3D Gouraud triangle", hr);
    else
        log_line(STATUS_PASS, "D3D Gouraud triangle", "center=%04lX", raw);

    set_tl(&line[0], 20, 120, 0.5f, 0xFFFFFFFFUL, 0, 0);
    set_tl(&line[1], 300, 120, 0.5f, 0xFFFFFFFFUL, 0, 0);
    hr = IDirect3DDevice2_BeginScene(dev);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice2_DrawPrimitive(dev, D3DPT_LINELIST,
                                                            D3DVT_TLVERTEX, line, 2, D3DDP_WAIT);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice2_EndScene(dev);
    log_hr(SUCCEEDED(hr) ? STATUS_PASS : STATUS_FAIL, "D3D line primitive", hr);

    set_tl(&point[0], 100, 100, 0.5f, 0xFFFFFFFFUL, 0, 0);
    hr = IDirect3DDevice2_BeginScene(dev);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice2_DrawPrimitive(dev, D3DPT_POINTLIST,
                                                            D3DVT_TLVERTEX, point, 1, D3DDP_WAIT);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice2_EndScene(dev);
    log_hr(SUCCEEDED(hr) ? STATUS_PASS : STATUS_FAIL, "D3D point primitive", hr);

done:
    release_d3d(d3d, target, dev, vp);
}
