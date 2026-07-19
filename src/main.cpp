#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wbemidl.h>
#include <windowsx.h>
#include <winioctl.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dxgi.h>
#include <dxgi1_4.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <wininet.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "wbemuuid.lib")

#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")

#define WM_TRAYICON   (WM_APP + 1)
#define WM_FETCH_IP   (WM_APP + 2)
#define WM_REDRAW_IP  (WM_APP + 3)
#define ID_TRAY_EXIT      1001
#define ID_TRAY_AUTOSTART 1002
static NOTIFYICONDATAW g_nid = {};

// -----------------------------------------------------------------------
// NVML (NVIDIA Management Library) – dynamic loading, no import lib needed
// Works with RTX 5080 / Blackwell and all modern NVIDIA GPUs.
// -----------------------------------------------------------------------
typedef int   nvmlReturn_t;
typedef void* nvmlDevice_t;
#define NVML_SUCCESS          0
#define NVML_TEMPERATURE_GPU  0

typedef nvmlReturn_t (*PFN_nvmlInit)(void);
typedef nvmlReturn_t (*PFN_nvmlShutdown)(void);
typedef nvmlReturn_t (*PFN_nvmlDeviceGetHandleByIndex)(unsigned int, nvmlDevice_t*);
typedef nvmlReturn_t (*PFN_nvmlDeviceGetTemperature)(nvmlDevice_t, int, unsigned int*);


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
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    auto pfn = hUser32 ? reinterpret_cast<PFN_SetWindowCompositionAttribute>(
        GetProcAddress(hUser32, "SetWindowCompositionAttribute")) : nullptr;
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
// Layout constants
// -----------------------------------------------------------------------
static float g_fontScale    = 1.5f;        // configurable via config.json "font_scale"
static bool  g_debugEnabled = false;      // configurable via config.json "debug"
static bool  g_showVertDividers = true;   // configurable via config.json "show_vert_dividers"
static float PAD         = 6.f  * g_fontScale;
static float VEDGE       = 14.f * g_fontScale;

// -----------------------------------------------------------------------
// D2D / DirectWrite state
// -----------------------------------------------------------------------
static ID2D1Factory*        g_pD2DFactory = nullptr;
static IDWriteFactory*      g_pDWFactory  = nullptr;
static IDWriteTextFormat*   g_pChartFmtL  = nullptr;   // left/top  – chart name
static IDWriteTextFormat*   g_pChartFmtR  = nullptr;   // right/top – chart value
static ID2D1DCRenderTarget* g_pDCRT       = nullptr;
static IDWriteTextFormat*   g_pMonoFmt    = nullptr;   // monospace – ASCII art
static float                g_fontSize    = 14.f * g_fontScale;

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
    wchar_t absStr[64];
    double  displayCurrent;
    wchar_t displayAbsStr[64];
};

static const int NUM_CHARTS = 4;
static Chart g_charts[NUM_CHARTS]; // 0=CPU, 1=GPU, 2=RAM, 3=Disk

// -----------------------------------------------------------------------
// Per-core CPU state
// -----------------------------------------------------------------------
static const int MAX_CORES = 128;
static int    g_cpuMode           = 0; // 0=total, 1=logical cores, 2=physical cores, 3=load+temp
static int    g_logicalCoreCount  = 0;
static int    g_physicalCoreCount = 0;
static int    g_logicalToPhysical[MAX_CORES] = {}; // logical idx → physical idx
static Chart  g_logicalCharts[MAX_CORES]     = {};
static Chart  g_physicalCharts[MAX_CORES]    = {};
static PDH_HCOUNTER g_pdhCoreCtr = NULL;
static Chart        g_cpuTempChart  = {};
static PDH_HCOUNTER g_pdhCpuTempCtr = NULL;
static volatile LONG g_cpuWmiTempDeciC  = 0;   // tenths of Celsius from WMI thread (0 = N/A)
static HANDLE        g_cpuTempThread    = NULL;
static volatile bool g_cpuTempThreadStop = false;
static D2D1_RECT_F  g_cpuChartRect = {};  // CPU row rect (local coords), for click detection

// -----------------------------------------------------------------------
// GPU mode
// -----------------------------------------------------------------------
static int          g_gpuMode       = 0; // 0=core load, 1=VRAM %, 2=temp, 3=core+VRAM, 4=core+VRAM+temp
static Chart        g_gpuVramChart  = {};
static Chart        g_gpuTempChart  = {};       // regular (edge) GPU temperature
static Chart        g_gpuHotspotChart = {};      // GPU hot-spot temperature (HWiNFO only)
static PDH_HCOUNTER g_pdhGpuTempCtr = NULL;
static D2D1_RECT_F  g_gpuChartRect  = {};

// -----------------------------------------------------------------------
// RAM mode
// -----------------------------------------------------------------------
static int         g_ramMode      = 0; // 0=load, 1=load+temp
static Chart       g_ramTempChart = {};
static D2D1_RECT_F g_ramChartRect = {};

// NVML state (prefer over PDH for GPU temperature)
static HMODULE                        g_hNvml         = NULL;
static nvmlDevice_t                   g_nvmlDevice    = NULL;
static PFN_nvmlDeviceGetTemperature   g_nvmlGetTemp   = NULL;
static PFN_nvmlShutdown               g_nvmlShutdown  = NULL;

// -----------------------------------------------------------------------
// Per-disk state
// -----------------------------------------------------------------------
static const int    MAX_DISKS = 16;
static int          g_diskMode    = 0; // 0=_Total, 1..N = disk index (1-based)
static int          g_diskSubMode = 0; // 0=single, 1=read+write, 2=read+write+temp
static int          g_diskCount = 0;
static Chart        g_diskCharts[MAX_DISKS]      = {};
static Chart        g_diskReadCharts[MAX_DISKS]  = {};
static Chart        g_diskWriteCharts[MAX_DISKS] = {};
static Chart        g_diskTempCharts[MAX_DISKS]  = {};
static PDH_HCOUNTER g_pdhDiskCtrs[MAX_DISKS]     = {};
static PDH_HCOUNTER g_pdhDiskByteCtrs[MAX_DISKS] = {};
static PDH_HCOUNTER g_pdhDiskReadCtrs[MAX_DISKS]  = {};
static PDH_HCOUNTER g_pdhDiskWriteCtrs[MAX_DISKS] = {};
static double       g_diskIoPeak[MAX_DISKS]        = {};
static Chart        g_diskTotalReadChart  = {};
static Chart        g_diskTotalWriteChart = {};
static double       g_diskTotalIoPeak    = 0.0;
static D2D1_RECT_F  g_diskChartRect  = {};
static D2D1_RECT_F  g_weatherRect    = {};

// -----------------------------------------------------------------------
// Column layout configuration
// Panel type names: "weather", "ip", "rss",
//   "cpu_chart", "gpu_chart", "ram_chart", "disk_chart",
//   "cpu_proc",  "gpu_proc",  "ram_proc",  "disk_proc", "none"
// -----------------------------------------------------------------------
static const int MAX_COLS = 8;
static const int MAX_PANELS_PER_COL = 8;

static int  g_colCount = 3;
static char g_colPanels[MAX_COLS][MAX_PANELS_PER_COL][32];
static int  g_colPanelCount[MAX_COLS];

static float g_colDivX[MAX_COLS];                          // X of divider after column k
static bool  g_colDivXSet[MAX_COLS];                       // loaded from config?
static float g_colDivY[MAX_COLS][MAX_PANELS_PER_COL];      // Y between row r and r+1 in col c
static bool  g_colDivYSet[MAX_COLS][MAX_PANELS_PER_COL];
static bool  g_colShowYDiv[MAX_COLS];                      // per-column horizontal divider visibility, configurable via config.json "col_N_show_ydiv"

static int g_draggingColDivIdx = -1;   // which vertical divider is dragged (-1 = none)
static int g_draggingRowDivCol = -1;   // column of dragged horizontal divider (-1 = none)
static int g_draggingRowDivIdx = -1;   // row index of dragged horizontal divider

// RSS panel rects collected each frame for link hit-testing
static D2D1_RECT_F g_rssPanelRects[MAX_COLS * MAX_PANELS_PER_COL];
static int         g_rssPanelRectCount = 0;

// -----------------------------------------------------------------------
// Per-process top lists
// -----------------------------------------------------------------------
struct ProcEntry {
    wchar_t name[64];
    float   pct;        // 0..100
    wchar_t absStr[32]; // formatted absolute value
};

static const int MAX_PROC_SHOW = 32;

static ProcEntry g_procCpu[MAX_PROC_SHOW];
static ProcEntry g_procGpu[MAX_PROC_SHOW];
static ProcEntry g_procRam[MAX_PROC_SHOW];
static ProcEntry g_procDisk[MAX_PROC_SHOW];
static int       g_procCpuCount  = 0;
static int       g_procGpuCount  = 0;
static int       g_procRamCount  = 0;
static int       g_procDiskCount = 0;

// Display copies (flushed every FlushDisplayValues tick)
static ProcEntry g_dispProcCpu[MAX_PROC_SHOW];
static ProcEntry g_dispProcGpu[MAX_PROC_SHOW];
static ProcEntry g_dispProcRam[MAX_PROC_SHOW];
static ProcEntry g_dispProcDisk[MAX_PROC_SHOW];
static int       g_dispProcCpuCount  = 0;
static int       g_dispProcGpuCount  = 0;
static int       g_dispProcRamCount  = 0;
static int       g_dispProcDiskCount = 0;

// Toggle: false = percent, true = absolute
static bool g_procAbsMode[NUM_CHARTS] = {};

// Click rects for process lists (local window coordinates)
static D2D1_RECT_F g_procListRects[NUM_CHARTS] = {};

// Hover state for process list tiles (main-thread only)
static int g_hoveredProcList = -1; // 0..3 = tile index; -1 = none
static int g_hoveredProcRow  = -1; // entry row index within tile; -1 = none/title

// PDH process counters (added to g_pdhQuery in WM_CREATE)
static PDH_HCOUNTER g_pdhProcCpuCtr  = NULL;
static PDH_HCOUNTER g_pdhProcRamCtr  = NULL;
static PDH_HCOUNTER g_pdhProcDiskCtr = NULL;

static void PushChartValue(Chart& c, double v)
{
    c.values[c.head] = v;
    c.head  = (c.head + 1) % CHART_SAMPLES;
    if (c.count < CHART_SAMPLES) c.count++;
    c.current = v;
}

// Returns chart data-type index for a panel type string (0=cpu..3=disk; -1=not a chart).
static int ChartPanelIdx(const char* t)
{
    if (!t) return -1;
    if (strcmp(t, "cpu_chart") == 0) return 0;
    if (strcmp(t, "gpu_chart") == 0) return 1;
    if (strcmp(t, "ram_chart") == 0) return 2;
    if (strcmp(t, "disk_chart") == 0) return 3;
    return -1;
}

// Returns process-list data-type index (0=cpu..3=disk; -1=not a proc panel).
static int ProcPanelIdx(const char* t)
{
    if (!t) return -1;
    if (strcmp(t, "cpu_proc")  == 0) return 0;
    if (strcmp(t, "gpu_proc")  == 0) return 1;
    if (strcmp(t, "ram_proc")  == 0) return 2;
    if (strcmp(t, "disk_proc") == 0) return 3;
    return -1;
}

// Parse pipe-separated panel list "a|b|c" into out array; returns count.
static int ParsePanelList(const char* str, char out[][32], int maxCount)
{
    int count = 0;
    const char* p = str;
    while (*p && count < maxCount)
    {
        const char* end = strchr(p, '|');
        int len = end ? (int)(end - p) : (int)strlen(p);
        if (len > 0 && len < 32)
        {
            memcpy(out[count], p, len);
            out[count][len] = '\0';
            count++;
        }
        if (!end) break;
        p = end + 1;
    }
    return count;
}

// Set up the default 3-column layout (weather/ip/rss | charts | proc lists).
static void SetDefaultLayout()
{
    g_colCount = 3;
    g_colPanelCount[0] = 3;
    strcpy_s(g_colPanels[0][0], "weather");
    strcpy_s(g_colPanels[0][1], "ip");
    strcpy_s(g_colPanels[0][2], "rss");
    g_colPanelCount[1] = 4;
    strcpy_s(g_colPanels[1][0], "cpu_chart");
    strcpy_s(g_colPanels[1][1], "gpu_chart");
    strcpy_s(g_colPanels[1][2], "ram_chart");
    strcpy_s(g_colPanels[1][3], "disk_chart");
    g_colPanelCount[2] = 4;
    strcpy_s(g_colPanels[2][0], "cpu_proc");
    strcpy_s(g_colPanels[2][1], "gpu_proc");
    strcpy_s(g_colPanels[2][2], "ram_proc");
    strcpy_s(g_colPanels[2][3], "disk_proc");

    for (int c = 0; c < MAX_COLS; c++) g_colShowYDiv[c] = true;
}

// Initialize dividers to auto-spaced defaults (for any not loaded from config).
static void InitDividers(int w, int h)
{
    for (int k = 0; k < g_colCount - 1; k++)
    {
        if (!g_colDivXSet[k])
            g_colDivX[k] = (float)w * (float)(k + 1) / (float)g_colCount;
        // Clamp: ensure min 80px margin between dividers and edges
        float minX = 80.f + k * 80.f;
        float maxX = (float)w - 80.f - (g_colCount - 2 - k) * 80.f;
        g_colDivX[k] = max(minX, min(g_colDivX[k], maxX));
        // Enforce ordering
        if (k > 0) g_colDivX[k] = max(g_colDivX[k - 1] + 80.f, g_colDivX[k]);
    }
    for (int c = 0; c < g_colCount; c++)
    {
        int n = g_colPanelCount[c];
        for (int r = 0; r < n - 1; r++)
        {
            if (!g_colDivYSet[c][r])
                g_colDivY[c][r] = (float)(int)VEDGE + ((float)h - 2.f * (float)(int)VEDGE) * (float)(r + 1) / (float)n;
            float minY = (float)(int)VEDGE + 30.f + r * 30.f;
            float maxY = (float)h - (float)(int)VEDGE - 30.f - (n - 2 - r) * 30.f;
            g_colDivY[c][r] = max(minY, min(g_colDivY[c][r], maxY));
            if (r > 0) g_colDivY[c][r] = max(g_colDivY[c][r - 1] + 30.f, g_colDivY[c][r]);
        }
    }
}

static void SampleProcesses(); // forward declaration

static void FlushDisplayValues()
{
    auto flush = [](Chart& c) {
        c.displayCurrent = c.current;
        wcscpy_s(c.displayAbsStr, c.absStr);
    };
    for (int i = 0; i < NUM_CHARTS; ++i)        flush(g_charts[i]);
    for (int i = 0; i < g_logicalCoreCount; ++i) flush(g_logicalCharts[i]);
    for (int i = 0; i < g_physicalCoreCount; ++i) flush(g_physicalCharts[i]);
    flush(g_gpuVramChart);
    flush(g_gpuTempChart);
    flush(g_gpuHotspotChart);
    flush(g_cpuTempChart);
    flush(g_ramTempChart);
    flush(g_diskTotalReadChart);
    flush(g_diskTotalWriteChart);
    for (int i = 0; i < g_diskCount; ++i)
    {
        flush(g_diskCharts[i]);
        flush(g_diskReadCharts[i]);
        flush(g_diskWriteCharts[i]);
        flush(g_diskTempCharts[i]);
    }

    // Sample and flush process lists at the same rate as chart labels.
    // Skip flushing whichever tile the mouse is currently hovering over (freeze on hover).
    SampleProcesses();
    if (g_hoveredProcList != 0) {
        g_dispProcCpuCount = g_procCpuCount;
        if (g_procCpuCount > 0) memcpy(g_dispProcCpu, g_procCpu, g_procCpuCount * sizeof(ProcEntry));
    }
    if (g_hoveredProcList != 1) {
        g_dispProcGpuCount = g_procGpuCount;
        if (g_procGpuCount > 0) memcpy(g_dispProcGpu, g_procGpu, g_procGpuCount * sizeof(ProcEntry));
    }
    if (g_hoveredProcList != 2) {
        g_dispProcRamCount = g_procRamCount;
        if (g_procRamCount > 0) memcpy(g_dispProcRam, g_procRam, g_procRamCount * sizeof(ProcEntry));
    }
    if (g_hoveredProcList != 3) {
        g_dispProcDiskCount = g_procDiskCount;
        if (g_procDiskCount > 0) memcpy(g_dispProcDisk, g_procDisk, g_procDiskCount * sizeof(ProcEntry));
    }
}

// Draw text with character-level ellipsis trimming when it overflows rect width.
static void DrawTextEllipsis(ID2D1RenderTarget* rt,
                              const wchar_t* text, UINT32 len,
                              IDWriteTextFormat* fmt,
                              D2D1_RECT_F rect,
                              ID2D1Brush* brush)
{
    if (!g_pDWFactory || !fmt || len == 0) return;
    float w = rect.right - rect.left;
    float h = rect.bottom - rect.top;
    if (w <= 0.f || h <= 0.f) return;

    IDWriteTextLayout* layout = nullptr;
    if (FAILED(g_pDWFactory->CreateTextLayout(text, len, fmt, w, h, &layout)))
    {
        rt->DrawText(text, len, fmt, rect, brush);
        return;
    }
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    IDWriteInlineObject* ellipsis = nullptr;
    if (SUCCEEDED(g_pDWFactory->CreateEllipsisTrimmingSign(fmt, &ellipsis)))
    {
        DWRITE_TRIMMING trim = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        layout->SetTrimming(&trim, ellipsis);
        ellipsis->Release();
    }
    rt->DrawTextLayout({ rect.left, rect.top }, layout, brush);
    layout->Release();
}

static void DrawChart(ID2D1RenderTarget* rt, const Chart& c,
                      D2D1_RECT_F area,
                      IDWriteTextFormat* fmtL, IDWriteTextFormat* fmtR,
                      int maxSamples = CHART_SAMPLES,
                      bool absOnly = false)
{
    const float margin = 8.f;
    const float labelH = g_fontSize * 1.4f;

    D2D1_RECT_F labelArea = { area.left + margin, area.top, area.right - margin, area.top + labelH };
    D2D1_RECT_F plotArea  = { area.left + margin, area.top + labelH, area.right - margin, area.bottom };

    ID2D1SolidColorBrush* pBrush = nullptr;
    if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0.f, 0.f, 0.f, 1.f), &pBrush)))
        return;

    // Build value string first so we can measure its width before drawing the name.
    wchar_t valBuf[128] = {};
    if (fmtR)
    {
        if (absOnly && c.displayAbsStr[0])
            wcscpy_s(valBuf, c.displayAbsStr);
        else if (!absOnly && c.displayAbsStr[0])
            swprintf_s(valBuf, L"%.1f%% / %s", c.displayCurrent * 100.0, c.displayAbsStr);
        else if (!absOnly)
            swprintf_s(valBuf, L"%.1f%%", c.displayCurrent * 100.0);
    }

    // Measure value text width so the name won't overlap it.
    float valWidth = 0.f;
    if (fmtR && valBuf[0] && g_pDWFactory)
    {
        IDWriteTextLayout* vl = nullptr;
        float areaW = labelArea.right - labelArea.left;
        if (SUCCEEDED(g_pDWFactory->CreateTextLayout(
                valBuf, (UINT32)wcslen(valBuf), fmtR, areaW, labelH, &vl)))
        {
            DWRITE_TEXT_METRICS tm = {};
            vl->GetMetrics(&tm);
            valWidth = tm.widthIncludingTrailingWhitespace;
            if (valWidth > areaW) valWidth = areaW;
            vl->Release();
        }
    }

    // Draw name truncated to the space not occupied by the value (4 px gap).
    if (fmtL)
    {
        const float gap = 4.f;
        float nameMaxW = (labelArea.right - labelArea.left) - valWidth - gap;
        if (nameMaxW < 0.f) nameMaxW = 0.f;
        D2D1_RECT_F nameRect = { labelArea.left, labelArea.top,
                                  labelArea.left + nameMaxW, labelArea.bottom };
        DrawTextEllipsis(rt, c.name, (UINT32)wcslen(c.name), fmtL, nameRect, pBrush);
    }

    // Draw value (right-aligned).
    if (fmtR && valBuf[0])
        rt->DrawText(valBuf, (UINT32)wcslen(valBuf), fmtR, labelArea, pBrush);

    rt->DrawRectangle(plotArea, pBrush, 0.75f);

    const int n = min(c.count, maxSamples);
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

