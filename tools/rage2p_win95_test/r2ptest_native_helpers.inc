/* Native display-format helpers for Win95 DirectDraw validation. */
static DDPIXELFORMAT r2p_native_rgb16(void)
{
    DDSURFACEDESC sd;
    DDPIXELFORMAT fallback = pf_rgb555();
    ZeroMemory(&sd, sizeof(sd));
    sd.dwSize = sizeof(sd);
    if (g_app.dd && SUCCEEDED(IDirectDraw2_GetDisplayMode(g_app.dd, &sd))) {
        if ((sd.ddpfPixelFormat.dwFlags & DDPF_RGB) &&
            sd.ddpfPixelFormat.dwRGBBitCount == 16)
            return sd.ddpfPixelFormat;
    }
    return fallback;
}

static void r2p_log_native_display_format(void)
{
    DDSURFACEDESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.dwSize = sizeof(sd);
    if (!g_app.dd || FAILED(IDirectDraw2_GetDisplayMode(g_app.dd, &sd))) {
        log_line(STATUS_INFO, "Display pixel format", "GetDisplayMode unavailable");
        return;
    }
    log_line(STATUS_INFO, "Display pixel format",
             "%lux%lu %lu bpp R=%08lX G=%08lX B=%08lX",
             sd.dwWidth, sd.dwHeight, sd.ddpfPixelFormat.dwRGBBitCount,
             sd.ddpfPixelFormat.dwRBitMask,
             sd.ddpfPixelFormat.dwGBitMask,
             sd.ddpfPixelFormat.dwBBitMask);
}
