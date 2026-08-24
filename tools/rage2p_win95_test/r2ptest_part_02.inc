    DDBLTFX fx;
    HRESULT hr;
    DWORD got;

    hr = create_surface(32, 32, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY, &pf, &src);
    if (FAILED(hr)) { log_hr(STATUS_FAIL, "2D ROP source", hr); goto done; }
    hr = create_surface(32, 32, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY, &pf, &dst);
    if (FAILED(hr)) { log_hr(STATUS_FAIL, "2D ROP destination", hr); goto done; }
    fill_surface_color(src, 0x0F0F);
    fill_surface_color(dst, 0x00FF);
    ZeroMemory(&fx, sizeof(fx));
    fx.dwSize = sizeof(fx);
    fx.dwROP = 0x00660046L; /* SRCINVERT */
    hr = IDirectDrawSurface_Blt(dst, NULL, src, NULL, DDBLT_ROP | DDBLT_WAIT, &fx);
    if (FAILED(hr) || !read_raw_pixel(dst, 10, 10, &got) || (got & 0xFFFF) != (0x0F0F ^ 0x00FF))
        log_hr(STATUS_FAIL, "2D ROP SRCINVERT", hr);
    else
        log_line(STATUS_PASS, "2D ROP SRCINVERT", "pixel=%04lX", got);

done:
    if (dst) IDirectDrawSurface_Release(dst);
    if (src) IDirectDrawSurface_Release(src);
}

static void test_2d_colorkey(void)
{
    DDPIXELFORMAT pf = pf_rgb565();
    LPDIRECTDRAWSURFACE src = NULL, dst = NULL;
    DDCOLORKEY key;
    DDSURFACEDESC sd;
    HRESULT hr;
    int x, y;
    DWORD a, b;

    hr = create_surface(32, 32, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY, &pf, &src);
    if (FAILED(hr)) { log_hr(STATUS_FAIL, "ColorKey source create", hr); goto done; }
    hr = create_surface(32, 32, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY, &pf, &dst);
    if (FAILED(hr)) { log_hr(STATUS_FAIL, "ColorKey dest create", hr); goto done; }

    ZeroMemory(&sd, sizeof(sd)); sd.dwSize = sizeof(sd);
    hr = IDirectDrawSurface_Lock(src, NULL, &sd, DDLOCK_WAIT, NULL);
    if (FAILED(hr)) { log_hr(STATUS_FAIL, "ColorKey source lock", hr); goto done; }
    for (y = 0; y < 32; y++) {
        WORD *row = (WORD *)((BYTE *)sd.lpSurface + y * sd.lPitch);
        for (x = 0; x < 32; x++)
            row[x] = (x < 16) ? 0xF81F : 0x07E0; /* magenta key / green */
    }
    IDirectDrawSurface_Unlock(src, NULL);
    fill_surface_color(dst, 0x001F); /* blue */
    key.dwColorSpaceLowValue = 0xF81F;
    key.dwColorSpaceHighValue = 0xF81F;
    hr = IDirectDrawSurface_SetColorKey(src, DDCKEY_SRCBLT, &key);
    if (SUCCEEDED(hr))
        hr = IDirectDrawSurface_Blt(dst, NULL, src, NULL, DDBLT_KEYSRC | DDBLT_WAIT, NULL);
    if (FAILED(hr) || !read_raw_pixel(dst, 8, 16, &a) || !read_raw_pixel(dst, 24, 16, &b) || a != 0x001F || b != 0x07E0)
        log_hr(STATUS_FAIL, "Source color key BLT", hr);
    else
        log_line(STATUS_PASS, "Source color key BLT", "key stayed blue, green copied");

    /* Destination key: only blue half should accept red source. */
    ZeroMemory(&sd, sizeof(sd)); sd.dwSize = sizeof(sd);
    hr = IDirectDrawSurface_Lock(dst, NULL, &sd, DDLOCK_WAIT, NULL);
    if (SUCCEEDED(hr)) {
        for (y = 0; y < 32; y++) {
            WORD *row = (WORD *)((BYTE *)sd.lpSurface + y * sd.lPitch);
            for (x = 0; x < 32; x++) row[x] = (x < 16) ? 0x001F : 0x07E0;
        }
        IDirectDrawSurface_Unlock(dst, NULL);
    }
    fill_surface_color(src, 0xF800);
    key.dwColorSpaceLowValue = 0x001F;
    key.dwColorSpaceHighValue = 0x001F;
    hr = IDirectDrawSurface_SetColorKey(dst, DDCKEY_DESTBLT, &key);
    if (SUCCEEDED(hr))
        hr = IDirectDrawSurface_Blt(dst, NULL, src, NULL, DDBLT_KEYDEST | DDBLT_WAIT, NULL);
    if (FAILED(hr) || !read_raw_pixel(dst, 8, 16, &a) || !read_raw_pixel(dst, 24, 16, &b) || a != 0xF800 || b != 0x07E0)
        log_hr(STATUS_FAIL, "Destination color key BLT", hr);
    else
        log_line(STATUS_PASS, "Destination color key BLT", "blue replaced, green preserved");

done:
    if (dst) IDirectDrawSurface_Release(dst);
    if (src) IDirectDrawSurface_Release(src);
}

