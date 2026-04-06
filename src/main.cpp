#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <winioctl.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dxgi.h>
#include <dxgi1_4.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "pdh.lib")


// -----------------------------------------------------------------------
// Undocumented Windows 10/11 composition API (user32.dll)
// -----------------------------------------------------------------------
enum ACCENT_STATE
{
    ACCENT_DISABLED                   = 0,
    ACCENT_ENABLE_GRADIENT            = 1,
    ACCENT_ENABLE_TRANSPARENTGRADIENT = 2,
    ACCENT_ENABLE_BLURBEHIND          = 3,
    ACCENT_ENABLE_ACRYLICBLURBEHIND   = 4,
    ACCENT_INVALID_STATE              = 5
};

struct ACCENT_POLICY
{
    ACCENT_STATE AccentState;
    DWORD        AccentFlags;
    DWORD        GradientColor;
    DWORD        AnimationId;
};

enum WINDOWCOMPOSITIONATTRIB { WCA_ACCENT_POLICY = 19 };

struct WINDOWCOMPOSITIONATTRIBDATA
{
    WINDOWCOMPOSITIONATTRIB Attribute;
    PVOID                   pvData;
    SIZE_T                  cbData;
};

using PFN_SetWindowCompositionAttribute =
    BOOL(WINAPI*)(HWND, WINDOWCOMPOSITIONATTRIBDATA*);

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------
static void ApplyBlur(HWND hwnd)
{
    auto pfn = reinterpret_cast<PFN_SetWindowCompositionAttribute>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"),
                       "SetWindowCompositionAttribute"));
    if (pfn)
    {
        auto tryAccent = [&](ACCENT_STATE s) -> bool
        {
            ACCENT_POLICY p = {};
            p.AccentState   = s;
            p.GradientColor = 0x00FFFFFF;
            WINDOWCOMPOSITIONATTRIBDATA d = {};
            d.Attribute = WCA_ACCENT_POLICY;
            d.pvData    = &p;
            d.cbData    = sizeof(p);
            return pfn(hwnd, &d) != FALSE;
        };
        if (tryAccent(ACCENT_ENABLE_ACRYLICBLURBEHIND)) return;
        if (tryAccent(ACCENT_ENABLE_BLURBEHIND))        return;
    }
    MARGINS m = { -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(hwnd, &m);
}

// -----------------------------------------------------------------------
// D2D / DirectWrite state
// -----------------------------------------------------------------------
static ID2D1Factory*        g_pD2DFactory = nullptr;
static IDWriteFactory*      g_pDWFactory  = nullptr;
static IDWriteTextFormat*   g_pChartFmtL  = nullptr;   // left/top  – chart name
static IDWriteTextFormat*   g_pChartFmtR  = nullptr;   // right/top – chart value
static ID2D1DCRenderTarget* g_pDCRT       = nullptr;
static float                g_fontSize    = 14.f;

// -----------------------------------------------------------------------
// Chart
// -----------------------------------------------------------------------
static const int CHART_SAMPLES = 300;

struct Chart
{
    wchar_t name[256];
    double  values[CHART_SAMPLES];
    int     head;
    int     count;
    double  current;
    wchar_t absStr[64];     // absolute value string, updated each sample
    double  displayCurrent; // shown in label, updated once per second
    wchar_t displayAbsStr[64];
};

static const int NUM_CHARTS = 4;
static Chart g_charts[NUM_CHARTS]; // 0=CPU, 1=GPU, 2=RAM, 3=Disk

static void PushChartValue(Chart& c, double v)
{
    c.values[c.head] = v;
    c.head  = (c.head + 1) % CHART_SAMPLES;
    if (c.count < CHART_SAMPLES) c.count++;
    c.current = v;
}

static void FlushDisplayValues()
{
    for (int i = 0; i < NUM_CHARTS; ++i)
    {
        g_charts[i].displayCurrent = g_charts[i].current;
        wcscpy_s(g_charts[i].displayAbsStr, g_charts[i].absStr);
    }
}