// Draw the GPU temperature panel: hot-spot temperature in black followed by the regular
// (edge) temperature in gray parentheses, both on the label and on the plotted lines.
// Falls back to the plain single-series DrawChart when no hot-spot reading is available
// (e.g. HWiNFO64 isn't running).
static void DrawGpuTempChart(ID2D1RenderTarget* rt, const Chart& hot, const Chart& base,
                              D2D1_RECT_F area,
                              IDWriteTextFormat* fmtL, IDWriteTextFormat* fmtR,
                              int maxSamples = CHART_SAMPLES,
                              bool absOnly = false)
{
    bool haveHot = hot.displayAbsStr[0] && wcscmp(hot.displayAbsStr, L"N/A") != 0;
    if (!haveHot)
    {
        DrawChart(rt, base, area, fmtL, fmtR, maxSamples, absOnly);
        return;
    }

    const float margin = 8.f;
    const float labelH = g_fontSize * 1.4f;

    D2D1_RECT_F labelArea = { area.left + margin, area.top, area.right - margin, area.top + labelH };
    D2D1_RECT_F plotArea  = { area.left + margin, area.top + labelH, area.right - margin, area.bottom };

    ID2D1SolidColorBrush* pBlack = nullptr;
    ID2D1SolidColorBrush* pGray  = nullptr;
    if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0.f, 0.f, 0.f, 1.f), &pBlack)))
        return;
    if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0.3f, 0.3f, 0.3f, 1.f), &pGray)))
    {
        pBlack->Release();
        return;
    }

    bool haveBase = base.displayAbsStr[0] && wcscmp(base.displayAbsStr, L"N/A") != 0;

    // Value text: hot-spot temp (black) followed by the regular temp in gray parentheses.
    wchar_t valBuf[128] = {};
    UINT32  hotLen = 0;
    if (fmtR)
    {
        hotLen = (UINT32)wcslen(hot.displayAbsStr);
        if (haveBase)
            swprintf_s(valBuf, L"%s (%s)", hot.displayAbsStr, base.displayAbsStr);
        else
            wcscpy_s(valBuf, hot.displayAbsStr);
    }

    float valWidth = 0.f;
    IDWriteTextLayout* vl = nullptr;
    if (fmtR && valBuf[0] && g_pDWFactory)
    {
        float areaW = labelArea.right - labelArea.left;
        if (SUCCEEDED(g_pDWFactory->CreateTextLayout(
                valBuf, (UINT32)wcslen(valBuf), fmtR, areaW, labelH, &vl)))
        {
            DWRITE_TEXT_METRICS tm = {};
            vl->GetMetrics(&tm);
            valWidth = tm.widthIncludingTrailingWhitespace;
            if (valWidth > areaW) valWidth = areaW;

            UINT32 totalLen = (UINT32)wcslen(valBuf);
            if (haveBase && hotLen < totalLen)
            {
                DWRITE_TEXT_RANGE grayRange = { hotLen, totalLen - hotLen };
                vl->SetDrawingEffect(pGray, grayRange);
            }
        }
    }

    // Draw name truncated to the space not occupied by the value (4 px gap).
    if (fmtL)
    {
        const float gap = 4.f;
        float nameMaxW = (labelArea.right - labelArea.left) - valWidth - gap;
        if (nameMaxW < 0.f) nameMaxW = 0.f;
        D2D1_RECT_F nameRect = { labelArea.left, labelArea.top,
                                  labelArea.left + nameMaxW, labelArea.bottom };
        DrawTextEllipsis(rt, base.name, (UINT32)wcslen(base.name), fmtL, nameRect, pBlack);
    }

    // Draw value (right-aligned via fmtR's trailing alignment baked into the layout box).
    if (vl)
    {
        rt->DrawTextLayout({ labelArea.left, labelArea.top }, vl, pBlack);
        vl->Release();
    }

    rt->DrawRectangle(plotArea, pBlack, 0.75f);

    const int n = min(min(hot.count, base.count), maxSamples);
    if (n >= 2)
    {
        const float left   = plotArea.left   + 2.f;
        const float right  = plotArea.right  - 2.f;
        const float top    = plotArea.top    + 2.f;
        const float bottom = plotArea.bottom - 2.f;
        const float pw     = right  - left;
        const float ph     = bottom - top;

        auto drawSeries = [&](const Chart& c, ID2D1SolidColorBrush* brush) {
            const int start = (c.head - n + CHART_SAMPLES) % CHART_SAMPLES;
            for (int i = 1; i < n; ++i)
            {
                const int i0 = (start + i - 1) % CHART_SAMPLES;
                const int i1 = (start + i)     % CHART_SAMPLES;

                const float x0 = left + (float)(i - 1) / (n - 1) * pw;
                const float y0 = bottom - (float)c.values[i0] * ph;
                const float x1 = left + (float)i         / (n - 1) * pw;
                const float y1 = bottom - (float)c.values[i1] * ph;

                rt->DrawLine({ x0, y0 }, { x1, y1 }, brush, 0.75f);
            }
        };

        // Gray (regular temp) first, black (hot-spot) drawn on top so it stays legible.
        if (haveBase) drawSeries(base, pGray);
        drawSeries(hot, pBlack);
    }

    pGray->Release();
    pBlack->Release();
}

// Draw a process list panel: title, separator line, then entries (name left, value right).
// maxEntries is computed by the caller from tile height.
static void DrawProcessList(ID2D1RenderTarget* rt,
                             const wchar_t* title,
                             const ProcEntry* entries, int count,
                             D2D1_RECT_F area,
                             bool absMode,
                             int maxEntries)
{
    if (!g_pChartFmtL || !g_pChartFmtR) return;

    ID2D1SolidColorBrush* pBrush = nullptr;
    if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0.f, 0.f, 0.f, 1.f), &pBrush))) return;

    const float margin = 8.f;
    const float lineH  = g_fontSize * 1.4f;
    float y = area.top;

    // Title
    if (y + lineH <= area.bottom)
    {
        D2D1_RECT_F titleRect = { area.left + margin, y, area.right - margin, y + lineH };
        DrawTextEllipsis(rt, title, (UINT32)wcslen(title), g_pChartFmtL, titleRect, pBrush);
        y += lineH;
    }

    // Separator line under title (same style as dividers: 0.75 px stroke)
    if (y < area.bottom)
    {
        rt->DrawLine(
            D2D1::Point2F(area.left + margin, y),
            D2D1::Point2F(area.right - margin, y),
            pBrush, 0.75f);
        y += 2.f;
    }

    // Process entries
    int shown = 0;
    for (int i = 0; i < count && shown < maxEntries; ++i)
    {
        if (y + lineH > area.bottom) break;

        wchar_t pctStr[32];
        swprintf_s(pctStr, L"%.1f%%", entries[i].pct);
        const wchar_t* valStr = absMode ? entries[i].absStr : pctStr;
        UINT32 valLen = (UINT32)wcslen(valStr);

        // Measure value width for right alignment
        float valW = 0.f;
        if (g_pDWFactory && valLen > 0)
        {
            float areaW = area.right - area.left - 2.f * margin;
            IDWriteTextLayout* vl = nullptr;
            if (SUCCEEDED(g_pDWFactory->CreateTextLayout(valStr, valLen,
                    g_pChartFmtR, areaW, lineH, &vl)))
            {
                DWRITE_TEXT_METRICS tm = {};
                vl->GetMetrics(&tm);
                valW = tm.widthIncludingTrailingWhitespace;
                if (valW > areaW) valW = areaW;
                vl->Release();
            }
        }

        // Draw name (left-aligned, clipped to not overlap value)
        const float gap = 4.f;
        float nameMaxW = (area.right - margin) - (area.left + margin) - valW - gap;
        if (nameMaxW > 0.f)
        {
            D2D1_RECT_F nameRect = { area.left + margin, y,
                                     area.left + margin + nameMaxW, y + lineH };
            DrawTextEllipsis(rt, entries[i].name, (UINT32)wcslen(entries[i].name),
                             g_pChartFmtL, nameRect, pBrush);
        }

        // Draw value (right-aligned)
        D2D1_RECT_F valRect = { area.left + margin, y, area.right - margin, y + lineH };
        rt->DrawText(valStr, valLen, g_pChartFmtR, valRect, pBrush);

        y += lineH;
        ++shown;
    }

    pBrush->Release();
}

static constexpr int DIV_HIT = 4; // px hit area around dividers

// -----------------------------------------------------------------------
// Habr feed
// -----------------------------------------------------------------------
struct HabrArticle
{
    wchar_t title[256];
    char    url[512];
};
static const int MAX_HABR = 50;
static HabrArticle      g_habr[MAX_HABR] = {};
static int              g_habrCount      = 0;
static CRITICAL_SECTION g_habrCS;
static HANDLE           g_habrThread     = NULL;
static int              g_habrRefreshMin = 5; // configurable via config.json
static char             g_rssFeedUrl[1024] = "https://habr.com/ru/rss/all/all/";

// Link hover/click state (main-thread only, written by DrawHabrPanel)
static D2D1_RECT_F g_habrRects[MAX_HABR] = {};
static int         g_habrVisible          = 0;
static int         g_habrHover            = -1;

// -----------------------------------------------------------------------
// Weather
// -----------------------------------------------------------------------
struct WeatherData
{
    wchar_t ascii[5][72]; // ASCII weather icon (5 lines)
    wchar_t line1[128];   // description
    wchar_t line2[128];   // temp · humidity · wind
    wchar_t fc[3][64];    // 3-day forecast
    bool    valid;
};

static WeatherData      g_weather     = {};
static CRITICAL_SECTION g_weatherCS;
static char             g_location[256] = "";
static HANDLE           g_weatherThread = NULL;

// -----------------------------------------------------------------------
// Public IP
// -----------------------------------------------------------------------
static wchar_t          g_ipStr[64]         = L"";
static wchar_t          g_ipCountry[64]     = L"";
static CRITICAL_SECTION g_ipCS;
static HANDLE           g_ipThread          = NULL;
static HANDLE           g_ipWatchThread     = NULL;
static HANDLE           g_ipWatchStop       = NULL; // manual-reset event; signals watch thread to exit
static HWND             g_hwndForIp         = NULL;
static volatile LONG    g_ipFetchPending    = 0; // debounce: at most one WM_FETCH_IP queued

// Autostart
static bool g_autostart = false;

static void ApplyAutostart()
{
    const wchar_t* keyPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    const wchar_t* valueName = L"BlurBox";

    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, keyPath, 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS)
        return;

    if (g_autostart)
    {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(NULL, exePath, MAX_PATH);
        RegSetValueExW(hKey, valueName, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(exePath),
                       (DWORD)((wcslen(exePath) + 1) * sizeof(wchar_t)));
    }
    else
    {
        RegDeleteValueW(hKey, valueName);
    }

    RegCloseKey(hKey);
}

// Saved window-state globals (loaded from config before window creation)
static const int CFG_UNSET = -99999;
static int   g_cfgMonitorLeft = CFG_UNSET;
static int   g_cfgMonitorTop  = CFG_UNSET;
static int   g_cfgWinX        = CFG_UNSET;
static int   g_cfgWinY        = CFG_UNSET;
static int   g_cfgWinW        = -1;
static int   g_cfgWinH        = -1;

// -----------------------------------------------------------------------
// Config
// -----------------------------------------------------------------------
static void GetConfigPath(wchar_t* out, int len)
{
    GetModuleFileNameW(NULL, out, len);
    wchar_t* sl = wcsrchr(out, L'\\');
    if (sl) sl[1] = L'\0';
    wcsncat_s(out, len, L"config.json", _TRUNCATE);
}

static void SaveDefaultConfig()
{
    wchar_t path[MAX_PATH];
    GetConfigPath(path, MAX_PATH);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"w") == 0 && f)
    {
        fputs("{\n    \"location\": \"\",\n    \"autostart\": false,\n    \"debug\": false,\n    \"rss_feed_url\": \"https://habr.com/ru/rss/all/all/\"\n}\n", f);
        fclose(f);
    }
}

// -----------------------------------------------------------------------
// Monitor search helper (used for save/restore window position)
// -----------------------------------------------------------------------
struct MonitorSearchCtx
{
    int      targetLeft, targetTop;
    HMONITOR found;
    RECT     rcMonitor;
};

static BOOL CALLBACK FindMonitorProc(HMONITOR hMon, HDC, LPRECT, LPARAM lp)
{
    auto* ctx = reinterpret_cast<MonitorSearchCtx*>(lp);
    MONITORINFO mi = { sizeof(mi) };
    if (GetMonitorInfoW(hMon, &mi) &&
        mi.rcMonitor.left == ctx->targetLeft &&
        mi.rcMonitor.top  == ctx->targetTop)
    {
        ctx->found     = hMon;
        ctx->rcMonitor = mi.rcMonitor;
        return FALSE; // stop enumeration
    }
    return TRUE;
}

static void JsonEscapeStr(const char* src, char* dst, int dstLen)
{
    for (int i = 0, j = 0; src[i] && j < dstLen - 3; i++) {
        if (src[i] == '"' || src[i] == '\\') dst[j++] = '\\';
        dst[j++] = src[i];
    }
}

// Save current window position, size, monitor and divider to config.json
static void SaveWindowState(HWND hwnd)
{
    RECT rcWin;
    if (!GetWindowRect(hwnd, &rcWin)) return;

    HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(hMon, &mi)) return;

    int monL = mi.rcMonitor.left;
    int monT = mi.rcMonitor.top;
    int relX = rcWin.left - monL;
    int relY = rcWin.top  - monT;
    int winW = rcWin.right  - rcWin.left;
    int winH = rcWin.bottom - rcWin.top;

    char escaped[512] = {};
    JsonEscapeStr(g_location, escaped, sizeof(escaped));
    char escapedRss[1024] = {};
    JsonEscapeStr(g_rssFeedUrl, escapedRss, sizeof(escapedRss));

    wchar_t path[MAX_PATH];
    GetConfigPath(path, MAX_PATH);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"w") != 0 || !f) return;

    fprintf(f,
        "{\n"
        "    \"location\": \"%s\",\n"
        "    \"monitor_left\": %d,\n"
        "    \"monitor_top\": %d,\n"
        "    \"win_x\": %d,\n"
        "    \"win_y\": %d,\n"
        "    \"win_w\": %d,\n"
        "    \"win_h\": %d,\n"
        "    \"cpu_mode\": %d,\n"
        "    \"gpu_mode\": %d,\n"
        "    \"ram_mode\": %d,\n"
        "    \"disk_mode\": %d,\n"
        "    \"disk_sub_mode\": %d,\n"
        "    \"font_scale\": %.2f,\n"
        "    \"autostart\": %s,\n"
        "    \"debug\": %s,\n"
        "    \"show_vert_dividers\": %s,\n"
        "    \"rss_feed_url\": \"%s\",\n"
        "    \"proc_abs_cpu\": %d,\n"
        "    \"proc_abs_gpu\": %d,\n"
        "    \"proc_abs_ram\": %d,\n"
        "    \"proc_abs_disk\": %d,\n"
        "    \"col_count\": %d,\n",
        escaped, monL, monT, relX, relY, winW, winH,
        g_cpuMode, g_gpuMode, g_ramMode, g_diskMode, g_diskSubMode,
        (double)g_fontScale,
        g_autostart ? "true" : "false",
        g_debugEnabled ? "true" : "false",
        g_showVertDividers ? "true" : "false",
        escapedRss,
        g_procAbsMode[0] ? 1 : 0,
        g_procAbsMode[1] ? 1 : 0,
        g_procAbsMode[2] ? 1 : 0,
        g_procAbsMode[3] ? 1 : 0,
        g_colCount);
    // Column panel definitions
    for (int c = 0; c < g_colCount; c++)
    {
        fprintf(f, "    \"col_%d\": \"", c + 1);
        for (int r = 0; r < g_colPanelCount[c]; r++)
        {
            if (r > 0) fputc('|', f);
            fputs(g_colPanels[c][r], f);
        }
        fputs("\",\n", f);
        fprintf(f, "    \"col_%d_show_ydiv\": %s,\n", c + 1, g_colShowYDiv[c] ? "true" : "false");
    }
    // Vertical (column) dividers
    for (int k = 0; k < g_colCount - 1; k++)
        fprintf(f, "    \"col_div_%d\": %.2f,\n", k + 1, (double)g_colDivX[k]);
    // Horizontal (row) dividers — last value gets no trailing comma (valid JSON)
    {
        // Collect all row-divider entries so we know which is last
        struct { int c, r; } entries[MAX_COLS * MAX_PANELS_PER_COL];
        int ne = 0;
        for (int c = 0; c < g_colCount; c++)
            for (int r = 0; r < g_colPanelCount[c] - 1; r++)
                entries[ne++] = {c, r};
        for (int i = 0; i < ne; i++)
        {
            int c = entries[i].c, r = entries[i].r;
            const char* comma = (i < ne - 1) ? "," : "";
            fprintf(f, "    \"col_%d_ydiv_%d\": %.2f%s\n", c + 1, r + 1, (double)g_colDivY[c][r], comma);
        }
    }
    fputs("}\n", f);
    fclose(f);
}

// -----------------------------------------------------------------------
// Debug log (appends to aero_widget.log next to the executable when debug=true)
// -----------------------------------------------------------------------
static void DbgLog(const char* fmt, ...)
{
    if (!g_debugEnabled) return;

    wchar_t exeDir[MAX_PATH];
    GetModuleFileNameW(NULL, exeDir, MAX_PATH);
    wchar_t* sl = wcsrchr(exeDir, L'\\');
    if (sl) sl[1] = L'\0';
    wchar_t logPath[MAX_PATH];
    swprintf_s(logPath, L"%saero_widget.log", exeDir);

    static bool s_firstCall = true;
    const wchar_t* mode = s_firstCall ? L"w" : L"a";
    s_firstCall = false;

    FILE* f = nullptr;
    if (_wfopen_s(&f, logPath, mode) != 0 || !f) return;

    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}

// Find nth (0-based) occurrence of "key": "value" and extract value
static bool JsonStr(const char* json, const char* key, int nth, char* out, int outLen)
{
    char needle[256];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char* p = json;
    for (int i = 0; i <= nth; i++)
    {
        p = strstr(p, needle);
        if (!p) return false;
        if (i < nth) { p++; continue; }
    }
    p += strlen(needle);
    while (*p && (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    if (*p != '"') return false;
    p++;
    const char* e = strchr(p, '"');
    if (!e) return false;
    int n = (int)(e - p);
    if (n >= outLen) n = outLen - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}


// Find "key": <integer> and extract value
static bool JsonInt(const char* json, const char* key, int* out)
{
    char needle[256];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char* p = strstr(json, needle);
    if (!p) return false;
    p += strlen(needle);
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '-' && (*p < '0' || *p > '9')) return false;
    *out = atoi(p);
    return true;
}

// Find "key": <float> and extract value
static bool JsonDouble(const char* json, const char* key, double* out)
{
    char needle[256];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char* p = strstr(json, needle);
    if (!p) return false;
    p += strlen(needle);
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '-' && *p != '.' && (*p < '0' || *p > '9')) return false;
    *out = atof(p);
    return true;
}