static void test_rgb_scaler_format(const char *name, DDPIXELFORMAT pf,
                                   const DWORD colors[4])
{
    LPDIRECTDRAWSURFACE src = NULL, dst = NULL;
    RECT sr, dr;
    HRESULT hr;

    hr = create_surface(32, 24, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY, &pf, &src);
    if (FAILED(hr)) { log_hr(STATUS_FAIL, name, hr); goto done; }
    hr = create_surface(64, 48, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY, &pf, &dst);
    if (FAILED(hr)) { log_hr(STATUS_FAIL, name, hr); goto done; }
    if (!write_quadrants(src, colors)) { log_line(STATUS_FAIL, name, "source lock failed"); goto done; }
    SetRect(&sr, 0, 0, 32, 24); SetRect(&dr, 0, 0, 64, 48);
    hr = IDirectDrawSurface_Blt(dst, &dr, src, &sr, DDBLT_WAIT, NULL);
    if (FAILED(hr) || !verify_quadrant_samples(dst, colors, 64, 48))
        log_hr(STATUS_FAIL, name, hr);
    else
        log_line(STATUS_PASS, name, "32x24 -> 64x48");

done:
    if (dst) IDirectDrawSurface_Release(dst);
    if (src) IDirectDrawSurface_Release(src);
}

static int clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }
static WORD rgb_to_565(int r, int g, int b)
{
    return (WORD)(((clamp8(r) >> 3) << 11) | ((clamp8(g) >> 2) << 5) | (clamp8(b) >> 3));
}
static void yuv_to_rgb(int y, int u, int v, int *r, int *g, int *b)
{
    int d = u - 128, e = v - 128;
    *r = clamp8(y + ((359 * e) >> 8));
    *g = clamp8(y - ((88 * d + 183 * e) >> 8));
    *b = clamp8(y + ((454 * d) >> 8));
}

static int fill_yuy2_pattern(LPDIRECTDRAWSURFACE s, int uyvy)
{
    static const BYTE Y[4] = { 76, 150, 29, 240 };
    static const BYTE U[4] = { 85, 44, 255, 128 };
    static const BYTE V[4] = { 255, 21, 107, 128 };
    DDSURFACEDESC sd;
    int x, y;
    ZeroMemory(&sd, sizeof(sd)); sd.dwSize = sizeof(sd);
    if (FAILED(IDirectDrawSurface_Lock(s, NULL, &sd, DDLOCK_WAIT, NULL)))
        return 0;
    for (y = 0; y < (int)sd.dwHeight; y++) {
        BYTE *row = (BYTE *)sd.lpSurface + y * sd.lPitch;
        for (x = 0; x < (int)sd.dwWidth; x += 2) {
            int q = (x * 4) / (int)sd.dwWidth;
            if (q > 3) q = 3;
            if (!uyvy) {
                row[x * 2 + 0] = Y[q]; row[x * 2 + 1] = U[q];
                row[x * 2 + 2] = Y[q]; row[x * 2 + 3] = V[q];
            } else {
                row[x * 2 + 0] = U[q]; row[x * 2 + 1] = Y[q];
                row[x * 2 + 2] = V[q]; row[x * 2 + 3] = Y[q];
            }
        }
    }
    IDirectDrawSurface_Unlock(s, NULL);
    return 1;
}

static int verify_yuv_blocks(LPDIRECTDRAWSURFACE dst, int w, int h)
{
    static const BYTE Y[4] = { 76, 150, 29, 240 };
    static const BYTE U[4] = { 85, 44, 255, 128 };
    static const BYTE V[4] = { 255, 21, 107, 128 };
    int q;
    for (q = 0; q < 4; q++) {
        int r, g, b, ar, ag, ab;
        DWORD raw;
        int x = (w * (q * 2 + 1)) / 8;
        int y = h / 2;
        int dist;
        if (!read_raw_pixel(dst, x, y, &raw)) return 0;
        r = ((raw >> 11) & 31) * 255 / 31;
        g = ((raw >> 5) & 63) * 255 / 63;
        b = (raw & 31) * 255 / 31;
        yuv_to_rgb(Y[q], U[q], V[q], &ar, &ag, &ab);
        dist = abs(r - ar) + abs(g - ag) + abs(b - ab);
        if (dist > 100)
            return 0;
    }
    return 1;
}

static void test_yuv_scaler_one(DWORD fourcc, int uyvy, const char *label,
                                int dw, int dh, int odd_crop)
{
    DDPIXELFORMAT ypf = pf_fourcc(fourcc);
    DDPIXELFORMAT rgb = pf_rgb565();
    LPDIRECTDRAWSURFACE src = NULL, dst = NULL;