// Draw chart inside |area|.
// Layout: label strip (name left, value right) sits above the bordered plot area.
static void DrawChart(ID2D1RenderTarget* rt, const Chart& c,
                      D2D1_RECT_F area,
                      IDWriteTextFormat* fmtL, IDWriteTextFormat* fmtR)
{
    const float margin = 8.f;
    const float labelH = g_fontSize * 1.4f;

    D2D1_RECT_F labelArea = { area.left + margin, area.top, area.right - margin, area.top + labelH };
    D2D1_RECT_F plotArea  = { area.left + margin, area.top + labelH, area.right - margin, area.bottom };

    ID2D1SolidColorBrush* pBrush = nullptr;
    if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0.f, 0.f, 0.f, 1.f), &pBrush)))
        return;

    // --- Labels -------------------------------------------------------
    if (fmtL)
        rt->DrawText(c.name, (UINT32)wcslen(c.name), fmtL, labelArea, pBrush);

    if (fmtR)
    {
        wchar_t valBuf[128];
        if (c.displayAbsStr[0])
            swprintf_s(valBuf, L"%.1f%% / %s", c.displayCurrent * 100.0, c.displayAbsStr);
        else
            swprintf_s(valBuf, L"%.1f%%", c.displayCurrent * 100.0);
        rt->DrawText(valBuf, (UINT32)wcslen(valBuf), fmtR, labelArea, pBrush);
    }

    // --- Border -------------------------------------------------------
    rt->DrawRectangle(plotArea, pBrush, 0.75f);

    // --- Polyline -----------------------------------------------------
    const int n = c.count;
    if (n >= 2)
    {
        const float left   = plotArea.left   + 2.f;
        const float right  = plotArea.right  - 2.f;
        const float top    = plotArea.top    + 2.f;
        const float bottom = plotArea.bottom - 2.f;
        const float pw     = right  - left;
        const float ph     = bottom - top;

        const int start = (c.head - n + CHART_SAMPLES) % CHART_SAMPLES;

        for (int i = 1; i < n; ++i)
        {
            const int i0 = (start + i - 1) % CHART_SAMPLES;
            const int i1 = (start + i)     % CHART_SAMPLES;

            const float x0 = left + (float)(i - 1) / (n - 1) * pw;
            const float y0 = bottom - (float)c.values[i0] * ph;
            const float x1 = left + (float)i         / (n - 1) * pw;
            const float y1 = bottom - (float)c.values[i1] * ph;

            rt->DrawLine({ x0, y0 }, { x1, y1 }, pBrush, 0.75f);
        }
    }

    pBrush->Release();
}

// -----------------------------------------------------------------------
// System metrics state
// -----------------------------------------------------------------------
static ULONGLONG    g_cpuPrevIdle  = 0;
static ULONGLONG    g_cpuPrevTotal = 0;

static int          g_labelTick     = 0;
static PDH_HQUERY   g_pdhQuery      = NULL;
static PDH_HCOUNTER g_pdhGpuCtr    = NULL;
static PDH_HCOUNTER g_pdhDiskCtr   = NULL;
static PDH_HCOUNTER g_pdhCpuFreqCtr  = NULL;
static PDH_HCOUNTER g_pdhCpuPerfCtr  = NULL;
static PDH_HCOUNTER g_pdhDiskByteCtr = NULL;
static PDH_HCOUNTER g_pdhGpuMemCtr   = NULL;
static ULONGLONG    g_gpuTotalVram   = 0;

// -----------------------------------------------------------------------
// Device name helpers
// -----------------------------------------------------------------------
static void GetCpuName(wchar_t* buf, int len)
{
    wcscpy_s(buf, len, L"CPU");
    HKEY hk;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
            0, KEY_READ, &hk) == ERROR_SUCCESS)
    {
        DWORD sz = (DWORD)(len * sizeof(wchar_t));
        RegQueryValueExW(hk, L"ProcessorNameString", nullptr, nullptr,
                         (LPBYTE)buf, &sz);
        RegCloseKey(hk);
        for (int i = (int)wcslen(buf) - 1; i >= 0 && buf[i] == L' '; --i)
            buf[i] = L'\0';
    }
}

static void GetGpuName(wchar_t* buf, int len)
{
    wcscpy_s(buf, len, L"GPU");
    IDXGIFactory* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&factory)))
    {
        IDXGIAdapter* adapter = nullptr;
        if (SUCCEEDED(factory->EnumAdapters(0, &adapter)))
        {
            DXGI_ADAPTER_DESC desc;
            if (SUCCEEDED(adapter->GetDesc(&desc)))
                wcscpy_s(buf, len, desc.Description);
            adapter->Release();
        }
        factory->Release();
    }
}