static void LoadConfig()
{
    wchar_t path[MAX_PATH];
    GetConfigPath(path, MAX_PATH);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"r") != 0 || !f)
    {
        SaveDefaultConfig();
        return;
    }
    char buf[4096] = {};
    fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);

    char loc[256] = {};
    if (JsonStr(buf, "location", 0, loc, sizeof(loc)) && loc[0])
        strncpy_s(g_location, loc, _TRUNCATE);

    int iv = 0;
    if (JsonInt(buf, "monitor_left", &iv)) g_cfgMonitorLeft = iv;
    if (JsonInt(buf, "monitor_top",  &iv)) g_cfgMonitorTop  = iv;
    if (JsonInt(buf, "win_x",        &iv)) g_cfgWinX        = iv;
    if (JsonInt(buf, "win_y",        &iv)) g_cfgWinY        = iv;
    if (JsonInt(buf, "win_w",        &iv) && iv > 0) g_cfgWinW = iv;
    if (JsonInt(buf, "win_h",        &iv) && iv > 0) g_cfgWinH = iv;

    int habrMin = 0;
    if (JsonInt(buf, "habr_refresh_minutes", &habrMin) && habrMin > 0)
        g_habrRefreshMin = habrMin;

    {
        char rssUrl[1024] = {};
        if (JsonStr(buf, "rss_feed_url", 0, rssUrl, sizeof(rssUrl)) && rssUrl[0])
            strncpy_s(g_rssFeedUrl, rssUrl, _TRUNCATE);
        else
        {
            // Field missing — patch the file by inserting before the closing '}'
            FILE* fw = nullptr;
            if (_wfopen_s(&fw, path, L"w") == 0 && fw)
            {
                int len = (int)strlen(buf);
                int last = len - 1;
                while (last >= 0 && (buf[last] == '\n' || buf[last] == '\r' ||
                                      buf[last] == ' '  || buf[last] == '\t')) last--;
                if (last >= 0 && buf[last] == '}')
                {
                    buf[last] = '\0';
                    int end = last - 1;
                    while (end >= 0 && (buf[end] == ' ' || buf[end] == '\n' ||
                                         buf[end] == '\r' || buf[end] == '\t')) end--;
                    buf[end + 1] = '\0';
                    fprintf(fw, "%s,\n    \"rss_feed_url\": \"%s\"\n}\n", buf, g_rssFeedUrl);
                }
                else
                    fputs(buf, fw);
                fclose(fw);
            }
        }
    }

    int cpuMode = 0;
    if (JsonInt(buf, "cpu_mode", &cpuMode) && cpuMode >= 0 && cpuMode <= 3)
        g_cpuMode = cpuMode;

    int gpuMode = 0;
    if (JsonInt(buf, "gpu_mode", &gpuMode) && gpuMode >= 0 && gpuMode <= 4)
        g_gpuMode = gpuMode;

    int ramMode = 0;
    if (JsonInt(buf, "ram_mode", &ramMode) && ramMode >= 0 && ramMode <= 1)
        g_ramMode = ramMode;

    int diskMode = 0;
    if (JsonInt(buf, "disk_mode", &diskMode) && diskMode >= 0)
        g_diskMode = diskMode; // validated against g_diskCount after enumeration

    int diskSubMode = 0;
    if (JsonInt(buf, "disk_sub_mode", &diskSubMode) && diskSubMode >= 0 && diskSubMode <= 2)
        g_diskSubMode = diskSubMode;

    {
        static const char* procAbsKeys[NUM_CHARTS] = {
            "proc_abs_cpu", "proc_abs_gpu", "proc_abs_ram", "proc_abs_disk"
        };
        for (int i = 0; i < NUM_CHARTS; i++)
        {
            int v = 0;
            if (JsonInt(buf, procAbsKeys[i], &v))
                g_procAbsMode[i] = (v != 0);
        }
    }

    // autostart: look for "true" literal after the key
    {
        const char* p = strstr(buf, "\"autostart\"");
        if (p)
        {
            p += strlen("\"autostart\"");
            while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n' || *p == '\r') p++;
            g_autostart = (strncmp(p, "true", 4) == 0);
        }
    }
    // debug: look for "true" literal after the key (default false)
    {
        const char* p = strstr(buf, "\"debug\"");
        if (p)
        {
            p += strlen("\"debug\"");
            while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n' || *p == '\r') p++;
            g_debugEnabled = (strncmp(p, "true", 4) == 0);
        }
    }
    // vertical divider visibility: look for "true"/"false" literal after the key (default true)
    {
        const char* p = strstr(buf, "\"show_vert_dividers\"");
        if (p)
        {
            p += strlen("\"show_vert_dividers\"");
            while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n' || *p == '\r') p++;
            g_showVertDividers = (strncmp(p, "false", 5) != 0);
        }
    }
    double fontScale = 0.0;
    if (JsonDouble(buf, "font_scale", &fontScale) && fontScale > 0.0)
    {
        g_fontScale = (float)fontScale;
        PAD         = 6.f  * g_fontScale;
        VEDGE       = 14.f * g_fontScale;
        g_fontSize  = 14.f * g_fontScale;
    }

    // Column layout
    SetDefaultLayout();
    {
        int colCount = 0;
        if (JsonInt(buf, "col_count", &colCount) && colCount >= 1 && colCount <= MAX_COLS)
        {
            g_colCount = colCount;
            for (int c = 0; c < g_colCount; c++)
            {
                char key[32], val[256] = {};
                snprintf(key, sizeof(key), "col_%d", c + 1);
                if (JsonStr(buf, key, 0, val, sizeof(val)) && val[0])
                    g_colPanelCount[c] = ParsePanelList(val, g_colPanels[c], MAX_PANELS_PER_COL);

                // Per-column horizontal divider visibility
                char ydivKey[32];
                snprintf(ydivKey, sizeof(ydivKey), "\"col_%d_show_ydiv\"", c + 1);
                const char* p = strstr(buf, ydivKey);
                if (p)
                {
                    p += strlen(ydivKey);
                    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n' || *p == '\r') p++;
                    g_colShowYDiv[c] = (strncmp(p, "false", 5) != 0);
                }
            }
        }
        // Vertical dividers
        for (int k = 0; k < g_colCount - 1; k++)
        {
            char key[32]; double dv = 0.0;
            snprintf(key, sizeof(key), "col_div_%d", k + 1);
            if (JsonDouble(buf, key, &dv) && dv > 0.0)
            { g_colDivX[k] = (float)dv; g_colDivXSet[k] = true; }
        }
        // Horizontal (row) dividers
        for (int c = 0; c < g_colCount; c++)
        {
            for (int r = 0; r < g_colPanelCount[c] - 1; r++)
            {
                char key[32]; double dv = 0.0;
                snprintf(key, sizeof(key), "col_%d_ydiv_%d", c + 1, r + 1);
                if (JsonDouble(buf, key, &dv) && dv > 0.0)
                { g_colDivY[c][r] = (float)dv; g_colDivYSet[c][r] = true; }
            }
        }
    }

    ApplyAutostart();
}

// -----------------------------------------------------------------------
// Weather fetch thread
// -----------------------------------------------------------------------
static const wchar_t* WeekDay(const char* date)
{
    static const wchar_t* wd[] = { L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat" };
    int y, m, d;
    if (sscanf_s(date, "%d-%d-%d", &y, &m, &d) != 3) return L"?";
    SYSTEMTIME st = {};
    st.wYear = (WORD)y; st.wMonth = (WORD)m; st.wDay = (WORD)d;
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft)) return L"?";
    FileTimeToSystemTime(&ft, &st);
    return wd[st.wDayOfWeek % 7];
}

// Fetch a URL path from wttr.in over HTTPS; caller must free() the result.
static char* WttrGet(const wchar_t* path, const wchar_t* ua = L"AeroWidget/1.0")
{
    wchar_t url[1024];
    swprintf_s(url, L"https://wttr.in%s", path);

    HINTERNET hInet = InternetOpenW(ua, INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInet) { DbgLog("[weather] InternetOpen failed\n"); return nullptr; }

    // Increase receive timeout to 90 s — wttr.in sends chunked data slowly
    DWORD recvTimeout = 90000;
    InternetSetOption(hInet, INTERNET_OPTION_RECEIVE_TIMEOUT, &recvTimeout, sizeof(recvTimeout));

    HINTERNET hUrl = InternetOpenUrlW(hInet, url, nullptr, 0,
        INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);

    char* body = nullptr;
    int   blen = 0;

    if (hUrl)
    {
        if (g_debugEnabled)
        {
            // Log HTTP status
            DWORD statusCode = 0, scLen = sizeof(statusCode);
            if (HttpQueryInfoW(hUrl, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                    &statusCode, &scLen, nullptr))
                DbgLog("[weather] HTTP status: %lu\n", statusCode);
            else
                DbgLog("[weather] HTTP status: (unknown)\n");

            // Log Content-Length (may be absent for chunked)
            wchar_t clBuf[64] = {}; DWORD clLen = sizeof(clBuf) - 2;
            if (HttpQueryInfoW(hUrl, HTTP_QUERY_CONTENT_LENGTH, clBuf, &clLen, nullptr))
                DbgLog("[weather] Content-Length: %ls\n", clBuf);
            else
                DbgLog("[weather] Content-Length: (absent)\n");

            // Log Transfer-Encoding
            wchar_t teBuf[64] = {}; DWORD teLen = sizeof(teBuf) - 2;
            if (HttpQueryInfoW(hUrl, HTTP_QUERY_TRANSFER_ENCODING, teBuf, &teLen, nullptr))
                DbgLog("[weather] Transfer-Encoding: %ls\n", teBuf);
            else
                DbgLog("[weather] Transfer-Encoding: (absent)\n");
        }

        const int CAP = 256 * 1024;
        body = (char*)malloc(CAP);
        if (body)
        {
            DWORD rd = 0;
            int iter = 0;
            while (blen < CAP - 1)
            {
                DWORD toRead = (DWORD)min(4096, CAP - 1 - blen);
                BOOL rdOk = InternetReadFile(hUrl, body + blen, toRead, &rd);
                if (g_debugEnabled)
                    DbgLog("[weather]   iter %d: ReadData ok=%d rd=%lu total=%d\n",
                           iter, (int)rdOk, rd, blen + (int)rd);
                if (!rdOk || rd == 0) break;
                blen += (int)rd;
                iter++;
            }
            body[blen] = '\0';
            if (blen == 0) { free(body); body = nullptr; }
        }
        InternetCloseHandle(hUrl);
    }
    else
    {
        DbgLog("[weather] connect failed (hUrl is null)\n");
    }
    InternetCloseHandle(hInet);
    return body;
}

// Generic HTTPS GET – caller must free() result.
static char* HttpGetUrl(const wchar_t* url, const wchar_t* ua = L"AeroWidget/1.0")
{
    HINTERNET hInet = InternetOpenW(ua, INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInet) { DbgLog("[http] InternetOpen failed\n"); return nullptr; }

    DWORD recvTimeout = 30000;
    InternetSetOption(hInet, INTERNET_OPTION_RECEIVE_TIMEOUT, &recvTimeout, sizeof(recvTimeout));

    HINTERNET hUrl = InternetOpenUrlW(hInet, url, nullptr, 0,
        INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);

    char* body = nullptr;
    int   blen = 0;

    if (hUrl)
    {
        const int CAP = 512 * 1024;
        body = (char*)malloc(CAP);
        if (body)
        {
            DWORD rd = 0;
            while (blen < CAP - 1)
            {
                DWORD toRead = (DWORD)min(4096, CAP - 1 - blen);
                if (!InternetReadFile(hUrl, body + blen, toRead, &rd) || rd == 0) break;
                blen += (int)rd;
            }
            body[blen] = '\0';
            if (blen == 0) { free(body); body = nullptr; }
        }
        InternetCloseHandle(hUrl);
    }
    else
    {
        char urlBuf[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, url, -1, urlBuf, sizeof(urlBuf) - 1, nullptr, nullptr);
        DbgLog("[http] connect failed: %.200s\n", urlBuf);
    }
    InternetCloseHandle(hInet);
    return body;
}

// -----------------------------------------------------------------------
// Habr RSS helpers
// -----------------------------------------------------------------------

// Decode basic HTML entities in-place (UTF-8)
static void DecodeHtmlEntities(char* str)
{
    char* src = str;
    char* dst = str;
    while (*src)
    {
        if (*src != '&') { *dst++ = *src++; continue; }
        if      (strncmp(src, "&amp;",  5) == 0) { *dst++ = '&';  src += 5; }
        else if (strncmp(src, "&lt;",   4) == 0) { *dst++ = '<';  src += 4; }
        else if (strncmp(src, "&gt;",   4) == 0) { *dst++ = '>';  src += 4; }
        else if (strncmp(src, "&quot;", 6) == 0) { *dst++ = '"';  src += 6; }
        else if (strncmp(src, "&apos;", 6) == 0) { *dst++ = '\''; src += 6; }
        else if (src[1] == '#')
        {
            const char* semi = strchr(src + 2, ';');
            if (semi)
            {
                int cp = (src[2] == 'x' || src[2] == 'X')
                         ? (int)strtol(src + 3, nullptr, 16)
                         : atoi(src + 2);
                if (cp < 0x80)
                    *dst++ = (char)cp;
                else if (cp < 0x800)
                { *dst++ = (char)(0xC0 | (cp >> 6)); *dst++ = (char)(0x80 | (cp & 0x3F)); }
                else
                { *dst++ = (char)(0xE0 | (cp >> 12));
                  *dst++ = (char)(0x80 | ((cp >> 6) & 0x3F));
                  *dst++ = (char)(0x80 | (cp & 0x3F)); }
                src = (char*)semi + 1;
            }
            else { *dst++ = *src++; }
        }
        else { *dst++ = *src++; }
    }
    *dst = '\0';
}

// Extract content of <tag>…</tag> in the byte range [begin, end).
// Handles <![CDATA[…]]>. Returns true on success.
static bool XmlTag(const char* begin, const char* end,
                   const char* tag, char* out, int outLen)
{
    char openTag[128], closeTag[128];
    snprintf(openTag,  sizeof(openTag),  "<%s>",  tag);
    snprintf(closeTag, sizeof(closeTag), "</%s>", tag);

    const char* s = strstr(begin, openTag);
    if (!s || s >= end) return false;
    const char* content = s + strlen(openTag);
    const char* e = strstr(content, closeTag);
    if (!e || e >= end) return false;

    int len = (int)(e - content);
    // CDATA?
    if (len > 9 && strncmp(content, "<![CDATA[", 9) == 0)
    {
        const char* cd = strstr(content + 9, "]]>");
        if (cd && cd < end)
        {
            int cLen = (int)(cd - (content + 9));
            if (cLen >= outLen) cLen = outLen - 1;
            memcpy(out, content + 9, cLen);
            out[cLen] = '\0';
            return true;
        }
    }
    if (len >= outLen) len = outLen - 1;
    memcpy(out, content, len);
    out[len] = '\0';
    return true;
}

// -----------------------------------------------------------------------
// Habr fetch thread
// -----------------------------------------------------------------------
static DWORD WINAPI HabrThreadProc(LPVOID)
{
    DbgLog("[rss] fetching: %s\n", g_rssFeedUrl);
    wchar_t wFeedUrl[1024];
    MultiByteToWideChar(CP_UTF8, 0, g_rssFeedUrl, -1, wFeedUrl, 1024);
    char* body = HttpGetUrl(wFeedUrl);
    if (!body) { DbgLog("[rss] fetch failed\n"); return 0; }
    DbgLog("[rss] fetched %d bytes\n", (int)strlen(body));

    HabrArticle* articles = new HabrArticle[MAX_HABR]();
    int count = 0;

    const char* p = body;
    while (count < MAX_HABR)
    {
        const char* itemStart = strstr(p, "<item>");
        if (!itemStart) break;
        const char* itemEnd = strstr(itemStart + 6, "</item>");
        if (!itemEnd) break;

        char title[512] = {};
        char url[512]   = {};

        XmlTag(itemStart, itemEnd, "title", title, sizeof(title));
        DecodeHtmlEntities(title);

        if (!XmlTag(itemStart, itemEnd, "link", url, sizeof(url)))
            XmlTag(itemStart, itemEnd, "guid", url, sizeof(url));

        if (title[0] && url[0])
        {
            MultiByteToWideChar(CP_UTF8, 0, title, -1,
                                articles[count].title, 256);
            strncpy_s(articles[count].url, url, _TRUNCATE);
            count++;
        }
        p = itemEnd + 7; // past </item>
    }
    free(body);
    DbgLog("[rss] parsed %d items\n", count);

    EnterCriticalSection(&g_habrCS);
    memcpy(g_habr, articles, sizeof(HabrArticle) * count);
    g_habrCount = count;
    LeaveCriticalSection(&g_habrCS);
    delete[] articles;
    return 0;
}

static void StartHabrFetch()
{
    if (g_habrThread) { CloseHandle(g_habrThread); g_habrThread = NULL; }
    g_habrThread = CreateThread(nullptr, 0, HabrThreadProc, nullptr, 0, nullptr);
}

// -----------------------------------------------------------------------
// Draw Habr panel
// -----------------------------------------------------------------------
static void DrawHabrPanel(ID2D1RenderTarget* rt, D2D1_RECT_F area, int hoverIdx)
{
    if (!g_pChartFmtL || !g_pDWFactory) return;

    static HabrArticle articles[MAX_HABR];
    int count = 0;
    EnterCriticalSection(&g_habrCS);
    memcpy(articles, g_habr, sizeof(HabrArticle) * g_habrCount);
    count = g_habrCount;
    LeaveCriticalSection(&g_habrCS);

    g_habrVisible = 0;

    ID2D1SolidColorBrush* pBrush = nullptr;
    if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0.f, 0.f, 0.f, 1.f), &pBrush)))
        return;

    if (count == 0)
    {
        ID2D1SolidColorBrush* pDim = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(D2D1::ColorF(0.f, 0.f, 0.f, 0.5f), &pDim)))
        {
            const wchar_t* msg = L"Загрузка Habr…";
            rt->DrawText(msg, (UINT32)wcslen(msg), g_pChartFmtL, area, pDim);
            pDim->Release();
        }
        pBrush->Release();
        return;
    }

    const float lh = g_fontSize * 1.4f;
    float y = area.top;

    for (int i = 0; i < count && y + lh <= area.bottom; i++)
    {
        D2D1_RECT_F r = { area.left, y, area.right, y + lh };
        g_habrRects[i] = r;
        g_habrVisible  = i + 1;

        DrawTextEllipsis(rt, articles[i].title, (UINT32)wcslen(articles[i].title),
                         g_pChartFmtL, r, pBrush);

        if (i == hoverIdx)
        {
            // Measure text width for underline
            IDWriteTextLayout* layout = nullptr;
            if (SUCCEEDED(g_pDWFactory->CreateTextLayout(
                    articles[i].title, (UINT32)wcslen(articles[i].title),
                    g_pChartFmtL, r.right - r.left, lh, &layout)))
            {
                DWRITE_TEXT_METRICS tm = {};
                layout->GetMetrics(&tm);
                float textW = tm.widthIncludingTrailingWhitespace;
                if (textW > r.right - r.left) textW = r.right - r.left;
                float underY = y + lh - 2.f;
                rt->DrawLine({ area.left, underY },
                             { area.left + textW, underY },
                             pBrush, 0.75f);
                layout->Release();
            }
        }

        y += lh;
    }

    pBrush->Release();
}

