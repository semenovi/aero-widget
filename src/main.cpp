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
#include <wininet.h>
#include <shellapi.h>
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
// Layout constants
// -----------------------------------------------------------------------
static const float PAD   = 6.f;
static const float VEDGE = 14.f;

// -----------------------------------------------------------------------
// D2D / DirectWrite state
// -----------------------------------------------------------------------
static ID2D1Factory*        g_pD2DFactory = nullptr;
static IDWriteFactory*      g_pDWFactory  = nullptr;
static IDWriteTextFormat*   g_pChartFmtL  = nullptr;   // left/top  – chart name
static IDWriteTextFormat*   g_pChartFmtR  = nullptr;   // right/top – chart value
static ID2D1DCRenderTarget* g_pDCRT       = nullptr;
static IDWriteTextFormat*   g_pMonoFmt    = nullptr;   // monospace – ASCII art
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
    wchar_t absStr[64];
    double  displayCurrent;
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

    rt->DrawRectangle(plotArea, pBrush, 0.75f);

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
// Divider
// -----------------------------------------------------------------------
static float g_dividerX     = 0.f;
static bool  g_draggingDiv  = false;

// Horizontal divider (within left/weather+habr column)
static float g_dividerY     = 0.f;
static bool  g_draggingDivH = false;

static constexpr int DIV_HIT = 4; // px hit area around divider

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

// Saved window-state globals (loaded from config before window creation)
static const int CFG_UNSET = -99999;
static int   g_cfgMonitorLeft = CFG_UNSET;
static int   g_cfgMonitorTop  = CFG_UNSET;
static int   g_cfgWinX        = CFG_UNSET;
static int   g_cfgWinY        = CFG_UNSET;
static int   g_cfgWinW        = -1;
static int   g_cfgWinH        = -1;
static float g_cfgDividerX    = -1.f;
static float g_cfgDividerY    = -1.f;

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
        fputs("{\n    \"location\": \"\"\n}\n", f);
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

    // Escape location string for JSON (handle '"' and '\')
    char escaped[512] = {};
    for (int i = 0, j = 0; g_location[i] && j < (int)sizeof(escaped) - 3; i++)
    {
        if (g_location[i] == '"' || g_location[i] == '\\')
            escaped[j++] = '\\';
        escaped[j++] = g_location[i];
    }

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
        "    \"divider_x\": %.2f,\n"
        "    \"divider_y\": %.2f\n"
        "}\n",
        escaped, monL, monT, relX, relY, winW, winH,
        (double)g_dividerX, (double)g_dividerY);
    fclose(f);
}