static void GetRamName(wchar_t* buf, int len)
{
    MEMORYSTATUSEX ms = { sizeof(ms) };
    GlobalMemoryStatusEx(&ms);
    ULONGLONG gb = (ms.ullTotalPhys + ((1ULL << 30) - 1)) >> 30;
    swprintf_s(buf, len, L"RAM (%llu GB)", gb);
}

static void GetDiskName(wchar_t* buf, int len)
{
    wcscpy_s(buf, len, L"Disk");
    HANDLE h = CreateFileW(L"\\\\.\\PhysicalDrive0",
        0, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE)
    {
        char raw[512] = {};
        STORAGE_PROPERTY_QUERY q = { StorageDeviceProperty, PropertyStandardQuery };
        DWORD ret = 0;
        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY,
                &q, sizeof(q), raw, sizeof(raw), &ret, nullptr))
        {
            auto* d = (STORAGE_DEVICE_DESCRIPTOR*)raw;
            if (d->ProductIdOffset && d->ProductIdOffset < (DWORD)sizeof(raw))
            {
                const char* prod = raw + d->ProductIdOffset;
                int n = MultiByteToWideChar(CP_ACP, 0, prod, -1, buf, len);
                if (n > 1)
                    for (int i = n - 2; i >= 0 && buf[i] == L' '; --i)
                        buf[i] = L'\0';
            }
        }
        CloseHandle(h);
    }
}