static DWORD WINAPI WeatherThreadProc(LPVOID)
{
    // Build URL-encoded location (empty = auto-detect by IP)
    wchar_t wloc[256] = {};
    MultiByteToWideChar(CP_UTF8, 0, g_location, -1, wloc, 256);

    wchar_t encLoc[512] = {};
    for (int i = 0, j = 0; wloc[i] && j < 508; i++)
    {
        if (wloc[i] == L' ') { encLoc[j++] = L'%'; encLoc[j++] = L'2'; encLoc[j++] = L'0'; }
        else encLoc[j++] = wloc[i];
    }

    WeatherData wd = {};

    // ── Request 1: JSON (current conditions + forecast) ──────────────────
    {
        wchar_t path[512];
        swprintf_s(path, L"/%s?format=j1&m", encLoc);
        DbgLog("[weather] === WeatherThreadProc ===\n");
        char* body = WttrGet(path, L"AeroWidget/1.0");
        if (body)
        {
            int blen = (int)strlen(body);
            DbgLog("[weather] body length: %d\n", blen);

            // Log first 200 chars of body to confirm it looks like JSON
            char preview[201] = {};
            strncpy_s(preview, body, 200);
            DbgLog("[weather] body[0..200]: %.200s\n\n", preview);

            char tmp[256];

            // Description
            const char* wdPos = strstr(body, "\"weatherDesc\"");
            if (wdPos && JsonStr(wdPos, "value", 0, tmp, sizeof(tmp)))
                MultiByteToWideChar(CP_UTF8, 0, tmp, -1, wd.line1, 128);

            // Current stats
            char tempC[16]={}, humStr[16]={}, windStr[16]={};
            JsonStr(body, "temp_C",        0, tempC,   sizeof(tempC));
            JsonStr(body, "humidity",      0, humStr,  sizeof(humStr));
            JsonStr(body, "windspeedKmph", 0, windStr, sizeof(windStr));

            wchar_t wtmpC[16]={}, whum[16]={}, wwnd[16]={};
            MultiByteToWideChar(CP_UTF8, 0, tempC,   -1, wtmpC, 16);
            MultiByteToWideChar(CP_UTF8, 0, humStr,  -1, whum,  16);
            MultiByteToWideChar(CP_UTF8, 0, windStr, -1, wwnd,  16);
            swprintf_s(wd.line2, L"%s\u00B0C \u00B7 %s%% \u00B7 %s\u00A0km/h",
                       wtmpC, whum, wwnd);

            // 3-day forecast.
            {
                const char* wStart = strstr(body, "\"weather\":");
                DbgLog("[weather] \"weather\":  %s\n", wStart ? "FOUND" : "NOT FOUND");

                if (wStart)
                {
                    DbgLog("[weather] offset of \"weather\": %d\n", (int)(wStart - body));

                    for (int day = 0; day < 3; day++)
                    {
                        char date[32]={}, maxT[16]={}, minT[16]={};

                        bool okDate = JsonStr(wStart, "date",     day, date, sizeof(date));
                        bool okMax  = JsonStr(wStart, "maxtempC", day, maxT, sizeof(maxT));
                        bool okMin  = JsonStr(wStart, "mintempC", day, minT, sizeof(minT));

                        DbgLog("[weather] day[%d]: date=%s(%d) maxT=%s(%d) minT=%s(%d)\n",
                               day,
                               okDate ? date : "FAIL", okDate,
                               okMax  ? maxT : "FAIL", okMax,
                               okMin  ? minT : "FAIL", okMin);

                        if (!okDate || !okMax || !okMin) break;

                        wchar_t wmaxT[16]={}, wminT[16]={};
                        MultiByteToWideChar(CP_UTF8, 0, maxT, -1, wmaxT, 16);
                        MultiByteToWideChar(CP_UTF8, 0, minT, -1, wminT, 16);
                        swprintf_s(wd.fc[day], L"%-3s  %s\u2013%s\u00B0C",
                                   WeekDay(date), wminT, wmaxT);
                    }
                }
            }

            free(body);
        }
        else
        {
            DbgLog("[weather] body is NULL (request failed)\n");
        }
    }

    // ── Request 2: ASCII art (current weather only, no ANSI codes) ───────
    // curl User-Agent is required — wttr.in returns HTML for other agents
    {
        wchar_t path[512];
        swprintf_s(path, L"/%s?T&q&0&m", encLoc);
        char* ab = WttrGet(path, L"curl/7.68.0");
        DbgLog("[weather] ascii body: %s\n", ab ? "OK" : "NULL");
        if (ab)
        {
            char apreview[201] = {};
            strncpy_s(apreview, ab, 200);
            DbgLog("[weather] ascii[0..200]: %.200s\n", apreview);

            int lineIdx = 0;
            const char* lp = ab;
            while (*lp && lineIdx < 5)
            {
                const char* le = lp;
                while (*le && *le != '\n') le++;

                int len = (int)(le - lp);
                if (len > 0 && lp[len - 1] == '\r') len--;

                // Strip ANSI escape sequences (e.g. \033[38;5;240;1m) that
                // wttr.in sometimes emits even when ?T is requested.
                char stripped[256] = {};
                int slen = 0;
                for (int i = 0; i < len && slen < 255; i++)
                {
                    if ((unsigned char)lp[i] == 0x1B && i + 1 < len && lp[i + 1] == '[')
                    {
                        i += 2;
                        while (i < len && !((lp[i] >= 'A' && lp[i] <= 'Z') || (lp[i] >= 'a' && lp[i] <= 'z')))
                            i++;
                    }
                    else
                    {
                        stripped[slen++] = lp[i];
                    }
                }

                // wttr.in ASCII art lines always start with a space.
                // Skip lines that don't (e.g. site-name headers like "wttr.in").
                bool hasContent = (slen > 0 && stripped[0] == ' ');

                if (hasContent)
                {
                    if (slen > 71) slen = 71;
                    int wlen = MultiByteToWideChar(CP_UTF8, 0, stripped, slen,
                                                   wd.ascii[lineIdx], 72);
                    if (wlen > 0) wd.ascii[lineIdx][wlen] = L'\0';
                    lineIdx++;
                }

                lp = (*le == '\n') ? le + 1 : le;
            }
            DbgLog("[weather] ascii lines parsed: %d\n", lineIdx);
            free(ab);
        }
    }

    wd.valid = true;
    EnterCriticalSection(&g_weatherCS);
    g_weather = wd;
    LeaveCriticalSection(&g_weatherCS);
    return 0;
}

static void StartWeatherFetch()
{
    if (g_weatherThread) { CloseHandle(g_weatherThread); g_weatherThread = NULL; }
    g_weatherThread = CreateThread(nullptr, 0, WeatherThreadProc, nullptr, 0, nullptr);
}

// -----------------------------------------------------------------------
// Public IP fetch
// -----------------------------------------------------------------------
// Validate that s looks like an IPv4/IPv6 address (no HTML, no spaces beyond trimming)
static bool IsValidIp(const char* s)
{
    if (!s || !*s) return false;
    int len = (int)strlen(s);
    if (len < 7 || len > 45) return false;
    for (int i = 0; i < len; i++)
    {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || c == '.' || c == ':' ||
              (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

// Trim trailing whitespace in-place, return new length
static int TrimRight(char* s)
{
    int len = (int)strlen(s);
    while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r' || s[len-1] == ' ' || s[len-1] == '\t'))
        s[--len] = '\0';
    return len;
}

// lParam: 0 = timer/initial, 1 = interface-changed (adds reconnect delay)
static DWORD WINAPI IpThreadProc(LPVOID lParam)
{
    if (lParam) Sleep(3000); // wait for VPN to finish reconnecting

    // Fallback list: services that return plain-text IP.
    // Ordered so that services accessible in Russia without VPN come first;
    // when furious is active, all go through its proxy anyway.
    static const wchar_t* kServices[] = {
        L"https://api.ipify.org",
        L"https://checkip.amazonaws.com",
        L"https://ip.sb",
        L"https://icanhazip.com",
        L"https://ifconfig.me/ip",
    };

    DbgLog("[ip] starting IP fetch\n");
    wchar_t wip[64] = L"";
    for (int i = 0; i < (int)(sizeof(kServices)/sizeof(kServices[0])); i++)
    {
        char svcBuf[128] = {};
        WideCharToMultiByte(CP_UTF8, 0, kServices[i], -1, svcBuf, sizeof(svcBuf) - 1, nullptr, nullptr);
        char* body = HttpGetUrl(kServices[i], L"AeroWidget/1.0");
        if (body)
        {
            TrimRight(body);
            if (IsValidIp(body))
            {
                MultiByteToWideChar(CP_UTF8, 0, body, -1, wip, 64);
                DbgLog("[ip] got IP from %s: %s\n", svcBuf, body);
            }
            else
            {
                DbgLog("[ip] %s: invalid response: %.64s\n", svcBuf, body);
            }
            free(body);
        }
        else
        {
            DbgLog("[ip] %s: request failed\n", svcBuf);
        }
        if (wip[0]) break;
    }
    if (!wip[0])
        DbgLog("[ip] all services failed, IP unavailable\n");

    wchar_t wcountry[64] = L"";
    if (wip[0])
    {
        // Build URL: https://ipapi.co/{ip}/country_name/
        wchar_t geoUrl[128];
        swprintf_s(geoUrl, L"https://ipapi.co/%s/country_name/", wip);
        char* gbody = HttpGetUrl(geoUrl, L"AeroWidget/1.0");
        if (gbody)
        {
            TrimRight(gbody);
            // Validate: plain text country name, no HTML tags, reasonable length
            int glen = (int)strlen(gbody);
            bool ok = glen > 0 && glen < 56;
            for (int i = 0; ok && i < glen; i++)
            {
                unsigned char ch = (unsigned char)gbody[i];
                if (ch == '<' || ch == '\n' || ch == '\r') ok = false;
            }
            if (ok)
            {
                MultiByteToWideChar(CP_UTF8, 0, gbody, -1, wcountry, 64);
                DbgLog("[ip] country: %s\n", gbody);
            }
            else
            {
                DbgLog("[ip] country lookup: invalid response (len=%d)\n", glen);
            }
            free(gbody);
        }
        else
        {
            DbgLog("[ip] country lookup failed\n");
        }
    }

    EnterCriticalSection(&g_ipCS);
    wcscpy_s(g_ipStr, wip[0] ? wip : L"—");
    wcscpy_s(g_ipCountry, wcountry);
    LeaveCriticalSection(&g_ipCS);

    if (g_hwndForIp) PostMessageW(g_hwndForIp, WM_REDRAW_IP, 0, 0);
    return 0;
}

static void StartIpFetch(LPVOID cause = nullptr)
{
    if (g_ipThread)
    {
        if (WaitForSingleObject(g_ipThread, 0) == WAIT_TIMEOUT)
            return; // previous fetch still running, skip
        CloseHandle(g_ipThread);
        g_ipThread = NULL;
    }
    g_ipThread = CreateThread(nullptr, 0, IpThreadProc, cause, 0, nullptr);
}

// Watch thread: blocks on NotifyAddrChange (iphlpapi, no winsock needed).
// When a local address changes (VPN up/down), posts WM_FETCH_IP with lParam=1.
static DWORD WINAPI IpWatchThreadProc(LPVOID)
{
    while (true)
    {
        HANDLE hNet = INVALID_HANDLE_VALUE;
        OVERLAPPED ov = {};
        ov.hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!ov.hEvent) break;

        DWORD ret = NotifyAddrChange(&hNet, &ov);
        if (ret == ERROR_IO_PENDING)
        {
            HANDLE wait[2] = { ov.hEvent, g_ipWatchStop };
            DWORD w = WaitForMultipleObjects(2, wait, FALSE, INFINITE);
            CloseHandle(ov.hEvent);
            if (w != WAIT_OBJECT_0) break; // stop event or error — exit thread
            // Address changed — debounce
            if (InterlockedCompareExchange(&g_ipFetchPending, 1, 0) == 0)
                if (g_hwndForIp) PostMessageW(g_hwndForIp, WM_FETCH_IP, 1, 0);
        }
        else
        {
            CloseHandle(ov.hEvent);
            Sleep(5000); // unexpected error, avoid tight loop
        }
    }
    return 0;
}

static void StartIpWatchThread()
{
    g_ipWatchStop   = CreateEventW(nullptr, TRUE, FALSE, nullptr); // manual-reset
    g_ipWatchThread = CreateThread(nullptr, 0, IpWatchThreadProc, nullptr, 0, nullptr);
}

// -----------------------------------------------------------------------
// Draw IP panel
// -----------------------------------------------------------------------
static void DrawIpPanel(ID2D1RenderTarget* rt, D2D1_RECT_F area)
{
    if (!g_pChartFmtL) return;
    ID2D1SolidColorBrush* pBrush = nullptr;
    if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0.f, 0.f, 0.f, 1.f), &pBrush))) return;

    const float lh = g_fontSize * 1.4f;
    float y = area.top;

    wchar_t ip[64];
    wchar_t country[64];
    EnterCriticalSection(&g_ipCS);
    wcscpy_s(ip, g_ipStr[0] ? g_ipStr : L"…");
    wcscpy_s(country, g_ipCountry);
    LeaveCriticalSection(&g_ipCS);

    wchar_t text[160];
    if (country[0])
        swprintf_s(text, L"IP  %s (%s)", ip, country);
    else
        swprintf_s(text, L"IP  %s", ip);
    if (y + lh <= area.bottom)
    {
        D2D1_RECT_F r = { area.left, y, area.right, y + lh };
        DrawTextEllipsis(rt, text, (UINT32)wcslen(text), g_pChartFmtL, r, pBrush);
    }
    pBrush->Release();
}

// -----------------------------------------------------------------------
// Draw weather panel
// -----------------------------------------------------------------------
static void DrawWeather(ID2D1RenderTarget* rt, const WeatherData& wd, D2D1_RECT_F area)
{
    g_weatherRect = area;
    if (!wd.valid || !g_pChartFmtL) return;

    ID2D1SolidColorBrush* pBrush = nullptr;
    if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0.f, 0.f, 0.f, 1.f), &pBrush))) return;

    const float lh = g_fontSize * 1.4f;
    float y = area.top;

    // ASCII art icon (monospace, compact)
    if (g_pMonoFmt)
    {
        for (int j = 0; j < 5; j++)
        {
            if (!wd.ascii[j][0]) { y += lh; continue; }
            if (y + lh > area.bottom) break;
            D2D1_RECT_F r = { area.left, y, area.right, y + lh };
            DrawTextEllipsis(rt, wd.ascii[j], (UINT32)wcslen(wd.ascii[j]),
                             g_pMonoFmt, r, pBrush);
            y += lh;
        }
        y += lh * 0.3f;
    }
    else
    {
        y += lh * 5 + lh * 0.3f;
    }

    auto drawLine = [&](const wchar_t* text)
    {
        if (!text[0] || y + lh > area.bottom) return;
        D2D1_RECT_F r = { area.left, y, area.right, y + lh };
        DrawTextEllipsis(rt, text, (UINT32)wcslen(text), g_pChartFmtL, r, pBrush);
        y += lh;
    };

    drawLine(wd.line1);
    drawLine(wd.line2);
    y += lh * 0.5f;

    for (int i = 0; i < 3; i++)
        drawLine(wd.fc[i]);

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
static PDH_HCOUNTER g_pdhDiskCtr      = NULL;
static PDH_HCOUNTER g_pdhCpuFreqCtr   = NULL;
static PDH_HCOUNTER g_pdhCpuPerfCtr   = NULL;
static PDH_HCOUNTER g_pdhDiskByteCtr  = NULL;
static PDH_HCOUNTER g_pdhDiskReadCtr  = NULL;
static PDH_HCOUNTER g_pdhDiskWriteCtr = NULL;
static PDH_HCOUNTER g_pdhGpuMemCtr   = NULL;
static ULONGLONG    g_gpuTotalVram   = 0;

// -----------------------------------------------------------------------
// Device name helpers
// -----------------------------------------------------------------------
static void GetCpuName(wchar_t* buf, int len)
{
    if (!buf || len <= 0) return;
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

    wchar_t partBuf[128] = {};
    DWORD   speed        = 0;

    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IWbemLocator* pLoc = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_IWbemLocator, (void**)&pLoc)))
    {
        IWbemServices* pSvc = nullptr;
        BSTR bns = SysAllocString(L"root\\cimv2");
        if (SUCCEEDED(pLoc->ConnectServer(bns, nullptr, nullptr, nullptr,
                                          0, nullptr, nullptr, &pSvc)))
        {
            CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE,
                nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                nullptr, EOAC_NONE);

            BSTR wql   = SysAllocString(L"WQL");
            BSTR query = SysAllocString(
                L"SELECT PartNumber,ConfiguredClockSpeed,Speed FROM Win32_PhysicalMemory");
            IEnumWbemClassObject* pEnum = nullptr;
            if (SUCCEEDED(pSvc->ExecQuery(wql, query,
                                          WBEM_FLAG_FORWARD_ONLY, nullptr, &pEnum)))
            {
                IWbemClassObject* pObj = nullptr; ULONG ret = 0;
                while (pEnum->Next(WBEM_INFINITE, 1, &pObj, &ret) == S_OK)
                {
                    VARIANT vt;
                    if (!partBuf[0] &&
                        SUCCEEDED(pObj->Get(L"PartNumber", 0, &vt, nullptr, nullptr)) &&
                        vt.vt == VT_BSTR && vt.bstrVal)
                    {
                        wcscpy_s(partBuf, vt.bstrVal);
                        for (int i = (int)wcslen(partBuf) - 1; i >= 0 && partBuf[i] == L' '; --i)
                            partBuf[i] = L'\0';
                        VariantClear(&vt);
                    }
                    if (!speed &&
                        SUCCEEDED(pObj->Get(L"ConfiguredClockSpeed", 0, &vt, nullptr, nullptr)) &&
                        (vt.vt == VT_I4 || vt.vt == VT_UI4))
                    {
                        speed = (vt.vt == VT_I4) ? (DWORD)vt.lVal : vt.uintVal;
                        VariantClear(&vt);
                    }
                    if (!speed &&
                        SUCCEEDED(pObj->Get(L"Speed", 0, &vt, nullptr, nullptr)) &&
                        (vt.vt == VT_I4 || vt.vt == VT_UI4))
                    {
                        speed = (vt.vt == VT_I4) ? (DWORD)vt.lVal : vt.uintVal;
                        VariantClear(&vt);
                    }
                    pObj->Release();
                }
                pEnum->Release();
            }
            SysFreeString(query);
            SysFreeString(wql);
            pSvc->Release();
        }
        SysFreeString(bns);
        pLoc->Release();
    }
    if (hrCom == S_OK || hrCom == S_FALSE) CoUninitialize();

    if (partBuf[0])
    {
        if (speed)
            swprintf_s(buf, len, L"%s (%llu GB, %u MHz)", partBuf, gb, speed);
        else
            swprintf_s(buf, len, L"%s (%llu GB)", partBuf, gb);
    }
    else if (speed)
        swprintf_s(buf, len, L"RAM %u MHz (%llu GB)", speed, gb);
    else
        swprintf_s(buf, len, L"RAM (%llu GB)", gb);
}

// Query the storage device product name for \\.\\PhysicalDriveN via IOCTL.
// Writes trimmed wide string to buf on success; leaves buf unchanged on failure.
static void GetDiskProductName(int diskNum, wchar_t* buf, int len)
{
    wchar_t path[64];
    swprintf_s(path, L"\\\\.\\PhysicalDrive%d", diskNum);
    HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char raw[512] = {};
    STORAGE_PROPERTY_QUERY q = { StorageDeviceProperty, PropertyStandardQuery };
    DWORD ret = 0;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), raw, sizeof(raw), &ret, nullptr))
    {
        auto* d = (STORAGE_DEVICE_DESCRIPTOR*)raw;
        if (d->ProductIdOffset && d->ProductIdOffset < (DWORD)sizeof(raw))
        {
            int n = MultiByteToWideChar(CP_ACP, 0, raw + d->ProductIdOffset, -1, buf, len);
            for (int i = n - 2; i >= 0 && buf[i] == L' '; --i) buf[i] = L'\0';
        }
    }
    CloseHandle(h);
}

static void GetDiskName(wchar_t* buf, int len)
{
    wcscpy_s(buf, len, L"Disk");
    GetDiskProductName(0, buf, len);
}

// Enumerate physical disk PDH instances, set up per-disk counters and chart names.
// Must be called after PdhOpenQuery and before PdhCollectQueryData.
static void EnumerateDisks()
{
    g_diskCount = 0;

    DWORD counterListSize = 0, instanceListSize = 0;
    PDH_STATUS st = PdhEnumObjectItemsW(NULL, NULL, L"PhysicalDisk",
        NULL, &counterListSize, NULL, &instanceListSize,
        PERF_DETAIL_WIZARD, 0);
    if ((st != PDH_MORE_DATA && st != ERROR_SUCCESS) || instanceListSize == 0) return;

    wchar_t* counterList  = (wchar_t*)malloc(counterListSize  * sizeof(wchar_t));
    wchar_t* instanceList = (wchar_t*)malloc(instanceListSize * sizeof(wchar_t));
    if (!counterList || !instanceList) { free(counterList); free(instanceList); return; }

    st = PdhEnumObjectItemsW(NULL, NULL, L"PhysicalDisk",
        counterList, &counterListSize,
        instanceList, &instanceListSize,
        PERF_DETAIL_WIZARD, 0);

    if (st == ERROR_SUCCESS)
    {
        const wchar_t* p = instanceList;
        while (*p && g_diskCount < MAX_DISKS)
        {
            if (wcscmp(p, L"_Total") != 0)
            {
                int idx = g_diskCount;

                // Default name: "Disk N"
                swprintf_s(g_diskCharts[idx].name, L"Disk %d", idx);

                // Try to get a pretty name from the storage device
                GetDiskProductName(_wtoi(p), g_diskCharts[idx].name, 256);

                // Add PDH counters for this disk instance
                wchar_t ctrPath[256];
                swprintf_s(ctrPath, L"\\PhysicalDisk(%s)\\%% Disk Time", p);
                PdhAddEnglishCounterW(g_pdhQuery, ctrPath, 0, &g_pdhDiskCtrs[idx]);
                swprintf_s(ctrPath, L"\\PhysicalDisk(%s)\\Disk Bytes/sec", p);
                PdhAddEnglishCounterW(g_pdhQuery, ctrPath, 0, &g_pdhDiskByteCtrs[idx]);
                swprintf_s(ctrPath, L"\\PhysicalDisk(%s)\\Disk Read Bytes/sec", p);
                PdhAddEnglishCounterW(g_pdhQuery, ctrPath, 0, &g_pdhDiskReadCtrs[idx]);
                swprintf_s(ctrPath, L"\\PhysicalDisk(%s)\\Disk Write Bytes/sec", p);
                PdhAddEnglishCounterW(g_pdhQuery, ctrPath, 0, &g_pdhDiskWriteCtrs[idx]);

                // Set names for read/write/temp charts
                swprintf_s(g_diskReadCharts[idx].name,  L"%s Read",  g_diskCharts[idx].name);
                swprintf_s(g_diskWriteCharts[idx].name, L"%s Write", g_diskCharts[idx].name);
                swprintf_s(g_diskTempCharts[idx].name,  L"%s Temp",  g_diskCharts[idx].name);

                g_diskCount++;
            }
            p += wcslen(p) + 1;
        }
    }

    free(counterList);
    free(instanceList);
}