// -----------------------------------------------------------------------
// Debug log (appends to weather_debug.log next to the executable)
// -----------------------------------------------------------------------
static void DbgLog(const char* fmt, ...)
{
    wchar_t exeDir[MAX_PATH];
    GetModuleFileNameW(NULL, exeDir, MAX_PATH);
    wchar_t* sl = wcsrchr(exeDir, L'\\');
    if (sl) sl[1] = L'\0';
    wchar_t logPath[MAX_PATH];
    swprintf_s(logPath, L"%sweather_debug.log", exeDir);

    FILE* f = nullptr;
    if (_wfopen_s(&f, logPath, L"a") != 0 || !f) return;

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

    double dv = 0.0;
    if (JsonDouble(buf, "divider_x", &dv) && dv > 0.0)
        g_cfgDividerX = (float)dv;
    if (JsonDouble(buf, "divider_y", &dv) && dv > 0.0)
        g_cfgDividerY = (float)dv;

    int habrMin = 0;
    if (JsonInt(buf, "habr_refresh_minutes", &habrMin) && habrMin > 0)
        g_habrRefreshMin = habrMin;
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
static char* WttrGet(const wchar_t* path, const wchar_t* ua = L"AeroWidget/1.0",
                     bool doLog = false)
{
    wchar_t url[1024];
    swprintf_s(url, L"https://wttr.in%s", path);

    HINTERNET hInet = InternetOpenW(ua, INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInet) return nullptr;

    // Increase receive timeout to 90 s — wttr.in sends chunked data slowly
    DWORD recvTimeout = 90000;
    InternetSetOption(hInet, INTERNET_OPTION_RECEIVE_TIMEOUT, &recvTimeout, sizeof(recvTimeout));

    HINTERNET hUrl = InternetOpenUrlW(hInet, url, nullptr, 0,
        INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);

    char* body = nullptr;
    int   blen = 0;

    if (hUrl)
    {
        if (doLog)
        {
            // Log HTTP status
            DWORD statusCode = 0, scLen = sizeof(statusCode);
            if (HttpQueryInfoW(hUrl, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                    &statusCode, &scLen, nullptr))
                DbgLog("HTTP status: %lu\n", statusCode);
            else
                DbgLog("HTTP status: (unknown)\n");

            // Log Content-Length (may be absent for chunked)
            wchar_t clBuf[64] = {}; DWORD clLen = sizeof(clBuf) - 2;
            if (HttpQueryInfoW(hUrl, HTTP_QUERY_CONTENT_LENGTH, clBuf, &clLen, nullptr))
                DbgLog("Content-Length: %ls\n", clBuf);
            else
                DbgLog("Content-Length: (absent)\n");

            // Log Transfer-Encoding
            wchar_t teBuf[64] = {}; DWORD teLen = sizeof(teBuf) - 2;
            if (HttpQueryInfoW(hUrl, HTTP_QUERY_TRANSFER_ENCODING, teBuf, &teLen, nullptr))
                DbgLog("Transfer-Encoding: %ls\n", teBuf);
            else
                DbgLog("Transfer-Encoding: (absent)\n");
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
                if (doLog)
                    DbgLog("  iter %d: ReadData ok=%d rd=%lu total=%d\n",
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
    InternetCloseHandle(hInet);
    return body;
}

// Generic HTTPS GET – caller must free() result.
static char* HttpGetUrl(const wchar_t* url, const wchar_t* ua = L"AeroWidget/1.0")
{
    HINTERNET hInet = InternetOpenW(ua, INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInet) return nullptr;

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
    char* body = HttpGetUrl(L"https://habr.com/ru/rss/all/all/");
    if (!body) return 0;

    HabrArticle articles[MAX_HABR] = {};
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

    EnterCriticalSection(&g_habrCS);
    memcpy(g_habr, articles, sizeof(HabrArticle) * count);
    g_habrCount = count;
    LeaveCriticalSection(&g_habrCS);
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

    HabrArticle articles[MAX_HABR];
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

        rt->DrawText(articles[i].title, (UINT32)wcslen(articles[i].title),
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
        DbgLog("=== WeatherThreadProc ===\n");
        char* body = WttrGet(path, L"AeroWidget/1.0", /*doLog=*/true);
        if (body)
        {
            int blen = (int)strlen(body);
            DbgLog("body length: %d\n", blen);

            // Log first 200 chars of body to confirm it looks like JSON
            char preview[201] = {};
            strncpy_s(preview, body, 200);
            DbgLog("body[0..200]: %.200s\n\n", preview);

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
                DbgLog("\"weather\":  %s\n", wStart ? "FOUND" : "NOT FOUND");

                if (wStart)
                {
                    DbgLog("offset of \"weather\": %d\n", (int)(wStart - body));

                    for (int day = 0; day < 3; day++)
                    {
                        char date[32]={}, maxT[16]={}, minT[16]={};

                        bool okDate = JsonStr(wStart, "date",     day, date, sizeof(date));
                        bool okMax  = JsonStr(wStart, "maxtempC", day, maxT, sizeof(maxT));
                        bool okMin  = JsonStr(wStart, "mintempC", day, minT, sizeof(minT));

                        DbgLog("day[%d]: date=%s(%d) maxT=%s(%d) minT=%s(%d)\n",
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
            DbgLog("body is NULL (request failed)\n");
        }
    }

    // ── Request 2: ASCII art (current weather only, no ANSI codes) ───────
    // curl User-Agent is required — wttr.in returns HTML for other agents
    {
        wchar_t path[512];
        swprintf_s(path, L"/%s?T&q&0&m", encLoc);
        char* ab = WttrGet(path, L"curl/7.68.0");
        DbgLog("ascii body: %s\n", ab ? "OK" : "NULL");
        if (ab)
        {
            char apreview[201] = {};
            strncpy_s(apreview, ab, 200);
            DbgLog("ascii[0..200]: %.200s\n", apreview);

            int lineIdx = 0;
            const char* lp = ab;
            while (*lp && lineIdx < 5)
            {
                const char* le = lp;
                while (*le && *le != '\n') le++;

                int len = (int)(le - lp);
                if (len > 0 && lp[len - 1] == '\r') len--;

                // wttr.in ASCII art lines always start with a space.
                // Skip lines that don't (e.g. site-name headers like "wttr.in").
                bool hasContent = (len > 0 && lp[0] == ' ');

                if (hasContent)
                {
                    if (len > 71) len = 71;
                    int wlen = MultiByteToWideChar(CP_UTF8, 0, lp, len,
                                                   wd.ascii[lineIdx], 72);
                    if (wlen > 0) wd.ascii[lineIdx][wlen] = L'\0';
                    lineIdx++;
                }

                lp = (*le == '\n') ? le + 1 : le;
            }
            DbgLog("ascii lines parsed: %d\n", lineIdx);
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
// Draw weather panel
// -----------------------------------------------------------------------
static void DrawWeather(ID2D1RenderTarget* rt, const WeatherData& wd, D2D1_RECT_F area)
{
    if (!wd.valid || !g_pChartFmtL) return;

    ID2D1SolidColorBrush* pBrush = nullptr;
    if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0.f, 0.f, 0.f, 1.f), &pBrush))) return;

    const float lh     = g_fontSize * 1.4f;
    const float lhMono = g_fontSize * 1.4f;   // same line height as main text
    float y = area.top;

    // ASCII art icon (monospace, compact)
    if (g_pMonoFmt)
    {
        for (int j = 0; j < 5; j++)
        {
            if (!wd.ascii[j][0]) continue;
            if (y + lhMono > area.bottom) break;
            D2D1_RECT_F r = { area.left, y, area.right, y + lhMono };
            rt->DrawText(wd.ascii[j], (UINT32)wcslen(wd.ascii[j]),
                         g_pMonoFmt, r, pBrush);
            y += lhMono;
        }
        y += lh * 0.3f;
    }

    auto drawLine = [&](const wchar_t* text)
    {
        if (!text[0] || y + lh > area.bottom) return;
        D2D1_RECT_F r = { area.left, y, area.right, y + lh };
        rt->DrawText(text, (UINT32)wcslen(text), g_pChartFmtL, r, pBrush);
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

    RECT rcD2D = { 0, 0, w, h };
    if (SUCCEEDED(g_pDCRT->BindDC(hdcMem, &rcD2D)))
    {
        g_pDCRT->BeginDraw();
        g_pDCRT->Clear(D2D1::ColorF(0.f, 0.f, 0.f, 1.f / 255.f));

        // --- Weather panel (top-left: above horizontal divider) ---
        if (g_dividerX > 2.f * PAD && g_dividerY > VEDGE + PAD)
        {
            WeatherData wd;
            EnterCriticalSection(&g_weatherCS);
            wd = g_weather;
            LeaveCriticalSection(&g_weatherCS);
            D2D1_RECT_F weatherArea = { PAD, VEDGE, g_dividerX - PAD, g_dividerY - PAD };
            DrawWeather(g_pDCRT, wd, weatherArea);
        }

        // --- Horizontal divider (within left panel) ---
        {
            ID2D1SolidColorBrush* pDivH = nullptr;
            if (SUCCEEDED(g_pDCRT->CreateSolidColorBrush(D2D1::ColorF(0.f, 0.f, 0.f, 1.f), &pDivH)))
            {
                g_pDCRT->DrawLine(
                    D2D1::Point2F(PAD, g_dividerY),
                    D2D1::Point2F(g_dividerX - PAD, g_dividerY),
                    pDivH, 0.75f);
                pDivH->Release();
            }
        }

        // --- Habr panel (bottom-left: below horizontal divider) ---
        if (g_dividerX > 2.f * PAD && g_dividerY + PAD < (float)h - VEDGE)
        {
            D2D1_RECT_F habrArea = { PAD, g_dividerY + PAD,
                                     g_dividerX - PAD, (float)h - VEDGE };
            DrawHabrPanel(g_pDCRT, habrArea, g_habrHover);
        }

        // --- Vertical divider line ---
        {
            ID2D1SolidColorBrush* pDiv = nullptr;
            if (SUCCEEDED(g_pDCRT->CreateSolidColorBrush(D2D1::ColorF(0.f, 0.f, 0.f, 1.f), &pDiv)))
            {
                g_pDCRT->DrawLine(
                    D2D1::Point2F(g_dividerX, VEDGE),
                    D2D1::Point2F(g_dividerX, (float)h - VEDGE),
                    pDiv, 0.75f);
                pDiv->Release();
            }
        }

        // --- Charts (right of divider) ---
        {
            const float chartLeft = g_dividerX + PAD;
            const float colW = (float)w - chartLeft - PAD;
            const float rowH = ((float)h - 2.f * VEDGE - (NUM_CHARTS - 1) * PAD) / NUM_CHARTS;

            for (int i = 0; i < NUM_CHARTS; ++i)
            {
                float x = chartLeft;
                float y = VEDGE + i * (rowH + PAD);
                D2D1_RECT_F area = { x, y, x + colW, y + rowH };
                DrawChart(g_pDCRT, g_charts[i], area, g_pChartFmtL, g_pChartFmtR);
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
        g_fontSize = static_cast<float>(abs(ncm.lfMessageFont.lfHeight)) * 96.f / dpi;
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
            PdhCollectQueryData(g_pdhQuery);
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

        // Restore dividers from config or set defaults
        {
            RECT rc; GetWindowRect(hwnd, &rc);
            int w = rc.right - rc.left;
            int h = rc.bottom - rc.top;
            if (g_cfgDividerX > 0.f)
                g_dividerX = max(80.f, min(g_cfgDividerX, (float)w - 80.f));
            else
                g_dividerX = (float)w * 0.28f;

            if (g_cfgDividerY > 0.f)
                g_dividerY = max(60.f, min(g_cfgDividerY, (float)h - 60.f));
            else
                g_dividerY = (float)h * 0.55f;
        }

        // Config already loaded in WinMain; kick weather and Habr fetches
        StartWeatherFetch();
        StartHabrFetch();

        SetTimer(hwnd, 1, 50, nullptr);                           // ~20 fps chart update
        SetTimer(hwnd, 2, 600000, nullptr);                        // weather refresh every 10 min
        SetTimer(hwnd, 3, g_habrRefreshMin * 60000, nullptr);     // Habr refresh

        UpdateLayeredContent(hwnd);
        return 0;
    }

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

        // Vertical divider hit → client (so WM_LBUTTONDOWN fires)
        if (abs(mx - (int)g_dividerX) <= DIV_HIT &&
            my >= (int)VEDGE && my <= wh - (int)VEDGE)
            return HTCLIENT;

        // Horizontal divider hit (within left panel)
        if (mx >= 0 && mx < (int)g_dividerX - DIV_HIT &&
            abs(my - (int)g_dividerY) <= DIV_HIT)
            return HTCLIENT;

        // Habr panel (below horizontal divider, left of vertical divider)
        if (mx >= 0 && mx < (int)g_dividerX - DIV_HIT &&
            my > (int)g_dividerY + DIV_HIT && my < wh - (int)VEDGE)
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

            // Vertical divider
            if (abs(mx - (int)g_dividerX) <= DIV_HIT &&
                my >= (int)VEDGE && my <= wh - (int)VEDGE)
            {
                SetCursor(LoadCursorW(NULL, IDC_SIZEWE));
                return TRUE;
            }
            // Horizontal divider
            if (mx >= 0 && mx < (int)g_dividerX - DIV_HIT &&
                abs(my - (int)g_dividerY) <= DIV_HIT)
            {
                SetCursor(LoadCursorW(NULL, IDC_SIZENS));
                return TRUE;
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
        if (abs(mx - (int)g_dividerX) <= DIV_HIT)
        {
            g_draggingDiv = true;
            SetCapture(hwnd);
        }
        else if (mx >= 0 && mx < (int)g_dividerX - DIV_HIT &&
                 abs(my - (int)g_dividerY) <= DIV_HIT)
        {
            g_draggingDivH = true;
            SetCapture(hwnd);
        }
        return 0;
    }

    case WM_MOUSEMOVE:
    {
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);
        RECT rc; GetWindowRect(hwnd, &rc);

        if (g_draggingDiv)
        {
            int w = rc.right - rc.left;
            g_dividerX = max(80.f, min((float)mx, (float)w - 80.f));
            UpdateLayeredContent(hwnd);
        }
        else if (g_draggingDivH)
        {
            int h = rc.bottom - rc.top;
            g_dividerY = max(60.f, min((float)my, (float)h - 60.f));
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
        break;
    }

    case WM_LBUTTONUP:
    {
        bool wasDivV = g_draggingDiv;
        bool wasDivH = g_draggingDivH;

        if (g_draggingDiv)
        {
            g_draggingDiv = false;
            ReleaseCapture();
        }
        else if (g_draggingDivH)
        {
            g_draggingDivH = false;
            ReleaseCapture();
        }

        // Link click: only if no drag occurred and mouse is on a visible link
        if (!wasDivV && !wasDivH &&
            g_habrHover >= 0 && g_habrHover < g_habrVisible)
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
            g_dividerX = max(80.f, min(g_dividerX, (float)w - 80.f));
            g_dividerY = max(60.f, min(g_dividerY, (float)h - 60.f));
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
    case WM_RBUTTONUP:
        DestroyWindow(hwnd);
        return 0;

    // ------------------------------------------------------------------
    case WM_DESTROY:
        SaveWindowState(hwnd);
        KillTimer(hwnd, 1);
        KillTimer(hwnd, 2);
        KillTimer(hwnd, 3);
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
        DeleteCriticalSection(&g_weatherCS);
        DeleteCriticalSection(&g_habrCS);
        if (g_pdhQuery)   { PdhCloseQuery(g_pdhQuery); g_pdhQuery = NULL; }
        if (g_pMonoFmt)  { g_pMonoFmt->Release();   g_pMonoFmt   = nullptr; }
        if (g_pChartFmtR){ g_pChartFmtR->Release(); g_pChartFmtR = nullptr; }
        if (g_pChartFmtL){ g_pChartFmtL->Release(); g_pChartFmtL = nullptr; }
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
        WS_EX_LAYERED,
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