// -----------------------------------------------------------------------
// Sample all four metrics into charts
// -----------------------------------------------------------------------
static void SampleMetrics()
{
    // CPU – GetSystemTimes delta
    {
        FILETIME fi, fk, fu;
        GetSystemTimes(&fi, &fk, &fu);
        auto f2u = [](FILETIME f) -> ULONGLONG {
            return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime;
        };
        ULONGLONG idle  = f2u(fi);
        ULONGLONG total = f2u(fk) + f2u(fu);
        ULONGLONG di    = idle  - g_cpuPrevIdle;
        ULONGLONG dt    = total - g_cpuPrevTotal;
        g_cpuPrevIdle  = idle;
        g_cpuPrevTotal = total;
        double v = (dt > 0) ? (1.0 - (double)di / (double)dt) : 0.0;
        PushChartValue(g_charts[0], max(0.0, min(v, 1.0)));

        if (g_pdhCpuFreqCtr && g_pdhCpuPerfCtr)
        {
            PDH_FMT_COUNTERVALUE freq, perf;
            if (PdhGetFormattedCounterValue(g_pdhCpuFreqCtr, PDH_FMT_DOUBLE,
                    nullptr, &freq) == ERROR_SUCCESS &&
                freq.CStatus == PDH_CSTATUS_VALID_DATA &&
                PdhGetFormattedCounterValue(g_pdhCpuPerfCtr, PDH_FMT_DOUBLE,
                    nullptr, &perf) == ERROR_SUCCESS &&
                perf.CStatus == PDH_CSTATUS_VALID_DATA)
            {
                double actualMHz = freq.doubleValue * perf.doubleValue / 100.0;
                swprintf_s(g_charts[0].absStr, L"%.2f GHz", actualMHz / 1000.0);
            }
        }
    }

    // GPU – PDH wildcard over all 3D engine instances
    {
        double v = 0.0;
        if (g_pdhGpuCtr)
        {
            DWORD sz = 0, cnt = 0;
            PdhGetFormattedCounterArrayW(g_pdhGpuCtr, PDH_FMT_DOUBLE,
                                         &sz, &cnt, nullptr);
            if (sz > 0)
            {
                auto* items = (PDH_FMT_COUNTERVALUE_ITEM_W*)malloc(sz);
                if (items)
                {
                    if (PdhGetFormattedCounterArrayW(g_pdhGpuCtr, PDH_FMT_DOUBLE,
                            &sz, &cnt, items) == ERROR_SUCCESS)
                    {
                        for (DWORD i = 0; i < cnt; ++i)
                            if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA)
                                v += items[i].FmtValue.doubleValue;
                    }
                    free(items);
                }
            }
        }
        PushChartValue(g_charts[1], min(v / 100.0, 1.0));

        if (g_pdhGpuMemCtr && g_gpuTotalVram > 0)
        {
            DWORD sz2 = 0, cnt2 = 0;
            PdhGetFormattedCounterArrayW(g_pdhGpuMemCtr, PDH_FMT_LARGE,
                                         &sz2, &cnt2, nullptr);
            if (sz2 > 0)
            {
                auto* items2 = (PDH_FMT_COUNTERVALUE_ITEM_W*)malloc(sz2);
                if (items2)
                {
                    ULONGLONG totalUsed = 0;
                    if (PdhGetFormattedCounterArrayW(g_pdhGpuMemCtr, PDH_FMT_LARGE,
                            &sz2, &cnt2, items2) == ERROR_SUCCESS)
                    {
                        for (DWORD i = 0; i < cnt2; ++i)
                            if (items2[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA)
                                totalUsed += (ULONGLONG)items2[i].FmtValue.largeValue;
                    }
                    free(items2);
                    double usedGB  = (double)totalUsed      / (1024.0 * 1024.0 * 1024.0);
                    double totalGB = (double)g_gpuTotalVram / (1024.0 * 1024.0 * 1024.0);
                    swprintf_s(g_charts[1].absStr, L"%.1f / %.0f GB", usedGB, totalGB);
                }
            }
        }
    }

    // RAM – GlobalMemoryStatusEx
    {
        MEMORYSTATUSEX ms = { sizeof(ms) };
        GlobalMemoryStatusEx(&ms);
        PushChartValue(g_charts[2], ms.dwMemoryLoad / 100.0);

        {
            double usedGB  = (double)(ms.ullTotalPhys - ms.ullAvailPhys) / (1024.0 * 1024.0 * 1024.0);
            double totalGB = (double)ms.ullTotalPhys / (1024.0 * 1024.0 * 1024.0);
            swprintf_s(g_charts[2].absStr, L"%.1f / %.0f GB", usedGB, totalGB);
        }
    }

    // Disk – PDH % Disk Time
    {
        double v = 0.0;
        if (g_pdhDiskCtr)
        {
            PDH_FMT_COUNTERVALUE val;
            if (PdhGetFormattedCounterValue(g_pdhDiskCtr, PDH_FMT_DOUBLE,
                    nullptr, &val) == ERROR_SUCCESS &&
                val.CStatus == PDH_CSTATUS_VALID_DATA)
                v = val.doubleValue / 100.0;
        }
        PushChartValue(g_charts[3], max(0.0, min(v, 1.0)));

        if (g_pdhDiskByteCtr)
        {
            PDH_FMT_COUNTERVALUE val;
            if (PdhGetFormattedCounterValue(g_pdhDiskByteCtr, PDH_FMT_DOUBLE,
                    nullptr, &val) == ERROR_SUCCESS &&
                val.CStatus == PDH_CSTATUS_VALID_DATA)
                swprintf_s(g_charts[3].absStr, L"%.1f MB/s", val.doubleValue / (1024.0 * 1024.0));
        }
    }
}

// -----------------------------------------------------------------------
// Layered window update
// -----------------------------------------------------------------------
static void UpdateLayeredContent(HWND hwnd)
{
    RECT rcWin;
    GetWindowRect(hwnd, &rcWin);
    const int w = rcWin.right  - rcWin.left;
    const int h = rcWin.bottom - rcWin.top;
    if (w <= 0 || h <= 0 || !g_pDCRT) return;

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem    = CreateCompatibleDC(hdcScreen);

    BITMAPINFO bmi              = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = w;
    bmi.bmiHeader.biHeight      = -h;
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void*   pvBits = nullptr;
    HBITMAP hBmp   = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, &pvBits, NULL, 0);
    if (!hBmp) { DeleteDC(hdcMem); ReleaseDC(NULL, hdcScreen); return; }
    HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, hBmp);

    // --- D2D drawing --------------------------------------------------
    RECT rcD2D = { 0, 0, w, h };
    if (SUCCEEDED(g_pDCRT->BindDC(hdcMem, &rcD2D)))
    {
        g_pDCRT->BeginDraw();
        g_pDCRT->Clear(D2D1::ColorF(0.f, 0.f, 0.f, 1.f / 255.f));

        // vertical stack: CPU / GPU / RAM / Disk
        const float pad    = 6.f;
        const float vEdge  = 14.f; // matches DrawChart's inner margin (8) + pad (6)
        const float colW   = (float)w - 2.f * pad;
        const float rowH   = ((float)h - 2.f * vEdge - (NUM_CHARTS - 1) * pad) / NUM_CHARTS;

        for (int i = 0; i < NUM_CHARTS; ++i)
        {
            float x = pad;
            float y = vEdge + i * (rowH + pad);
            D2D1_RECT_F area = { x, y, x + colW, y + rowH };
            DrawChart(g_pDCRT, g_charts[i], area, g_pChartFmtL, g_pChartFmtR);
        }

        g_pDCRT->EndDraw();
    }
    // ------------------------------------------------------------------

    POINT         ptSrc  = { 0, 0 };
    POINT         ptDst  = { rcWin.left, rcWin.top };
    SIZE          size   = { w, h };
    BLENDFUNCTION blend  = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(hwnd, hdcScreen, &ptDst, &size, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);

    SelectObject(hdcMem, hOld);
    DeleteObject(hBmp);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
}