// -----------------------------------------------------------------------
// Core topology detection
// -----------------------------------------------------------------------
static void DetectCoreTopology()
{
    SYSTEM_INFO si = {};
    GetSystemInfo(&si);
    g_logicalCoreCount = (int)si.dwNumberOfProcessors;
    if (g_logicalCoreCount > MAX_CORES) g_logicalCoreCount = MAX_CORES;

    for (int i = 0; i < g_logicalCoreCount; i++)
        g_logicalToPhysical[i] = i; // default 1:1

    DWORD sz = 0;
    GetLogicalProcessorInformation(nullptr, &sz);
    if (sz > 0)
    {
        auto* buf = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION*)malloc(sz);
        if (buf)
        {
            if (GetLogicalProcessorInformation(buf, &sz))
            {
                int n = (int)(sz / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
                int physIdx = 0;
                for (int i = 0; i < n; i++)
                {
                    if (buf[i].Relationship == RelationProcessorCore)
                    {
                        ULONG_PTR mask = buf[i].ProcessorMask;
                        for (int bit = 0; bit < MAX_CORES; bit++)
                            if (mask & ((ULONG_PTR)1 << bit))
                                g_logicalToPhysical[bit] = physIdx;
                        physIdx++;
                    }
                }
                g_physicalCoreCount = physIdx ? physIdx : g_logicalCoreCount;
            }
            else
                g_physicalCoreCount = g_logicalCoreCount;
            free(buf);
        }
        else
            g_physicalCoreCount = g_logicalCoreCount;
    }
    else
        g_physicalCoreCount = g_logicalCoreCount;

    for (int i = 0; i < g_logicalCoreCount; i++)
        swprintf_s(g_logicalCharts[i].name, L"L%d", i);
    for (int i = 0; i < g_physicalCoreCount; i++)
        swprintf_s(g_physicalCharts[i].name, L"P%d", i);
}

// Allocates the PDH counter array, calls cb(items, count), then frees.
// Returns false if the counter is null, buffer allocation fails, or PDH fails.
template<typename Fn>
static bool PdhEachArray(PDH_HCOUNTER ctr, DWORD fmt, Fn cb)
{
    if (!ctr) return false;
    DWORD sz = 0, cnt = 0;
    PdhGetFormattedCounterArrayW(ctr, fmt, &sz, &cnt, nullptr);
    if (!sz) return false;
    auto* it = (PDH_FMT_COUNTERVALUE_ITEM_W*)malloc(sz);
    if (!it) return false;
    bool ok = (PdhGetFormattedCounterArrayW(ctr, fmt, &sz, &cnt, it) == ERROR_SUCCESS);
    if (ok) cb(it, cnt);
    free(it);
    return ok;
}

// Push a temperature reading (°C) into a chart; writes absStr.
static void PushTempChart(Chart& c, double tempC, bool gotTemp)
{
    if (gotTemp && tempC > 0.0) {
        PushChartValue(c, max(0.0, min(tempC / 100.0, 1.0)));
        swprintf_s(c.absStr, L"%.0f°C", tempC);
    } else {
        PushChartValue(c, 0.0);
        wcscpy_s(c.absStr, L"N/A");
    }
}

// Sample per-core CPU usage via PDH wildcard counter
static void SampleCores()
{
    if (!g_pdhCoreCtr || g_logicalCoreCount == 0) return;

    double logVals[MAX_CORES] = {};
    if (!PdhEachArray(g_pdhCoreCtr, PDH_FMT_DOUBLE, [&](auto* it, DWORD cnt) {
        for (DWORD j = 0; j < cnt; j++) {
            if (it[j].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA) continue;
            const wchar_t* nm = it[j].szName;
            if (wcscmp(nm, L"_Total") == 0) continue;
            // Instance may be "N" or "G,N" (processor group format)
            const wchar_t* comma = wcschr(nm, L',');
            int coreIdx = comma ? _wtoi(nm) * 64 + _wtoi(comma + 1) : _wtoi(nm);
            if (coreIdx >= 0 && coreIdx < g_logicalCoreCount)
                logVals[coreIdx] = it[j].FmtValue.doubleValue / 100.0;
        }
    })) return;

    for (int i = 0; i < g_logicalCoreCount; i++)
        PushChartValue(g_logicalCharts[i], max(0.0, min(logVals[i], 1.0)));

    double physSum[MAX_CORES] = {};
    int    physCnt[MAX_CORES] = {};
    for (int i = 0; i < g_logicalCoreCount; i++) {
        int p = g_logicalToPhysical[i];
        if (p >= 0 && p < MAX_CORES) { physSum[p] += logVals[i]; physCnt[p]++; }
    }
    for (int p = 0; p < g_physicalCoreCount; p++)
        PushChartValue(g_physicalCharts[p],
            physCnt[p] > 0 ? max(0.0, min(physSum[p] / physCnt[p], 1.0)) : 0.0);
}

// -----------------------------------------------------------------------
// HWiNFO64 shared memory – CPU temperature reader
// Requires HWiNFO64 running with "Shared Memory Support" enabled (free feature).
// -----------------------------------------------------------------------
#define HWINFO_SM_NAME      "Global\\HWiNFO_SENS_SM2"
#define HWINFO_SM_SIG       0x53695748u  // 'HWiS'
#define HWINFO_STR_LEN      128
#define HWINFO_UNIT_LEN     16

#pragma pack(push, 1)
struct HWiNFO_HEADER {
    DWORD    dwSignature;
    DWORD    dwVersion;
    DWORD    dwRevision;
    LONGLONG poll_time;
    DWORD    dwOffsetOfSensorSection;
    DWORD    dwSizeOfSensorElement;
    DWORD    dwNumSensorElements;
    DWORD    dwOffsetOfReadingSection;
    DWORD    dwSizeOfReadingElement;
    DWORD    dwNumReadingElements;
};
struct HWiNFO_READING {
    DWORD  tReading;          // 1 = temperature
    DWORD  dwSensorIndex;
    DWORD  dwReadingID;
    char   szLabelOrig[HWINFO_STR_LEN];
    char   szLabelUser[HWINFO_STR_LEN];
    char   szUnit[HWINFO_UNIT_LEN];
    double Value;
    double ValueMin;
    double ValueMax;
    double ValueAvg;
};
#pragma pack(pop)

// Returns CPU temperature in tenths of °C, or 0 if HWiNFO is not running.
static LONG ReadCpuTempHwInfo()
{
    HANDLE hMap = OpenFileMappingA(FILE_MAP_READ, FALSE, HWINFO_SM_NAME);
    if (!hMap)
    {
        static int s_failCount = 0;
        if (++s_failCount == 1) DbgLog("[hwinfo] CPU temp: shared memory not available\n");
        return 0;
    }

    const void* pView = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (!pView) { CloseHandle(hMap); return 0; }

    LONG result = 0;
    const auto* hdr = static_cast<const HWiNFO_HEADER*>(pView);

    if (hdr->dwSignature == HWINFO_SM_SIG && hdr->dwNumReadingElements > 0)
    {
        const BYTE* base    = static_cast<const BYTE*>(pView);
        const BYTE* rdBase  = base + hdr->dwOffsetOfReadingSection;
        DWORD       stride  = hdr->dwSizeOfReadingElement;
        DWORD       count   = hdr->dwNumReadingElements;

        double bestHi  = 0.0;  // Tctl/Tdie, CPU Package – most specific
        double bestLo  = 0.0;  // any temp label with "CPU" but not a core/die sub-sensor

        for (DWORD i = 0; i < count; ++i)
        {
            const auto* r = reinterpret_cast<const HWiNFO_READING*>(rdBase + i * stride);
            if (r->tReading != 1) continue;                     // temperature only
            if (r->Value <= 0.0 || r->Value > 150.0) continue;

            const char* lbl = r->szLabelOrig[0] ? r->szLabelOrig : r->szLabelUser;

            // Skip non-CPU sensors
            if (strstr(lbl, "GPU"))         continue;
            if (strstr(lbl, "Motherboard")) continue;
            if (strstr(lbl, "PCH"))         continue;
            if (strstr(lbl, "M.2"))         continue;
            if (strstr(lbl, "Drive"))       continue;
            if (strstr(lbl, "SSD"))         continue;
            if (strstr(lbl, "HDD"))         continue;
            if (strstr(lbl, "NVMe"))        continue;

            // Highest priority: AMD Tctl/Tdie or Intel Package
            if (strstr(lbl, "Tctl") || strstr(lbl, "Tdie") ||
                strstr(lbl, "CPU Package") || strcmp(lbl, "CPU") == 0)
            {
                if (r->Value > bestHi) bestHi = r->Value;
            }
            else if (strstr(lbl, "CPU"))
            {
                if (r->Value > bestLo) bestLo = r->Value;
            }
        }

        double chosen = bestHi > 0.0 ? bestHi : bestLo;
        if (chosen > 0.0)
            result = (LONG)(chosen * 10.0 + 0.5);
    }

    UnmapViewOfFile(pView);
    CloseHandle(hMap);
    return result;
}

// Returns max RAM/DIMM temperature in tenths of °C from HWiNFO shared memory, or 0 if unavailable.
static LONG ReadRamTempHwInfo()
{
    HANDLE hMap = OpenFileMappingA(FILE_MAP_READ, FALSE, HWINFO_SM_NAME);
    if (!hMap)
    {
        static int s_failCount = 0;
        if (++s_failCount == 1) DbgLog("[hwinfo] RAM temp: shared memory not available\n");
        return 0;
    }

    const void* pView = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (!pView) { CloseHandle(hMap); return 0; }

    LONG result = 0;
    const auto* hdr = static_cast<const HWiNFO_HEADER*>(pView);

    if (hdr->dwSignature == HWINFO_SM_SIG && hdr->dwNumReadingElements > 0)
    {
        const BYTE* base   = static_cast<const BYTE*>(pView);
        const BYTE* rdBase = base + hdr->dwOffsetOfReadingSection;
        DWORD       stride = hdr->dwSizeOfReadingElement;
        DWORD       count  = hdr->dwNumReadingElements;

        double maxTemp = 0.0;
        for (DWORD i = 0; i < count; ++i)
        {
            const auto* r = reinterpret_cast<const HWiNFO_READING*>(rdBase + i * stride);
            if (r->tReading != 1) continue;
            if (r->Value <= 0.0 || r->Value > 150.0) continue;

            const char* lbl = r->szLabelOrig[0] ? r->szLabelOrig : r->szLabelUser;
            if (strstr(lbl, "GPU")) continue;  // e.g. "GPU Memory Junction Temperature"

            if (strstr(lbl, "DIMM") || strstr(lbl, "Memory") ||
                strstr(lbl, "RAM")  || strstr(lbl, "SPD"))
            {
                if (r->Value > maxTemp) maxTemp = r->Value;
            }
        }
        if (maxTemp > 0.0)
            result = (LONG)(maxTemp * 10.0 + 0.5);
    }

    UnmapViewOfFile(pView);
    CloseHandle(hMap);
    return result;
}

// Returns GPU hot-spot temperature in tenths of °C from HWiNFO shared memory, or 0 if
// unavailable (requires HWiNFO64 with Shared Memory Support; NVML has no hot-spot sensor).
static LONG ReadGpuHotspotHwInfo()
{
    HANDLE hMap = OpenFileMappingA(FILE_MAP_READ, FALSE, HWINFO_SM_NAME);
    if (!hMap)
    {
        static int s_failCount = 0;
        if (++s_failCount == 1) DbgLog("[hwinfo] GPU hot spot: shared memory not available\n");
        return 0;
    }

    const void* pView = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (!pView) { CloseHandle(hMap); return 0; }

    LONG result = 0;
    const auto* hdr = static_cast<const HWiNFO_HEADER*>(pView);

    if (hdr->dwSignature == HWINFO_SM_SIG && hdr->dwNumReadingElements > 0)
    {
        const BYTE* base   = static_cast<const BYTE*>(pView);
        const BYTE* rdBase = base + hdr->dwOffsetOfReadingSection;
        DWORD       stride = hdr->dwSizeOfReadingElement;
        DWORD       count  = hdr->dwNumReadingElements;

        double maxTemp = 0.0;
        for (DWORD i = 0; i < count; ++i)
        {
            const auto* r = reinterpret_cast<const HWiNFO_READING*>(rdBase + i * stride);
            if (r->tReading != 1) continue;
            if (r->Value <= 0.0 || r->Value > 150.0) continue;

            const char* lbl = r->szLabelOrig[0] ? r->szLabelOrig : r->szLabelUser;
            if (strstr(lbl, "GPU") && strstr(lbl, "Hot Spot"))
                if (r->Value > maxTemp) maxTemp = r->Value;
        }
        if (maxTemp > 0.0)
            result = (LONG)(maxTemp * 10.0 + 0.5);
    }

    UnmapViewOfFile(pView);
    CloseHandle(hMap);
    return result;
}

// Returns temperature of physical disk `diskIdx` (0-based) in tenths of °C from HWiNFO shared
// memory, grouping readings by sensor. Returns 0 if HWiNFO is not running or no data.
static LONG ReadDiskTempHwInfo(int diskIdx)
{
    HANDLE hMap = OpenFileMappingA(FILE_MAP_READ, FALSE, HWINFO_SM_NAME);
    if (!hMap)
    {
        static int s_failCount = 0;
        if (++s_failCount == 1) DbgLog("[hwinfo] disk temp: shared memory not available\n");
        return 0;
    }
    const void* pView = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (!pView) { CloseHandle(hMap); return 0; }

    LONG result = 0;
    const auto* hdr = static_cast<const HWiNFO_HEADER*>(pView);
    if (hdr->dwSignature == HWINFO_SM_SIG && hdr->dwNumReadingElements > 0)
    {
        const BYTE* base   = static_cast<const BYTE*>(pView);
        const BYTE* rdBase = base + hdr->dwOffsetOfReadingSection;
        DWORD       stride = hdr->dwSizeOfReadingElement;
        DWORD       count  = hdr->dwNumReadingElements;

        // Collect per-sensor max temperatures for disk sensors.
        // Sensors are identified by dwSensorIndex and ordered by first appearance.
        DWORD  seenSensors[MAX_DISKS]   = {};
        double sensorMaxTemp[MAX_DISKS] = {};
        int    sensorCount = 0;

        for (DWORD i = 0; i < count; ++i)
        {
            const auto* r = reinterpret_cast<const HWiNFO_READING*>(rdBase + i * stride);
            if (r->tReading != 1) continue;
            if (r->Value <= 0.0 || r->Value > 150.0) continue;

            const char* lbl = r->szLabelOrig[0] ? r->szLabelOrig : r->szLabelUser;
            // Note: deliberately no bare "Temperature" match here — it's too broad and
            // pulls in unrelated sensors (e.g. "SPD Hub Temperature" for RAM), which would
            // steal a disk slot since they can appear before the real drive sensor in the
            // shared memory reading order.
            bool isDisk = strstr(lbl, "Drive")  || strstr(lbl, "SSD") ||
                          strstr(lbl, "HDD")    || strstr(lbl, "NVMe") ||
                          strstr(lbl, "M.2");
            if (!isDisk) continue;
            if (strstr(lbl, "CPU") || strstr(lbl, "GPU") ||
                strstr(lbl, "DIMM") || strstr(lbl, "RAM")) continue;

            int sidx = -1;
            for (int s = 0; s < sensorCount; s++)
                if (seenSensors[s] == r->dwSensorIndex) { sidx = s; break; }
            if (sidx < 0 && sensorCount < MAX_DISKS)
            {
                sidx = sensorCount;
                seenSensors[sensorCount++] = r->dwSensorIndex;
            }
            if (sidx >= 0 && r->Value > sensorMaxTemp[sidx])
                sensorMaxTemp[sidx] = r->Value;
        }

        if (diskIdx < sensorCount && sensorMaxTemp[diskIdx] > 0.0)
            result = (LONG)(sensorMaxTemp[diskIdx] * 10.0 + 0.5);
    }

    UnmapViewOfFile(pView);
    CloseHandle(hMap);
    return result;
}

// -----------------------------------------------------------------------
// WMI CPU temperature background thread.
// Priority order each tick:
//   1. HWiNFO64 shared memory   – best AMD support, instant read
//   2. OpenHardwareMonitor WMI  – fallback if OHM is running
//   3. LibreHardwareMonitor WMI – fallback if LHM is running
//   4. MSAcpi_ThermalZoneTemperature – Intel / some AMD laptops
// -----------------------------------------------------------------------

// Connect to a WMI namespace; returns nullptr on failure (caller must Release).
static IWbemServices* WmiConnect(IWbemLocator* pLoc, const wchar_t* ns)
{
    IWbemServices* pSvc = nullptr;
    BSTR bns = SysAllocString(ns);
    HRESULT hr = pLoc->ConnectServer(bns, nullptr, nullptr, nullptr,
                                     0, nullptr, nullptr, &pSvc);
    SysFreeString(bns);
    if (FAILED(hr)) return nullptr;
    (void)CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                      RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                      nullptr, EOAC_NONE);
    return pSvc;
}

// Query an OHM/LHM Sensor table for CPU temperature; returns tenths of °C or 0.
static LONG WmiQueryHwMon(IWbemServices* pSvc)
{
    if (!pSvc) return 0;
    BSTR wql   = SysAllocString(L"WQL");
    BSTR query = SysAllocString(
        L"SELECT Value FROM Sensor WHERE SensorType='Temperature' AND "
        L"(Name='CPU Package' OR Name='Core (Tdie)' OR Name='Core Average' OR "
        L"Name='CPU' OR Name='Core Max')");

    LONG result = 0;
    IEnumWbemClassObject* pEnum = nullptr;
    if (SUCCEEDED(pSvc->ExecQuery(wql, query,
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
            nullptr, &pEnum)))
    {
        IWbemClassObject* pObj = nullptr; ULONG ret = 0;
        float best = 0.0f;
        while (pEnum->Next(WBEM_INFINITE, 1, &pObj, &ret) == S_OK)
        {
            VARIANT vt; VariantInit(&vt);
            if (SUCCEEDED(pObj->Get(L"Value", 0, &vt, nullptr, nullptr)))
            {
                float v = (vt.vt == VT_R4) ? vt.fltVal :
                          (vt.vt == VT_R8) ? (float)vt.dblVal : 0.0f;
                if (v > best) best = v;
            }
            VariantClear(&vt); pObj->Release();
        }
        pEnum->Release();
        if (best > 0.0f) result = (LONG)(best * 10.0f + 0.5f);
    }
    SysFreeString(query); SysFreeString(wql);
    return result;
}

// Query MSAcpi_ThermalZoneTemperature; returns tenths of °C or 0.
static LONG WmiQueryAcpi(IWbemServices* pSvc)
{
    if (!pSvc) return 0;
    BSTR wql   = SysAllocString(L"WQL");
    BSTR query = SysAllocString(L"SELECT * FROM MSAcpi_ThermalZoneTemperature");

    LONG result = 0;
    IEnumWbemClassObject* pEnum = nullptr;
    if (SUCCEEDED(pSvc->ExecQuery(wql, query,
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
            nullptr, &pEnum)))
    {
        IWbemClassObject* pObj = nullptr; ULONG ret = 0;
        LONG maxDeciK = 0;
        while (pEnum->Next(WBEM_INFINITE, 1, &pObj, &ret) == S_OK)
        {
            VARIANT vt; VariantInit(&vt);
            if (SUCCEEDED(pObj->Get(L"CurrentTemperature", 0, &vt, nullptr, nullptr)))
            {
                LONG val = (vt.vt == VT_I4)  ? vt.lVal :
                           (vt.vt == VT_UI4) ? (LONG)vt.ulVal : 0;
                if (val > maxDeciK) maxDeciK = val;
            }
            VariantClear(&vt); pObj->Release();
        }
        pEnum->Release();
        if (maxDeciK > 2731) result = maxDeciK - 2731; // tenths K → tenths °C
    }
    SysFreeString(query); SysFreeString(wql);
    return result;
}

static DWORD WINAPI CpuTempThreadProc(LPVOID)
{
    (void)CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    (void)CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
        RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE,
        nullptr, EOAC_NONE, nullptr);

    IWbemLocator* pLoc = nullptr;
    (void)CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                     IID_IWbemLocator, (void**)&pLoc);

    // Pre-connect WMI namespaces (best-effort; nullptr if unavailable)
    IWbemServices* pOhm  = pLoc ? WmiConnect(pLoc, L"root\\OpenHardwareMonitor")  : nullptr;
    IWbemServices* pLhm  = pLoc ? WmiConnect(pLoc, L"root\\LibreHardwareMonitor") : nullptr;
    IWbemServices* pAcpi = pLoc ? WmiConnect(pLoc, L"root\\wmi")                  : nullptr;

    while (!g_cpuTempThreadStop)
    {
        LONG val = 0;

        // 1. HWiNFO64 shared memory (best for AMD Ryzen, no driver required)
        if (!val) val = ReadCpuTempHwInfo();
        // 2. OpenHardwareMonitor WMI
        if (!val) val = WmiQueryHwMon(pOhm);
        // 3. LibreHardwareMonitor WMI
        if (!val) val = WmiQueryHwMon(pLhm);
        // 4. ACPI thermal zone (Intel / some AMD laptops)
        if (!val) val = WmiQueryAcpi(pAcpi);

        if (val > 0)
            InterlockedExchange(&g_cpuWmiTempDeciC, val);

        for (int i = 0; i < 40 && !g_cpuTempThreadStop; ++i)
            Sleep(50);
    }

    if (pOhm)  pOhm->Release();
    if (pLhm)  pLhm->Release();
    if (pAcpi) pAcpi->Release();
    if (pLoc)  pLoc->Release();
    CoUninitialize();
    return 0;
}