// -----------------------------------------------------------------------
// Window procedure
// -----------------------------------------------------------------------
static constexpr int RESIZE_BORDER = 6;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    // ------------------------------------------------------------------
    case WM_CREATE:
    {
        ApplyBlur(hwnd);

        DWM_WINDOW_CORNER_PREFERENCE pref = DWMWCP_ROUND;
        DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
                              &pref, sizeof(pref));

        NONCLIENTMETRICSW ncm = { sizeof(ncm) };
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        HDC hdc    = GetDC(hwnd);
        float dpi  = static_cast<float>(GetDeviceCaps(hdc, LOGPIXELSY));
        g_fontSize = static_cast<float>(abs(ncm.lfMessageFont.lfHeight)) * 96.f / dpi;
        ReleaseDC(hwnd, hdc);

        // Chart label format – left aligned
        g_pDWFactory->CreateTextFormat(
            ncm.lfMessageFont.lfFaceName, nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, g_fontSize, L"", &g_pChartFmtL);
        if (g_pChartFmtL)
        {
            g_pChartFmtL->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            g_pChartFmtL->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }

        // Chart value format – right aligned
        g_pDWFactory->CreateTextFormat(
            ncm.lfMessageFont.lfFaceName, nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, g_fontSize, L"", &g_pChartFmtR);
        if (g_pChartFmtR)
        {
            g_pChartFmtR->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            g_pChartFmtR->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }

        // DXGI adapter for GPU total VRAM
        {
            IDXGIFactory* factory = nullptr;
            if (SUCCEEDED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&factory)))
            {
                IDXGIAdapter* adapter = nullptr;
                if (SUCCEEDED(factory->EnumAdapters(0, &adapter)))
                {
                    DXGI_ADAPTER_DESC desc = {};
                    if (SUCCEEDED(adapter->GetDesc(&desc)))
                        g_gpuTotalVram = desc.DedicatedVideoMemory;
                    adapter->Release();
                }
                factory->Release();
            }
        }

        // Device names -> chart titles
        GetCpuName (g_charts[0].name, 256);
        GetGpuName (g_charts[1].name, 256);
        GetRamName (g_charts[2].name, 256);
        GetDiskName(g_charts[3].name, 256);

        // PDH query: GPU engine utilization + disk I/O
        if (PdhOpenQuery(NULL, 0, &g_pdhQuery) == ERROR_SUCCESS)
        {
            PdhAddEnglishCounterW(g_pdhQuery,
                L"\\GPU Engine(*engtype_3D)\\Utilization Percentage",
                0, &g_pdhGpuCtr);
            PdhAddEnglishCounterW(g_pdhQuery,
                L"\\PhysicalDisk(_Total)\\% Disk Time",
                0, &g_pdhDiskCtr);
            PdhAddEnglishCounterW(g_pdhQuery,
                L"\\Processor Information(_Total)\\Processor Frequency",
                0, &g_pdhCpuFreqCtr);
            PdhAddEnglishCounterW(g_pdhQuery,
                L"\\Processor Information(_Total)\\% Processor Performance",
                0, &g_pdhCpuPerfCtr);
            PdhAddEnglishCounterW(g_pdhQuery,
                L"\\PhysicalDisk(_Total)\\Disk Bytes/sec",
                0, &g_pdhDiskByteCtr);
            PdhAddEnglishCounterW(g_pdhQuery,
                L"\\GPU Adapter Memory(*)\\Dedicated Usage",
                0, &g_pdhGpuMemCtr);
            PdhCollectQueryData(g_pdhQuery); // baseline
        }

        // CPU baseline (seed prev values so first delta is valid)
        {
            FILETIME fi, fk, fu;
            GetSystemTimes(&fi, &fk, &fu);
            auto f2u = [](FILETIME f) -> ULONGLONG {
                return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime;
            };
            g_cpuPrevIdle  = f2u(fi);
            g_cpuPrevTotal = f2u(fk) + f2u(fu);
        }

        // Initial flush so labels show something on first paint
        SampleMetrics();
        FlushDisplayValues();

        SetTimer(hwnd, 1, 50, nullptr); // ~20 fps

        UpdateLayeredContent(hwnd);
        return 0;
    }

    // ------------------------------------------------------------------
    case WM_TIMER:
    {
        if (g_pdhQuery) PdhCollectQueryData(g_pdhQuery);
        SampleMetrics();
        if (++g_labelTick >= 20) // 20 * 50ms = 1 second
        {
            g_labelTick = 0;
            FlushDisplayValues();
        }
        UpdateLayeredContent(hwnd);
        return 0;
    }

    // ------------------------------------------------------------------
    case WM_NCHITTEST:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        RECT  rc;
        GetWindowRect(hwnd, &rc);

        const bool L = pt.x <  rc.left   + RESIZE_BORDER;
        const bool R = pt.x >= rc.right  - RESIZE_BORDER;
        const bool T = pt.y <  rc.top    + RESIZE_BORDER;
        const bool B = pt.y >= rc.bottom - RESIZE_BORDER;

        if (T && L) return HTTOPLEFT;
        if (T && R) return HTTOPRIGHT;
        if (B && L) return HTBOTTOMLEFT;
        if (B && R) return HTBOTTOMRIGHT;
        if (T)      return HTTOP;
        if (B)      return HTBOTTOM;
        if (L)      return HTLEFT;
        if (R)      return HTRIGHT;

        return HTCAPTION;
    }

    // ------------------------------------------------------------------
    case WM_WINDOWPOSCHANGED:
    {
        const auto* wp = reinterpret_cast<const WINDOWPOS*>(lParam);
        if (!(wp->flags & SWP_NOMOVE) || !(wp->flags & SWP_NOSIZE))
            UpdateLayeredContent(hwnd);
        return 0;
    }

    // ------------------------------------------------------------------
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }

    // ------------------------------------------------------------------
    case WM_DWMCOMPOSITIONCHANGED:
        ApplyBlur(hwnd);
        UpdateLayeredContent(hwnd);
        return 0;

    // ------------------------------------------------------------------
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) DestroyWindow(hwnd);
        return 0;

    case WM_NCRBUTTONUP:
    case WM_RBUTTONUP:
        DestroyWindow(hwnd);
        return 0;

    // ------------------------------------------------------------------
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        if (g_pdhQuery)      { PdhCloseQuery(g_pdhQuery); g_pdhQuery = NULL; }
        if (g_pChartFmtR) { g_pChartFmtR->Release(); g_pChartFmtR = nullptr; }
        if (g_pChartFmtL) { g_pChartFmtL->Release(); g_pChartFmtL = nullptr; }
        if (g_pDCRT)      { g_pDCRT->Release();       g_pDCRT      = nullptr; }
        if (g_pDWFactory) { g_pDWFactory->Release();  g_pDWFactory = nullptr; }
        if (g_pD2DFactory){ g_pD2DFactory->Release(); g_pD2DFactory = nullptr; }
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// -----------------------------------------------------------------------
// Entry point
// -----------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_pD2DFactory);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                        __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(&g_pDWFactory));

    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    g_pD2DFactory->CreateDCRenderTarget(&rtProps, &g_pDCRT);

    const wchar_t CLASS_NAME[] = L"BlurBoxWnd";

    WNDCLASSEXW wc   = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = CLASS_NAME;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED,
        CLASS_NAME,
        L"BlurBox",
        WS_POPUP | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 400, 560,
        NULL, NULL, hInstance, NULL);

    if (!hwnd) return 1;

    ShowWindow(hwnd, nCmdShow);

    MSG msg = {};
    while (GetMessageW(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return static_cast<int>(msg.wParam);
}