static void StartCpuTempThread()
{
    g_cpuTempThreadStop = false;
    if (g_cpuTempThread) { CloseHandle(g_cpuTempThread); g_cpuTempThread = NULL; }
    g_cpuTempThread = CreateThread(nullptr, 0, CpuTempThreadProc, nullptr, 0, nullptr);
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

    // CPU temperature – prefer WMI thread result (OHM/LHM/ACPI), fall back to PDH Thermal Zone
    {
        double tempC  = 0.0;
        bool gotTemp  = false;

        // Primary: WMI background thread (OHM → LHM → MSAcpi); value is tenths of °C
        LONG deciC = InterlockedCompareExchange(&g_cpuWmiTempDeciC, 0, 0);
        if (deciC > 0)
        {
            tempC   = deciC / 10.0;
            gotTemp = true;
        }

        // Fallback: PDH Thermal Zone counter (works on some Intel platforms)
        if (!gotTemp)
            PdhEachArray(g_pdhCpuTempCtr, PDH_FMT_DOUBLE, [&](auto* it, DWORD cnt) {
                double maxT = 0.0;
                for (DWORD i = 0; i < cnt; ++i)
                    if (it[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA) {
                        double t = it[i].FmtValue.doubleValue / 10.0 - 273.15;
                        if (t > maxT) { maxT = t; gotTemp = true; }
                    }
                tempC = maxT;
            });

        PushTempChart(g_cpuTempChart, tempC, gotTemp);
    }

    // GPU – PDH wildcard over all 3D engine instances
    {
        // Core utilization
        double v = 0.0;
        PdhEachArray(g_pdhGpuCtr, PDH_FMT_DOUBLE, [&](auto* it, DWORD cnt) {
            for (DWORD i = 0; i < cnt; ++i)
                if (it[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA)
                    v += it[i].FmtValue.doubleValue;
        });
        PushChartValue(g_charts[1], min(v / 100.0, 1.0));

        // VRAM usage
        if (g_pdhGpuMemCtr && g_gpuTotalVram > 0)
        {
            ULONGLONG totalUsed = 0;
            PdhEachArray(g_pdhGpuMemCtr, PDH_FMT_LARGE, [&](auto* it, DWORD cnt) {
                for (DWORD i = 0; i < cnt; ++i)
                    if (it[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA)
                        totalUsed += (ULONGLONG)it[i].FmtValue.largeValue;
            });
            const double gb = 1.0 / (1024.0 * 1024.0 * 1024.0);
            double usedGB  = (double)totalUsed      * gb;
            double totalGB = (double)g_gpuTotalVram * gb;
            swprintf_s(g_charts[1].absStr,    L"%.1f / %.0f GB", usedGB, totalGB);
            PushChartValue(g_gpuVramChart, max(0.0, min((double)totalUsed / (double)g_gpuTotalVram, 1.0)));
            swprintf_s(g_gpuVramChart.absStr, L"%.1f / %.0f GB", usedGB, totalGB);
        }
        else
        {
            PushChartValue(g_gpuVramChart, 0.0);
        }

        // GPU temperature: prefer NVML (reliable on RTX 5080/Blackwell),
        // fall back to PDH on non-NVIDIA or older drivers.
        {
            double tempC  = 0.0;
            bool   gotTemp = false;

            // --- NVML path ---
            if (g_nvmlDevice && g_nvmlGetTemp)
            {
                unsigned int t = 0;
                if (g_nvmlGetTemp(g_nvmlDevice, NVML_TEMPERATURE_GPU, &t) == NVML_SUCCESS)
                {
                    tempC   = (double)t;
                    gotTemp = true;
                }
            }

            // --- PDH fallback (AMD / Intel / older NVIDIA drivers) ---
            if (!gotTemp)
                PdhEachArray(g_pdhGpuTempCtr, PDH_FMT_DOUBLE, [&](auto* it, DWORD cnt) {
                    for (DWORD i = 0; i < cnt; ++i)
                        if (it[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA)
                            { tempC = it[i].FmtValue.doubleValue; gotTemp = true; break; }
                });
            PushTempChart(g_gpuTempChart, tempC, gotTemp);

            // GPU hot-spot temperature: HWiNFO64 shared memory only (NVML has no such sensor).
            LONG hotspotDeciC = ReadGpuHotspotHwInfo();
            PushTempChart(g_gpuHotspotChart, hotspotDeciC / 10.0, hotspotDeciC > 0);
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

        // RAM temperature via HWiNFO64 shared memory (requires DIMM sensors)
        LONG ramTempDeciC = ReadRamTempHwInfo();
        PushTempChart(g_ramTempChart, ramTempDeciC / 10.0, ramTempDeciC > 0);
    }

    // Disk – PDH % Disk Time + read/write bytes (_Total + per-disk)
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

        // _Total read/write sampling
        {
            double readMb = 0.0, writeMb = 0.0;
            if (g_pdhDiskReadCtr)
            {
                PDH_FMT_COUNTERVALUE val;
                if (PdhGetFormattedCounterValue(g_pdhDiskReadCtr, PDH_FMT_DOUBLE,
                        nullptr, &val) == ERROR_SUCCESS &&
                    val.CStatus == PDH_CSTATUS_VALID_DATA)
                    readMb = val.doubleValue / (1024.0 * 1024.0);
            }
            if (g_pdhDiskWriteCtr)
            {
                PDH_FMT_COUNTERVALUE val;
                if (PdhGetFormattedCounterValue(g_pdhDiskWriteCtr, PDH_FMT_DOUBLE,
                        nullptr, &val) == ERROR_SUCCESS &&
                    val.CStatus == PDH_CSTATUS_VALID_DATA)
                    writeMb = val.doubleValue / (1024.0 * 1024.0);
            }
            double combined = readMb + writeMb;
            if (combined > g_diskTotalIoPeak)
                g_diskTotalIoPeak = combined * 1.1 + 0.1;
            else if (g_diskTotalIoPeak > 0.1)
                g_diskTotalIoPeak = max(combined, g_diskTotalIoPeak * 0.9998);
            double peak = max(g_diskTotalIoPeak, 0.1);
            PushChartValue(g_diskTotalReadChart,  max(0.0, min(readMb  / peak, 1.0)));
            PushChartValue(g_diskTotalWriteChart, max(0.0, min(writeMb / peak, 1.0)));
            swprintf_s(g_diskTotalReadChart.absStr,  L"%.1f MB/s", readMb);
            swprintf_s(g_diskTotalWriteChart.absStr, L"%.1f MB/s", writeMb);
        }

        // Per-disk sampling
        for (int di = 0; di < g_diskCount; di++)
        {
            double dv = 0.0;
            if (g_pdhDiskCtrs[di])
            {
                PDH_FMT_COUNTERVALUE val;
                if (PdhGetFormattedCounterValue(g_pdhDiskCtrs[di], PDH_FMT_DOUBLE,
                        nullptr, &val) == ERROR_SUCCESS &&
                    val.CStatus == PDH_CSTATUS_VALID_DATA)
                    dv = val.doubleValue / 100.0;
            }
            PushChartValue(g_diskCharts[di], max(0.0, min(dv, 1.0)));

            if (g_pdhDiskByteCtrs[di])
            {
                PDH_FMT_COUNTERVALUE val;
                if (PdhGetFormattedCounterValue(g_pdhDiskByteCtrs[di], PDH_FMT_DOUBLE,
                        nullptr, &val) == ERROR_SUCCESS &&
                    val.CStatus == PDH_CSTATUS_VALID_DATA)
                    swprintf_s(g_diskCharts[di].absStr, L"%.1f MB/s",
                        val.doubleValue / (1024.0 * 1024.0));
            }

            // Per-disk read/write sampling (auto-scaled, shared peak for comparison)
            {
                double readMb = 0.0, writeMb = 0.0;
                if (g_pdhDiskReadCtrs[di])
                {
                    PDH_FMT_COUNTERVALUE val;
                    if (PdhGetFormattedCounterValue(g_pdhDiskReadCtrs[di], PDH_FMT_DOUBLE,
                            nullptr, &val) == ERROR_SUCCESS &&
                        val.CStatus == PDH_CSTATUS_VALID_DATA)
                        readMb = val.doubleValue / (1024.0 * 1024.0);
                }
                if (g_pdhDiskWriteCtrs[di])
                {
                    PDH_FMT_COUNTERVALUE val;
                    if (PdhGetFormattedCounterValue(g_pdhDiskWriteCtrs[di], PDH_FMT_DOUBLE,
                            nullptr, &val) == ERROR_SUCCESS &&
                        val.CStatus == PDH_CSTATUS_VALID_DATA)
                        writeMb = val.doubleValue / (1024.0 * 1024.0);
                }
                double combined = readMb + writeMb;
                if (combined > g_diskIoPeak[di])
                    g_diskIoPeak[di] = combined * 1.1 + 0.1;
                else if (g_diskIoPeak[di] > 0.1)
                    g_diskIoPeak[di] = max(combined, g_diskIoPeak[di] * 0.9998);
                double peak = max(g_diskIoPeak[di], 0.1);
                PushChartValue(g_diskReadCharts[di],  max(0.0, min(readMb  / peak, 1.0)));
                PushChartValue(g_diskWriteCharts[di], max(0.0, min(writeMb / peak, 1.0)));
                swprintf_s(g_diskReadCharts[di].absStr,  L"%.1f MB/s", readMb);
                swprintf_s(g_diskWriteCharts[di].absStr, L"%.1f MB/s", writeMb);
            }

            // Per-disk temperature from HWiNFO
            LONG tempDeciC = ReadDiskTempHwInfo(di);
            PushTempChart(g_diskTempCharts[di], tempDeciC / 10.0, tempDeciC > 0);
        }
    }

    SampleCores();
}

// -----------------------------------------------------------------------
// Per-process sampling helpers
// -----------------------------------------------------------------------

static void StripExeSuffix(wchar_t* s)
{
    int n = (int)wcslen(s);
    if (n >= 4 && _wcsicmp(s + n - 4, L".exe") == 0) s[n - 4] = L'\0';
}

// Strip "#N" PDH instance suffix and ".exe" extension for clean display name.
static void NormalizeProcessName(const wchar_t* src, wchar_t* dst, int dstLen)
{
    wcsncpy_s(dst, dstLen, src, _TRUNCATE);
    wchar_t* hash = wcsrchr(dst, L'#');
    if (hash)
    {
        bool allDigits = true;
        for (wchar_t* p = hash + 1; *p; ++p)
            if (!iswdigit(*p)) { allDigits = false; break; }
        if (allDigits) *hash = L'\0';
    }
    StripExeSuffix(dst);
}

// Get process exe name (without extension) from PID via kernel query.
static void PidToName(DWORD pid, wchar_t* dst, int dstLen)
{
    dst[0] = L'\0';
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return;
    wchar_t buf[MAX_PATH] = {};
    DWORD sz = MAX_PATH;
    if (QueryFullProcessImageNameW(h, 0, buf, &sz) && sz > 0)
    {
        wchar_t* last = wcsrchr(buf, L'\\');
        wcsncpy_s(dst, dstLen, last ? last + 1 : buf, _TRUNCATE);
        StripExeSuffix(dst);
    }
    CloseHandle(h);
}

struct TempProc {
    wchar_t name[64];
    double  pct;
    double  absVal;
};

static int CompareTempProcDesc(const void* a, const void* b)
{
    double pa = ((const TempProc*)a)->pct;
    double pb = ((const TempProc*)b)->pct;
    return (pb > pa) ? 1 : (pb < pa) ? -1 : 0;
}

// Find or insert by name in temps array; returns index, or -1 if full.
static int TempFindOrAdd(TempProc* temps, int& count, int maxCount, const wchar_t* name)
{
    for (int j = 0; j < count; ++j)
        if (wcscmp(temps[j].name, name) == 0) return j;
    if (count >= maxCount) return -1;
    wcscpy_s(temps[count].name, name);
    temps[count].pct    = 0.0;
    temps[count].absVal = 0.0;
    return count++;
}

static void SampleProcesses()
{
    static TempProc temps[1024];

    // ---- CPU processes (\\Process(*)\\% Processor Time) ----
    {
        int nT = 0;
        PdhEachArray(g_pdhProcCpuCtr, PDH_FMT_DOUBLE, [&](auto* it, DWORD cnt) {
            for (DWORD i = 0; i < cnt; ++i) {
                if (it[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA) continue;
                double v = it[i].FmtValue.doubleValue;
                if (v <= 0.0) continue;
                const wchar_t* nm = it[i].szName;
                if (wcscmp(nm, L"_Total") == 0 || wcscmp(nm, L"Idle") == 0) continue;
                wchar_t clean[64];
                NormalizeProcessName(nm, clean, 64);
                int idx = TempFindOrAdd(temps, nT, 1024, clean);
                if (idx >= 0) { temps[idx].pct += v; temps[idx].absVal += v; }
            }
        });
        double norm = (g_logicalCoreCount > 0) ? (double)g_logicalCoreCount : 1.0;
        for (int j = 0; j < nT; ++j) { temps[j].pct /= norm; temps[j].absVal /= norm; }
        qsort(temps, nT, sizeof(TempProc), CompareTempProcDesc);
        g_procCpuCount = min(nT, MAX_PROC_SHOW);
        for (int j = 0; j < g_procCpuCount; ++j) {
            wcscpy_s(g_procCpu[j].name, temps[j].name);
            g_procCpu[j].pct = (float)min(temps[j].pct, 100.0 * norm);
            swprintf_s(g_procCpu[j].absStr, L"%d ms/s", (int)(temps[j].pct / 100.0 * 1000.0 + 0.5));
        }
    }

    // ---- GPU processes (parse PIDs from GPU engine instance names) ----
    {
        int nT = 0;
        PdhEachArray(g_pdhGpuCtr, PDH_FMT_DOUBLE, [&](auto* it, DWORD cnt) {
            for (DWORD i = 0; i < cnt; ++i) {
                if (it[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA) continue;
                double v = it[i].FmtValue.doubleValue;
                if (v <= 0.0) continue;
                const wchar_t* nm = it[i].szName;
                DWORD pid = (wcsncmp(nm, L"pid_", 4) == 0) ? (DWORD)_wtoi(nm + 4) : 0;
                if (pid == 0) continue;
                wchar_t procName[64] = {};
                PidToName(pid, procName, 64);
                if (!procName[0]) swprintf_s(procName, L"PID %u", pid);
                int idx = TempFindOrAdd(temps, nT, 1024, procName);
                if (idx >= 0) temps[idx].pct += v;
            }
        });
        qsort(temps, nT, sizeof(TempProc), CompareTempProcDesc);
        g_procGpuCount = min(nT, MAX_PROC_SHOW);
        for (int j = 0; j < g_procGpuCount; ++j) {
            wcscpy_s(g_procGpu[j].name, temps[j].name);
            double pct = min(temps[j].pct, 100.0);
            g_procGpu[j].pct = (float)pct;
            swprintf_s(g_procGpu[j].absStr, L"%.1f%%", pct);
        }
    }

    // ---- RAM processes (\\Process(*)\\Working Set) ----
    {
        MEMORYSTATUSEX ms = { sizeof(ms) };
        GlobalMemoryStatusEx(&ms);
        double totalBytes = (ms.ullTotalPhys > 0) ? (double)ms.ullTotalPhys : 1.0;
        int nT = 0;
        PdhEachArray(g_pdhProcRamCtr, PDH_FMT_LARGE, [&](auto* it, DWORD cnt) {
            for (DWORD i = 0; i < cnt; ++i) {
                if (it[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA) continue;
                double bytes = (double)it[i].FmtValue.largeValue;
                if (bytes <= 0.0) continue;
                const wchar_t* nm = it[i].szName;
                if (wcscmp(nm, L"_Total") == 0 || wcscmp(nm, L"Idle") == 0) continue;
                wchar_t clean[64];
                NormalizeProcessName(nm, clean, 64);
                int idx = TempFindOrAdd(temps, nT, 1024, clean);
                if (idx >= 0) { temps[idx].pct += bytes / totalBytes * 100.0; temps[idx].absVal += bytes; }
            }
        });
        qsort(temps, nT, sizeof(TempProc), CompareTempProcDesc);
        g_procRamCount = min(nT, MAX_PROC_SHOW);
        for (int j = 0; j < g_procRamCount; ++j) {
            wcscpy_s(g_procRam[j].name, temps[j].name);
            g_procRam[j].pct = (float)min(temps[j].pct, 100.0);
            double mb = temps[j].absVal / (1024.0 * 1024.0);
            swprintf_s(g_procRam[j].absStr, mb >= 1024.0 ? L"%.1f GB" : L"%.0f MB",
                       mb >= 1024.0 ? mb / 1024.0 : mb);
        }
    }

    // ---- Disk processes (\\Process(*)\\IO Data Bytes/sec) ----
    {
        double diskPct = 0.0;
        if (g_pdhDiskCtr) {
            PDH_FMT_COUNTERVALUE val;
            if (PdhGetFormattedCounterValue(g_pdhDiskCtr, PDH_FMT_DOUBLE, nullptr, &val) == ERROR_SUCCESS &&
                val.CStatus == PDH_CSTATUS_VALID_DATA)
                diskPct = max(0.0, min(val.doubleValue, 100.0));
        }
        int nT = 0;
        PdhEachArray(g_pdhProcDiskCtr, PDH_FMT_DOUBLE, [&](auto* it, DWORD cnt) {
            double totalBps = 1.0;
            for (DWORD i = 0; i < cnt; ++i)
                if (it[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA &&
                    wcscmp(it[i].szName, L"_Total") == 0)
                    { totalBps = it[i].FmtValue.doubleValue; if (totalBps <= 0.0) totalBps = 1.0; break; }
            for (DWORD i = 0; i < cnt; ++i) {
                if (it[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA) continue;
                double bps = it[i].FmtValue.doubleValue;
                if (bps <= 0.0) continue;
                const wchar_t* nm = it[i].szName;
                if (wcscmp(nm, L"_Total") == 0 || wcscmp(nm, L"Idle") == 0) continue;
                wchar_t clean[64];
                NormalizeProcessName(nm, clean, 64);
                int idx = TempFindOrAdd(temps, nT, 1024, clean);
                if (idx >= 0) { temps[idx].pct += bps / totalBps * diskPct; temps[idx].absVal += bps; }
            }
        });
        qsort(temps, nT, sizeof(TempProc), CompareTempProcDesc);
        g_procDiskCount = min(nT, MAX_PROC_SHOW);
        for (int j = 0; j < g_procDiskCount; ++j) {
            wcscpy_s(g_procDisk[j].name, temps[j].name);
            g_procDisk[j].pct = (float)min(temps[j].pct, 100.0);
            swprintf_s(g_procDisk[j].absStr, L"%.1f MB/s", temps[j].absVal / (1024.0 * 1024.0));
        }
    }
}

// -----------------------------------------------------------------------
// Panel drawing helpers
// -----------------------------------------------------------------------
static void DrawChartPanel(ID2D1RenderTarget* rt, int dataIdx, D2D1_RECT_F area)
{
    const float x      = area.left;
    const float y      = area.top;
    const float colW   = area.right - area.left;
    const float rowH   = area.bottom - area.top;
    const float titleH = g_fontSize * 1.4f;
    const float margin = 8.f;

    // Store rect for click detection
    switch (dataIdx) {
        case 0: g_cpuChartRect  = area; break;
        case 1: g_gpuChartRect  = area; break;
        case 2: g_ramChartRect  = area; break;
        case 3: g_diskChartRect = area; break;
    }

    auto drawTitle = [&](const wchar_t* name) {
        ID2D1SolidColorBrush* pB = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(D2D1::ColorF(0.f,0.f,0.f,1.f), &pB))) {
            DrawTextEllipsis(rt, name, (UINT32)wcslen(name), g_pChartFmtL,
                             {x+margin, y, x+colW-margin, y+titleH}, pB);
            pB->Release();
        }
    };

    if (dataIdx == 0) // CPU
    {
        if (g_cpuMode == 0) {
            DrawChart(rt, g_charts[0], area, g_pChartFmtL, g_pChartFmtR);
        } else if (g_cpuMode == 3) {
            drawTitle(g_charts[0].name);
            float halfW = colW/2.f, cY = y+titleH;
            int ms = max(2, CHART_SAMPLES/2);
            DrawChart(rt, g_charts[0],    {x,       cY, x+halfW, y+rowH}, nullptr, g_pChartFmtR, ms);
            DrawChart(rt, g_cpuTempChart, {x+halfW, cY, x+colW,  y+rowH}, nullptr, g_pChartFmtR, ms, true);
        } else {
            drawTitle(g_charts[0].name);
            const Chart* cores = (g_cpuMode==1) ? g_logicalCharts  : g_physicalCharts;
            int nc = (g_cpuMode==1) ? g_logicalCoreCount : g_physicalCoreCount;
            if (nc < 1) nc = 1;
            int ms = max(2, CHART_SAMPLES/nc);
            float mW = colW/(float)nc, cY = y+titleH;
            for (int c = 0; c < nc; c++)
                DrawChart(rt, cores[c], {x+c*mW, cY, x+(c+1)*mW, y+rowH}, g_pChartFmtL, g_pChartFmtR, ms);
        }
    }
    else if (dataIdx == 1) // GPU
    {
        if (g_gpuMode == 0)
            DrawChart(rt, g_charts[1], area, g_pChartFmtL, g_pChartFmtR);
        else if (g_gpuMode == 1)
            DrawChart(rt, g_gpuVramChart, area, g_pChartFmtL, g_pChartFmtR);
        else if (g_gpuMode == 2)
            DrawGpuTempChart(rt, g_gpuHotspotChart, g_gpuTempChart, area, g_pChartFmtL, g_pChartFmtR);
        else if (g_gpuMode == 3) {
            drawTitle(g_charts[1].name);
            float halfW = colW/2.f, cY = y+titleH; int ms = max(2, CHART_SAMPLES/2);
            DrawChart(rt, g_charts[1],    {x,       cY, x+halfW, y+rowH}, nullptr, g_pChartFmtR, ms);
            DrawChart(rt, g_gpuVramChart, {x+halfW, cY, x+colW,  y+rowH}, nullptr, g_pChartFmtR, ms);
        } else { // gpuMode == 4
            drawTitle(g_charts[1].name);
            float tW = colW/3.f, cY = y+titleH; int ms = max(2, CHART_SAMPLES/3);
            Chart core = g_charts[1]; core.displayAbsStr[0] = L'\0';
            DrawChart(rt, core,           {x,        cY, x+tW,      y+rowH}, nullptr, g_pChartFmtR, ms);
            DrawChart(rt, g_gpuVramChart, {x+tW,     cY, x+2.f*tW,  y+rowH}, nullptr, g_pChartFmtR, ms);
            DrawGpuTempChart(rt, g_gpuHotspotChart, g_gpuTempChart, {x+2.f*tW, cY, x+colW, y+rowH}, nullptr, g_pChartFmtR, ms, true);
        }
    }
    else if (dataIdx == 2) // RAM
    {
        if (g_ramMode == 0)
            DrawChart(rt, g_charts[2], area, g_pChartFmtL, g_pChartFmtR);
        else {
            drawTitle(g_charts[2].name);
            float halfW = colW/2.f, cY = y+titleH; int ms = max(2, CHART_SAMPLES/2);
            DrawChart(rt, g_charts[2],    {x,       cY, x+halfW, y+rowH}, nullptr, g_pChartFmtR, ms);
            DrawChart(rt, g_ramTempChart, {x+halfW, cY, x+colW,  y+rowH}, nullptr, g_pChartFmtR, ms, true);
        }
    }
    else if (dataIdx == 3) // Disk
    {
        int di = g_diskMode - 1;
        bool validDisk = (g_diskMode > 0) && (di < g_diskCount);
        const Chart& base  = validDisk ? g_diskCharts[di]     : g_charts[3];
        const Chart& rdC   = validDisk ? g_diskReadCharts[di]  : g_diskTotalReadChart;
        const Chart& wrC   = validDisk ? g_diskWriteCharts[di] : g_diskTotalWriteChart;
        const Chart* tmpC  = validDisk ? &g_diskTempCharts[di] : nullptr;
        if (g_diskSubMode == 1) {
            drawTitle(base.name);
            float halfW = colW/2.f, cY = y+titleH; int ms = max(2, CHART_SAMPLES/2);
            DrawChart(rt, rdC, {x,       cY, x+halfW, y+rowH}, nullptr, g_pChartFmtR, ms, true);
            DrawChart(rt, wrC, {x+halfW, cY, x+colW,  y+rowH}, nullptr, g_pChartFmtR, ms, true);
        } else if (g_diskSubMode == 2 && tmpC) {
            drawTitle(base.name);
            float tW = colW/3.f, cY = y+titleH; int ms = max(2, CHART_SAMPLES/3);
            DrawChart(rt, rdC,   {x,        cY, x+tW,     y+rowH}, nullptr, g_pChartFmtR, ms, true);
            DrawChart(rt, wrC,   {x+tW,     cY, x+2.f*tW, y+rowH}, nullptr, g_pChartFmtR, ms, true);
            DrawChart(rt, *tmpC, {x+2.f*tW, cY, x+colW,   y+rowH}, nullptr, g_pChartFmtR, ms, true);
        } else if (g_diskMode > 0)
            DrawChart(rt, base, area, g_pChartFmtL, g_pChartFmtR);
        else
            DrawChart(rt, g_charts[3], area, g_pChartFmtL, g_pChartFmtR);
    }
}

static void DrawProcPanel(ID2D1RenderTarget* rt, int dataIdx, D2D1_RECT_F area)
{
    if (dataIdx < 0 || dataIdx > 3) return;
    static const wchar_t* titles[4] = { L"CPU", L"GPU", L"RAM", L"Disk" };
    const ProcEntry* data[4] = { g_dispProcCpu, g_dispProcGpu, g_dispProcRam, g_dispProcDisk };
    int cnts[4] = { g_dispProcCpuCount, g_dispProcGpuCount, g_dispProcRamCount, g_dispProcDiskCount };

    g_procListRects[dataIdx] = area;

    const float lineH = g_fontSize * 1.4f;
    int maxE = (int)((area.bottom - area.top - lineH - 2.f) / lineH);
    if (maxE < 0) maxE = 0;
    DrawProcessList(rt, titles[dataIdx], data[dataIdx], cnts[dataIdx], area, g_procAbsMode[dataIdx], maxE);
}

static void DispatchPanel(ID2D1RenderTarget* rt, const char* type, D2D1_RECT_F area)
{
    if (!type || !type[0] || strcmp(type, "none") == 0) return;
    if (strcmp(type, "weather") == 0)
    {
        WeatherData wd;
        EnterCriticalSection(&g_weatherCS);
        wd = g_weather;
        LeaveCriticalSection(&g_weatherCS);
        DrawWeather(rt, wd, area);
    }
    else if (strcmp(type, "ip") == 0)
        DrawIpPanel(rt, area);
    else if (strcmp(type, "rss") == 0)
    {
        if (g_rssPanelRectCount < MAX_COLS * MAX_PANELS_PER_COL)
            g_rssPanelRects[g_rssPanelRectCount++] = area;
        DrawHabrPanel(rt, area, g_habrHover);
    }
    else
    {
        int ci = ChartPanelIdx(type);
        if (ci >= 0) { DrawChartPanel(rt, ci, area); return; }
        int pi = ProcPanelIdx(type);
        if (pi >= 0) DrawProcPanel(rt, pi, area);
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

    RECT rcD2D = { 0, 0, w, h };
    if (SUCCEEDED(g_pDCRT->BindDC(hdcMem, &rcD2D)))
    {
        g_pDCRT->BeginDraw();
        g_pDCRT->Clear(D2D1::ColorF(0.f, 0.f, 0.f, 1.f / 255.f));

        // Reset per-frame click-detection rects
        g_cpuChartRect  = {};
        g_gpuChartRect  = {};
        g_ramChartRect  = {};
        g_diskChartRect = {};
        for (int i = 0; i < NUM_CHARTS; ++i) g_procListRects[i] = {};
        g_rssPanelRectCount = 0;

        // Reusable brush helper
        auto MakeBrush = [&]() -> ID2D1SolidColorBrush* {
            ID2D1SolidColorBrush* b = nullptr;
            g_pDCRT->CreateSolidColorBrush(D2D1::ColorF(0.f,0.f,0.f,1.f), &b);
            return b;
        };

        // Draw all columns
        for (int c = 0; c < g_colCount; c++)
        {
            float colL = (c == 0) ? PAD : g_colDivX[c - 1] + PAD;
            float colR = (c < g_colCount - 1) ? g_colDivX[c] - PAD : (float)w - PAD;
            if (colR <= colL) continue;

            int n = g_colPanelCount[c];
            if (n <= 0) continue;

            for (int r = 0; r < n; r++)
            {
                float rowT = (r == 0) ? VEDGE : g_colDivY[c][r - 1] + PAD;
                float rowB = (r == n - 1) ? (float)h - VEDGE : g_colDivY[c][r] - PAD;
                if (rowB <= rowT) continue;

                DispatchPanel(g_pDCRT, g_colPanels[c][r], {colL, rowT, colR, rowB});

                // Horizontal row divider below this panel
                if (r < n - 1 && g_colShowYDiv[c])
                {
                    if (ID2D1SolidColorBrush* pD = MakeBrush())
                    {
                        g_pDCRT->DrawLine({colL, g_colDivY[c][r]}, {colR, g_colDivY[c][r]}, pD, 0.75f);
                        pD->Release();
                    }
                }
            }

            // Vertical divider after this column
            if (c < g_colCount - 1 && g_showVertDividers)
            {
                if (ID2D1SolidColorBrush* pD = MakeBrush())
                {
                    g_pDCRT->DrawLine({g_colDivX[c], VEDGE}, {g_colDivX[c], (float)h - VEDGE}, pD, 0.75f);
                    pD->Release();
                }
            }
        }

        g_pDCRT->EndDraw();
    }

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
        g_fontSize = static_cast<float>(abs(ncm.lfMessageFont.lfHeight)) * 96.f / dpi * g_fontScale;
        ReleaseDC(hwnd, hdc);

        g_pDWFactory->CreateTextFormat(
            ncm.lfMessageFont.lfFaceName, nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, g_fontSize, L"", &g_pChartFmtL);
        if (g_pChartFmtL)
        {
            g_pChartFmtL->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            g_pChartFmtL->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }

        g_pDWFactory->CreateTextFormat(
            ncm.lfMessageFont.lfFaceName, nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, g_fontSize, L"", &g_pChartFmtR);
        if (g_pChartFmtR)
        {
            g_pChartFmtR->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            g_pChartFmtR->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }

        // Monospace format for ASCII art — same size as main font
        g_pDWFactory->CreateTextFormat(
            L"Consolas", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, g_fontSize, L"", &g_pMonoFmt);
        if (g_pMonoFmt)
        {
            g_pMonoFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            g_pMonoFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            g_pMonoFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }

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

        GetCpuName (g_charts[0].name, 256);
        GetGpuName (g_charts[1].name, 256);
        GetRamName (g_charts[2].name, 256);
        GetDiskName(g_charts[3].name, 256);

        swprintf_s(g_gpuVramChart.name,       L"%s VRAM",  g_charts[1].name);
        swprintf_s(g_gpuTempChart.name,       L"%s Temp",  g_charts[1].name);
        swprintf_s(g_cpuTempChart.name,       L"%s Temp",  g_charts[0].name);
        swprintf_s(g_ramTempChart.name,       L"%s Temp",  g_charts[2].name);
        swprintf_s(g_diskTotalReadChart.name,  L"%s Read",  g_charts[3].name);
        swprintf_s(g_diskTotalWriteChart.name, L"%s Write", g_charts[3].name);

        DetectCoreTopology();

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
                L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec",
                0, &g_pdhDiskReadCtr);
            PdhAddEnglishCounterW(g_pdhQuery,
                L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec",
                0, &g_pdhDiskWriteCtr);
            PdhAddEnglishCounterW(g_pdhQuery,
                L"\\GPU Adapter Memory(*)\\Dedicated Usage",
                0, &g_pdhGpuMemCtr);
            PdhAddEnglishCounterW(g_pdhQuery,
                L"\\Processor(*)\\% Processor Time",
                0, &g_pdhCoreCtr);
            // GPU temperature (best-effort; not available on all hardware/drivers)
            PdhAddEnglishCounterW(g_pdhQuery,
                L"\\GPU Engine(*)\\Temperature",
                0, &g_pdhGpuTempCtr);
            // CPU temperature via Thermal Zone (best-effort; values are in tenths of Kelvin)
            PdhAddEnglishCounterW(g_pdhQuery,
                L"\\Thermal Zone Information(*)\\Temperature",
                0, &g_pdhCpuTempCtr);
            // Per-disk counters (enumerate instances first)
            EnumerateDisks();
            // Clamp restored disk mode to valid range
            if (g_diskMode > g_diskCount) g_diskMode = 0;
            // Per-process counters for the process lists
            PdhAddEnglishCounterW(g_pdhQuery,
                L"\\Process(*)\\% Processor Time",
                0, &g_pdhProcCpuCtr);
            PdhAddEnglishCounterW(g_pdhQuery,
                L"\\Process(*)\\Working Set",
                0, &g_pdhProcRamCtr);
            PdhAddEnglishCounterW(g_pdhQuery,
                L"\\Process(*)\\IO Data Bytes/sec",
                0, &g_pdhProcDiskCtr);
            PdhCollectQueryData(g_pdhQuery);
        }

        // Try to load NVML for reliable GPU temperature (works with RTX 5080 / Blackwell
        // where the PDH "GPU Engine\Temperature" counter is unavailable).
        {
            // nvml.dll lives in System32 on modern driver installs; also try NVSMI path.
            g_hNvml = LoadLibraryW(L"nvml.dll");
            if (!g_hNvml)
            {
                wchar_t nvsmi[MAX_PATH];
                ExpandEnvironmentStringsW(
                    L"%ProgramFiles%\\NVIDIA Corporation\\NVSMI\\nvml.dll",
                    nvsmi, MAX_PATH);
                g_hNvml = LoadLibraryW(nvsmi);
            }
            if (!g_hNvml)
            {
                DbgLog("[nvml] nvml.dll not found — NVIDIA GPU temp via NVML unavailable\n");
            }
            if (g_hNvml)
            {
                // Prefer versioned entry points introduced in NVML r304+
                auto pfnInit = (PFN_nvmlInit)GetProcAddress(g_hNvml, "nvmlInit_v2");
                if (!pfnInit)
                    pfnInit = (PFN_nvmlInit)GetProcAddress(g_hNvml, "nvmlInit");

                auto pfnHandle = (PFN_nvmlDeviceGetHandleByIndex)
                    GetProcAddress(g_hNvml, "nvmlDeviceGetHandleByIndex_v2");
                if (!pfnHandle)
                    pfnHandle = (PFN_nvmlDeviceGetHandleByIndex)
                        GetProcAddress(g_hNvml, "nvmlDeviceGetHandleByIndex");

                g_nvmlGetTemp  = (PFN_nvmlDeviceGetTemperature)
                    GetProcAddress(g_hNvml, "nvmlDeviceGetTemperature");
                g_nvmlShutdown = (PFN_nvmlShutdown)
                    GetProcAddress(g_hNvml, "nvmlShutdown");

                if (pfnInit && pfnHandle && g_nvmlGetTemp &&
                    pfnInit() == NVML_SUCCESS)
                {
                    pfnHandle(0, &g_nvmlDevice);
                }

                if (!g_nvmlDevice)
                {
                    DbgLog("[nvml] device init failed (pfnInit=%d pfnHandle=%d getTemp=%d)\n",
                           pfnInit != nullptr, pfnHandle != nullptr, g_nvmlGetTemp != nullptr);
                    if (g_nvmlShutdown) g_nvmlShutdown();
                    FreeLibrary(g_hNvml);
                    g_hNvml       = NULL;
                    g_nvmlGetTemp = NULL;
                }
                else
                {
                    DbgLog("[nvml] initialized, GPU temp via NVML enabled\n");
                }
            }
        }

        {
            FILETIME fi, fk, fu;
            GetSystemTimes(&fi, &fk, &fu);
            auto f2u = [](FILETIME f) -> ULONGLONG {
                return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime;
            };
            g_cpuPrevIdle  = f2u(fi);
            g_cpuPrevTotal = f2u(fk) + f2u(fu);
        }

        SampleMetrics();
        FlushDisplayValues();

        // Initialize dividers from config or compute defaults
        {
            RECT rc; GetWindowRect(hwnd, &rc);
            InitDividers(rc.right - rc.left, rc.bottom - rc.top);
        }

        // Config already loaded in WinMain; kick weather, Habr, CPU temp, and IP fetches
        StartWeatherFetch();
        StartHabrFetch();
        StartCpuTempThread();

        InitializeCriticalSection(&g_ipCS);
        g_hwndForIp = hwnd;
        StartIpFetch();
        StartIpWatchThread();

        SetTimer(hwnd, 1, 50, nullptr);                           // ~20 fps chart update
        SetTimer(hwnd, 2, 600000, nullptr);                        // weather refresh every 10 min
        SetTimer(hwnd, 3, g_habrRefreshMin * 60000, nullptr);     // Habr refresh
        SetTimer(hwnd, 4, 60000, nullptr);                         // IP refresh every 1 min

        UpdateLayeredContent(hwnd);

        // Tray icon
        g_nid.cbSize           = sizeof(g_nid);
        g_nid.hWnd             = hwnd;
        g_nid.uID              = 1;
        g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        g_nid.uCallbackMessage = WM_TRAYICON;
        g_nid.hIcon            = LoadIconW(NULL, IDI_APPLICATION);
        wcscpy_s(g_nid.szTip, L"BlurBox");
        Shell_NotifyIconW(NIM_ADD, &g_nid);

        return 0;
    }

    // ------------------------------------------------------------------
    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP)
        {
            POINT pt;
            GetCursorPos(&pt);
            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu,
                        MF_STRING | (g_autostart ? MF_CHECKED : MF_UNCHECKED),
                        ID_TRAY_AUTOSTART, L"Autostart");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"Exit");
            SetForegroundWindow(hwnd);
            int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                     pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(hMenu);
            if (cmd == ID_TRAY_AUTOSTART)
            {
                g_autostart = !g_autostart;
                ApplyAutostart();
                SaveWindowState(hwnd);
            }
            else if (cmd == ID_TRAY_EXIT)
                DestroyWindow(hwnd);
        }
        return 0;

    // ------------------------------------------------------------------
    case WM_TIMER:
    {
        if (wParam == 1)
        {
            if (g_pdhQuery) PdhCollectQueryData(g_pdhQuery);
            SampleMetrics();
            if (++g_labelTick >= 20)
            {
                g_labelTick = 0;
                FlushDisplayValues();
            }
            UpdateLayeredContent(hwnd);
        }
        else if (wParam == 2)
        {
            StartWeatherFetch();
        }
        else if (wParam == 3)
        {
            StartHabrFetch();
        }
        else if (wParam == 4)
        {
            StartIpFetch((LPVOID)0); // timer-triggered, no delay
        }
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

        int mx = pt.x - rc.left;
        int my = pt.y - rc.top;
        int wh = rc.bottom - rc.top;
        int ww = rc.right - rc.left;

        // Vertical column dividers
        for (int k = 0; k < g_colCount - 1; k++)
            if (abs(mx - (int)g_colDivX[k]) <= DIV_HIT && my >= (int)VEDGE && my <= wh - (int)VEDGE)
                return HTCLIENT;

        // Horizontal row dividers within each column
        for (int c = 0; c < g_colCount; c++)
        {
            float colL = (c == 0) ? PAD : g_colDivX[c - 1] + PAD;
            float colR = (c < g_colCount - 1) ? g_colDivX[c] - PAD : (float)ww - PAD;
            if ((float)mx >= colL && (float)mx <= colR)
                for (int r = 0; r < g_colPanelCount[c] - 1; r++)
                    if (abs(my - (int)g_colDivY[c][r]) <= DIV_HIT)
                        return HTCLIENT;
        }

        // RSS panels need HTCLIENT for link hover/click
        for (int i = 0; i < g_rssPanelRectCount; i++)
            if ((float)mx >= g_rssPanelRects[i].left  && (float)mx <= g_rssPanelRects[i].right &&
                (float)my >= g_rssPanelRects[i].top   && (float)my <= g_rssPanelRects[i].bottom)
                return HTCLIENT;

        // Chart click areas
        D2D1_RECT_F* chartRects[4] = { &g_cpuChartRect, &g_gpuChartRect, &g_ramChartRect, &g_diskChartRect };
        for (int i = 0; i < 4; i++)
            if (chartRects[i]->right > chartRects[i]->left &&
                (float)mx >= chartRects[i]->left && (float)mx <= chartRects[i]->right &&
                (float)my >= chartRects[i]->top  && (float)my <= chartRects[i]->bottom)
                return HTCLIENT;

        // Process list areas
        for (int i = 0; i < NUM_CHARTS; ++i)
            if (g_procListRects[i].right > g_procListRects[i].left &&
                (float)mx >= g_procListRects[i].left  && (float)mx <= g_procListRects[i].right &&
                (float)my >= g_procListRects[i].top   && (float)my <= g_procListRects[i].bottom)
                return HTCLIENT;

        return HTCAPTION;
    }

    // ------------------------------------------------------------------
    case WM_SETCURSOR:
    {
        if (LOWORD(lParam) == HTCLIENT)
        {
            POINT pt; GetCursorPos(&pt);
            RECT rc; GetWindowRect(hwnd, &rc);
            int mx = pt.x - rc.left;
            int my = pt.y - rc.top;
            int wh = rc.bottom - rc.top;

            int ww2 = rc.right - rc.left;
            // Vertical column dividers → horizontal resize cursor
            for (int k = 0; k < g_colCount - 1; k++)
                if (abs(mx - (int)g_colDivX[k]) <= DIV_HIT && my >= (int)VEDGE && my <= wh - (int)VEDGE)
                {
                    SetCursor(LoadCursorW(NULL, IDC_SIZEWE));
                    return TRUE;
                }
            // Horizontal row dividers → vertical resize cursor
            for (int c = 0; c < g_colCount; c++)
            {
                float colL = (c == 0) ? PAD : g_colDivX[c - 1] + PAD;
                float colR = (c < g_colCount - 1) ? g_colDivX[c] - PAD : (float)ww2 - PAD;
                if ((float)mx >= colL && (float)mx <= colR)
                    for (int r = 0; r < g_colPanelCount[c] - 1; r++)
                        if (abs(my - (int)g_colDivY[c][r]) <= DIV_HIT)
                        {
                            SetCursor(LoadCursorW(NULL, IDC_SIZENS));
                            return TRUE;
                        }
            }
            // Link hover
            if (g_habrHover >= 0)
            {
                SetCursor(LoadCursorW(NULL, IDC_HAND));
                return TRUE;
            }
        }
        break;
    }

    // ------------------------------------------------------------------
    case WM_LBUTTONDOWN:
    {
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);
        {
            bool divHit = false;
            for (int k = 0; k < g_colCount - 1 && !divHit; k++)
            {
                if (abs(mx - (int)g_colDivX[k]) <= DIV_HIT)
                {
                    g_draggingColDivIdx = k;
                    SetCapture(hwnd);
                    divHit = true;
                }
            }
            if (!divHit)
            {
                RECT rc2; GetWindowRect(hwnd, &rc2);
                int w = rc2.right - rc2.left;
                for (int c = 0; c < g_colCount && !divHit; c++)
                {
                    float colL = (c == 0) ? PAD : g_colDivX[c-1] + PAD;
                    float colR = (c < g_colCount-1) ? g_colDivX[c] - PAD : (float)w - PAD;
                    if ((float)mx >= colL && (float)mx <= colR)
                    {
                        for (int r = 0; r < g_colPanelCount[c] - 1 && !divHit; r++)
                        {
                            if (abs(my - (int)g_colDivY[c][r]) <= DIV_HIT)
                            {
                                g_draggingRowDivCol = c;
                                g_draggingRowDivIdx = r;
                                SetCapture(hwnd);
                                divHit = true;
                            }
                        }
                    }
                }
            }
            if (divHit) return 0;
        }
        if ((float)mx >= g_cpuChartRect.left && (float)mx <= g_cpuChartRect.right &&
                 (float)my >= g_cpuChartRect.top  && (float)my <= g_cpuChartRect.bottom)
        {
            // Cycle CPU mode: 0 (total) → 1 (logical) → 2 (physical, if SMT) → 3 (load+temp) → 0
            int maxMode = (g_physicalCoreCount > 0 &&
                           g_physicalCoreCount != g_logicalCoreCount) ? 3 : 2;
            g_cpuMode = (g_cpuMode + 1) % (maxMode + 1);
            SaveWindowState(hwnd);
            UpdateLayeredContent(hwnd);
        }
        else if ((float)mx >= g_gpuChartRect.left && (float)mx <= g_gpuChartRect.right &&
                 (float)my >= g_gpuChartRect.top  && (float)my <= g_gpuChartRect.bottom)
        {
            // Cycle GPU mode: 0 (core) → 1 (VRAM %) → 2 (temp) → 3 (core+VRAM) → 4 (core+VRAM+temp) → 0
            g_gpuMode = (g_gpuMode + 1) % 5;
            SaveWindowState(hwnd);
            UpdateLayeredContent(hwnd);
        }
        else if ((float)mx >= g_ramChartRect.left && (float)mx <= g_ramChartRect.right &&
                 (float)my >= g_ramChartRect.top  && (float)my <= g_ramChartRect.bottom)
        {
            // Cycle RAM mode: 0 (load) → 1 (load+temp) → 0
            g_ramMode = (g_ramMode + 1) % 2;
            SaveWindowState(hwnd);
            UpdateLayeredContent(hwnd);
        }
        else if ((float)mx >= g_diskChartRect.left && (float)mx <= g_diskChartRect.right &&
                 (float)my >= g_diskChartRect.top  && (float)my <= g_diskChartRect.bottom)
        {
            // Cycle sub-mode first (single → read+write → read+write+temp),
            // then advance to next disk when sub-mode wraps.
            g_diskSubMode++;
            if (g_diskSubMode >= 3)
            {
                g_diskSubMode = 0;
                if (g_diskCount > 0)
                    g_diskMode = (g_diskMode + 1) % (g_diskCount + 1);
            }
            SaveWindowState(hwnd);
            UpdateLayeredContent(hwnd);
        }
        else if ((float)mx >= g_weatherRect.left && (float)mx <= g_weatherRect.right &&
                 (float)my >= g_weatherRect.top  && (float)my <= g_weatherRect.bottom)
        {
            StartWeatherFetch();
        }
        else
        {
            // Process list click: toggle abs/pct display for the clicked tile
            for (int i = 0; i < NUM_CHARTS; ++i)
            {
                if ((float)mx >= g_procListRects[i].left  && (float)mx <= g_procListRects[i].right &&
                    (float)my >= g_procListRects[i].top   && (float)my <= g_procListRects[i].bottom)
                {
                    g_procAbsMode[i] = !g_procAbsMode[i];
                    SaveWindowState(hwnd);
                    UpdateLayeredContent(hwnd);
                    break;
                }
            }
        }
        return 0;
    }

    case WM_MOUSELEAVE:
    {
        if (g_habrHover >= 0)
        {
            g_habrHover = -1;
            UpdateLayeredContent(hwnd);
        }
        g_hoveredProcList = -1;
        g_hoveredProcRow  = -1;
        break;
    }

    case WM_MOUSEMOVE:
    {
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);

        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        RECT rc; GetWindowRect(hwnd, &rc);

        if (g_draggingColDivIdx >= 0)
        {
            int w = rc.right - rc.left;
            int k = g_draggingColDivIdx;
            float minX = (k == 0) ? 80.f : g_colDivX[k-1] + 80.f;
            float maxX = (k == g_colCount-2) ? (float)w - 80.f : g_colDivX[k+1] - 80.f;
            g_colDivX[k] = max(minX, min((float)mx, maxX));
            UpdateLayeredContent(hwnd);
        }
        else if (g_draggingRowDivCol >= 0)
        {
            int h = rc.bottom - rc.top;
            int c = g_draggingRowDivCol, r = g_draggingRowDivIdx;
            float minY = (r == 0) ? VEDGE + 30.f : g_colDivY[c][r-1] + 30.f;
            float maxY = (r == g_colPanelCount[c]-2) ? (float)h - VEDGE - 30.f : g_colDivY[c][r+1] - 30.f;
            g_colDivY[c][r] = max(minY, min((float)my, maxY));
            UpdateLayeredContent(hwnd);
        }
        else
        {
            // Update link hover
            int newHover = -1;
            for (int i = 0; i < g_habrVisible; i++)
            {
                if ((float)mx >= g_habrRects[i].left  &&
                    (float)mx <= g_habrRects[i].right  &&
                    (float)my >= g_habrRects[i].top    &&
                    (float)my <= g_habrRects[i].bottom)
                {
                    newHover = i;
                    break;
                }
            }
            if (newHover != g_habrHover)
            {
                g_habrHover = newHover;
                UpdateLayeredContent(hwnd);
            }

            // Track which process list tile and row the mouse is over
            int newProcList = -1, newProcRow = -1;
            const float lineH = g_fontSize * 1.4f;
            for (int i = 0; i < NUM_CHARTS; ++i)
            {
                if ((float)mx >= g_procListRects[i].left  &&
                    (float)mx <= g_procListRects[i].right &&
                    (float)my >= g_procListRects[i].top   &&
                    (float)my <= g_procListRects[i].bottom)
                {
                    newProcList = i;
                    float relY = (float)my - g_procListRects[i].top;
                    float afterTitle = lineH + 2.f;
                    if (relY >= afterTitle)
                        newProcRow = (int)((relY - afterTitle) / lineH);
                    break;
                }
            }
            g_hoveredProcList = newProcList;
            g_hoveredProcRow  = newProcRow;
        }
        return 0;
    }

    case WM_NCMOUSEMOVE:
    {
        if (g_habrHover >= 0)
        {
            g_habrHover = -1;
            UpdateLayeredContent(hwnd);
        }
        g_hoveredProcList = -1;
        g_hoveredProcRow  = -1;
        break;
    }

    case WM_LBUTTONUP:
    {
        bool wasDragging = (g_draggingColDivIdx >= 0) || (g_draggingRowDivCol >= 0);
        if (g_draggingColDivIdx >= 0)
        {
            g_draggingColDivIdx = -1;
            ReleaseCapture();
            SaveWindowState(hwnd);
        }
        else if (g_draggingRowDivCol >= 0)
        {
            g_draggingRowDivCol = -1;
            g_draggingRowDivIdx = -1;
            ReleaseCapture();
            SaveWindowState(hwnd);
        }

        // Link click: only if no drag occurred and mouse is on a visible link
        if (!wasDragging && g_habrHover >= 0 && g_habrHover < g_habrVisible)
        {
            int idx = g_habrHover;
            char url[512] = {};
            EnterCriticalSection(&g_habrCS);
            if (idx < g_habrCount)
                strncpy_s(url, g_habr[idx].url, _TRUNCATE);
            LeaveCriticalSection(&g_habrCS);

            if (url[0])
            {
                wchar_t wurl[512] = {};
                MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 512);
                ShellExecuteW(NULL, L"open", wurl, NULL, NULL, SW_SHOWNORMAL);
            }
        }
        return 0;
    }

    // ------------------------------------------------------------------
    case WM_WINDOWPOSCHANGED:
    {
        const auto* wp = reinterpret_cast<const WINDOWPOS*>(lParam);
        if (!(wp->flags & SWP_NOMOVE) || !(wp->flags & SWP_NOSIZE))
        {
            RECT rc; GetWindowRect(hwnd, &rc);
            int w = rc.right - rc.left;
            int h = rc.bottom - rc.top;
            for (int k = 0; k < g_colCount - 1; k++)
            {
                float minX = (k == 0) ? 80.f : g_colDivX[k-1] + 80.f;
                float maxX = (k == g_colCount-2) ? (float)w - 80.f : g_colDivX[k+1] - 80.f;
                if (maxX < minX) maxX = minX;
                g_colDivX[k] = max(minX, min(g_colDivX[k], maxX));
            }
            for (int c = 0; c < g_colCount; c++)
            {
                for (int r = 0; r < g_colPanelCount[c] - 1; r++)
                {
                    float minY = (r == 0) ? VEDGE + 30.f : g_colDivY[c][r-1] + 30.f;
                    float maxY = (r == g_colPanelCount[c]-2) ? (float)h - VEDGE - 30.f : g_colDivY[c][r+1] - 30.f;
                    if (maxY < minY) maxY = minY;
                    g_colDivY[c][r] = max(minY, min(g_colDivY[c][r], maxY));
                }
            }
            UpdateLayeredContent(hwnd);
        }
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
        DestroyWindow(hwnd);
        return 0;

    case WM_RBUTTONUP:
    {
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);

        // Check if the click is on a process list entry; if so, kill that process.
        bool handled = false;
        const float lineH = g_fontSize * 1.4f;
        for (int i = 0; i < NUM_CHARTS; ++i)
        {
            if ((float)mx >= g_procListRects[i].left  &&
                (float)mx <= g_procListRects[i].right &&
                (float)my >= g_procListRects[i].top   &&
                (float)my <= g_procListRects[i].bottom)
            {
                float relY = (float)my - g_procListRects[i].top;
                float afterTitle = lineH + 2.f;
                if (relY >= afterTitle)
                {
                    int row = (int)((relY - afterTitle) / lineH);
                    const ProcEntry* arrs[NUM_CHARTS] = {
                        g_dispProcCpu, g_dispProcGpu, g_dispProcRam, g_dispProcDisk
                    };
                    int cnts[NUM_CHARTS] = {
                        g_dispProcCpuCount, g_dispProcGpuCount,
                        g_dispProcRamCount, g_dispProcDiskCount
                    };
                    if (row >= 0 && row < cnts[i])
                    {
                        const wchar_t* targetName = arrs[i][row].name;
                        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
                        if (snap != INVALID_HANDLE_VALUE)
                        {
                            PROCESSENTRY32W pe = { sizeof(pe) };
                            if (Process32FirstW(snap, &pe))
                            {
                                do {
                                    wchar_t peName[64];
                                    NormalizeProcessName(pe.szExeFile, peName, 64);
                                    if (_wcsicmp(peName, targetName) == 0)
                                    {
                                        HANDLE hp = OpenProcess(PROCESS_TERMINATE, FALSE,
                                                                pe.th32ProcessID);
                                        if (hp) { TerminateProcess(hp, 1); CloseHandle(hp); }
                                    }
                                } while (Process32NextW(snap, &pe));
                            }
                            CloseHandle(snap);
                        }
                    }
                }
                handled = true;
                break;
            }
        }
        if (!handled)
            DestroyWindow(hwnd);
        return 0;
    }

    // ------------------------------------------------------------------
    case WM_FETCH_IP:
        InterlockedExchange(&g_ipFetchPending, 0); // clear debounce flag
        StartIpFetch((LPVOID)lParam);              // lParam: 0=timer, 1=interface-changed
        return 0;

    case WM_REDRAW_IP:
        UpdateLayeredContent(hwnd);
        return 0;

    // ------------------------------------------------------------------
    case WM_DESTROY:
        SaveWindowState(hwnd);
        KillTimer(hwnd, 1);
        KillTimer(hwnd, 2);
        KillTimer(hwnd, 3);
        KillTimer(hwnd, 4);
        g_hwndForIp = NULL;
        if (g_ipWatchStop)
        {
            SetEvent(g_ipWatchStop);
            if (g_ipWatchThread) { WaitForSingleObject(g_ipWatchThread, 3000); CloseHandle(g_ipWatchThread); g_ipWatchThread = NULL; }
            CloseHandle(g_ipWatchStop); g_ipWatchStop = NULL;
        }
        if (g_ipThread) { WaitForSingleObject(g_ipThread, 2000); CloseHandle(g_ipThread); g_ipThread = NULL; }
        DeleteCriticalSection(&g_ipCS);
        if (g_weatherThread)
        {
            WaitForSingleObject(g_weatherThread, 2000);
            CloseHandle(g_weatherThread);
            g_weatherThread = NULL;
        }
        if (g_habrThread)
        {
            WaitForSingleObject(g_habrThread, 2000);
            CloseHandle(g_habrThread);
            g_habrThread = NULL;
        }
        g_cpuTempThreadStop = true;
        if (g_cpuTempThread)
        {
            WaitForSingleObject(g_cpuTempThread, 3000);
            CloseHandle(g_cpuTempThread);
            g_cpuTempThread = NULL;
        }
        DeleteCriticalSection(&g_weatherCS);
        DeleteCriticalSection(&g_habrCS);
        if (g_hNvml)
        {
            if (g_nvmlShutdown) g_nvmlShutdown();
            FreeLibrary(g_hNvml);
            g_hNvml = NULL;
        }
        if (g_pdhQuery)   { PdhCloseQuery(g_pdhQuery); g_pdhQuery = NULL; }
        if (g_pMonoFmt)  { g_pMonoFmt->Release();   g_pMonoFmt   = nullptr; }
        if (g_pChartFmtR){ g_pChartFmtR->Release(); g_pChartFmtR = nullptr; }
        if (g_pChartFmtL){ g_pChartFmtL->Release(); g_pChartFmtL = nullptr; }
        if (g_pDCRT)      { g_pDCRT->Release();       g_pDCRT      = nullptr; }
        if (g_pDWFactory) { g_pDWFactory->Release();  g_pDWFactory = nullptr; }
        if (g_pD2DFactory){ g_pD2DFactory->Release(); g_pD2DFactory = nullptr; }
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// -----------------------------------------------------------------------
// Entry point
// -----------------------------------------------------------------------
int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPSTR, _In_ int nCmdShow)
{
    InitializeCriticalSection(&g_weatherCS);
    InitializeCriticalSection(&g_habrCS);

    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_pD2DFactory);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                        __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(&g_pDWFactory));

    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    g_pD2DFactory->CreateDCRenderTarget(&rtProps, &g_pDCRT);

    // Load config before window creation to restore position/size
    LoadConfig();

    int startX = CW_USEDEFAULT, startY = CW_USEDEFAULT;
    int startW = (g_cfgWinW > 0) ? g_cfgWinW : 600;
    int startH = (g_cfgWinH > 0) ? g_cfgWinH : 560;

    if (g_cfgMonitorLeft != CFG_UNSET && g_cfgWinX != CFG_UNSET)
    {
        MonitorSearchCtx msc = {};
        msc.targetLeft = g_cfgMonitorLeft;
        msc.targetTop  = g_cfgMonitorTop;
        EnumDisplayMonitors(NULL, NULL, FindMonitorProc, (LPARAM)&msc);
        if (msc.found)
        {
            startX = msc.rcMonitor.left + g_cfgWinX;
            startY = msc.rcMonitor.top  + g_cfgWinY;
        }
    }

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
        WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        CLASS_NAME,
        L"BlurBox",
        WS_POPUP | WS_VISIBLE,
        startX, startY, startW, startH,
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
