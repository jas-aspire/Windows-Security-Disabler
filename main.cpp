#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <d3d11.h>
#include <string>
#include <thread>
#include <atomic>
#include <cstdio>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "resources.h"   // embedded binaries & scripts

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "shell32.lib")

// ─────────────────────────────────────────────────────────────────────────────
//  D3D11 globals
// ─────────────────────────────────────────────────────────────────────────────
static ID3D11Device*           g_dev  = nullptr;
static ID3D11DeviceContext*    g_ctx  = nullptr;
static IDXGISwapChain*         g_sc   = nullptr;
static ID3D11RenderTargetView* g_rtv  = nullptr;
static HWND                    g_hwnd = nullptr;

// ─────────────────────────────────────────────────────────────────────────────
//  App state
// ─────────────────────────────────────────────────────────────────────────────
enum StepStatus { ST_IDLE, ST_RUNNING, ST_OK, ST_FAIL };

static std::atomic<int>  g_countdown{ -1 };
static std::atomic<bool> g_busy{ false };
static int               g_page = 0;   // 0 = Disable, 1 = Enable

struct PageState { StepStatus steps[2] = { ST_IDLE, ST_IDLE }; };
static PageState g_ps[2];

static void ResetPage(int p) { g_ps[p].steps[0] = g_ps[p].steps[1] = ST_IDLE; }

// ─────────────────────────────────────────────────────────────────────────────
//  Helpers — write embedded resource to temp file, then run it
// ─────────────────────────────────────────────────────────────────────────────
static std::string TmpPath(const char* name)
{
    char t[MAX_PATH];
    GetTempPathA(MAX_PATH, t);
    return std::string(t) + name;
}

static bool WriteTmp(const char* name, const unsigned char* data, int len)
{
    std::string path = TmpPath(name);
    HANDLE hf = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    WriteFile(hf, data, (DWORD)len, &written, nullptr);
    CloseHandle(hf);
    return written == (DWORD)len;
}

static bool RunExe(const char* name)
{
    std::string path = TmpPath(name);
    SHELLEXECUTEINFOA sei = {};
    sei.cbSize  = sizeof(sei);
    sei.fMask   = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb  = "runas";
    sei.lpFile  = path.c_str();
    sei.nShow   = SW_HIDE;
    if (!ShellExecuteExA(&sei)) return false;
    WaitForSingleObject(sei.hProcess, 90000);
    CloseHandle(sei.hProcess);
    return true;
}

static bool RunPS1(const char* name)
{
    std::string path = TmpPath(name);
    std::string args = std::string("-ExecutionPolicy Bypass -File \"") + path + "\"";

    SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    HANDLE rPipe = nullptr, wPipe = nullptr;
    if (!CreatePipe(&rPipe, &wPipe, &sa, 0)) return false;
    SetHandleInformation(wPipe, HANDLE_FLAG_INHERIT, 0);

    const char* ans = "N\r\n";
    DWORD wr = 0;
    WriteFile(wPipe, ans, (DWORD)strlen(ans), &wr, nullptr);
    CloseHandle(wPipe);

    STARTUPINFOA si{};
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput   = rPipe;
    si.hStdOutput  = INVALID_HANDLE_VALUE;
    si.hStdError   = INVALID_HANDLE_VALUE;

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "powershell.exe %s", args.c_str());

    PROCESS_INFORMATION pi{};
    bool ok = CreateProcessA(nullptr, cmd, nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) != 0;
    CloseHandle(rPipe);
    if (!ok) return false;
    WaitForSingleObject(pi.hProcess, 120000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Worker tasks (run on background threads)
// ─────────────────────────────────────────────────────────────────────────────
static void RunTask(int page)
{
    g_busy = true;
    ResetPage(page);

    // Step 0 — exe
    g_ps[page].steps[0] = ST_RUNNING;
    bool ok;
    if (page == 0) {
        ok = WriteTmp("disable-defender.exe", disable_defender_exe, disable_defender_exe_len)
          && RunExe("disable-defender.exe");
    } else {
        ok = WriteTmp("enable-defender.exe", enable_defender_exe, enable_defender_exe_len)
          && RunExe("enable-defender.exe");
    }
    g_ps[page].steps[0] = ok ? ST_OK : ST_FAIL;

    // Step 1 — PS1
    g_ps[page].steps[1] = ST_RUNNING;
    if (page == 0) {
        ok = WriteTmp("disablecoreisolationanduac.ps1", disable_ps1, disable_ps1_len)
          && RunPS1("disablecoreisolationanduac.ps1");
    } else {
        ok = WriteTmp("enablecoreisolationanduac.ps1", enable_ps1, enable_ps1_len)
          && RunPS1("enablecoreisolationanduac.ps1");
    }
    g_ps[page].steps[1] = ok ? ST_OK : ST_FAIL;

    g_busy = false;
}

static void TaskRestart()
{
    for (int i = 5; i >= 1; --i) { g_countdown = i; Sleep(1000); }
    g_countdown = 0;

    HANDLE ht; TOKEN_PRIVILEGES tp;
    if (OpenProcessToken(GetCurrentProcess(),
                         TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &ht)) {
        LookupPrivilegeValueA(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid);
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(ht, FALSE, &tp, 0, nullptr, nullptr);
        CloseHandle(ht);
    }
    ExitWindowsEx(EWX_REBOOT | EWX_FORCE, SHTDN_REASON_MAJOR_APPLICATION);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Theme
// ─────────────────────────────────────────────────────────────────────────────
static inline ImVec4 Col(int r, int g, int b, int a = 255)
{
    return ImVec4(r / 255.f, g / 255.f, b / 255.f, a / 255.f);
}

static void ApplyTheme()
{
    ImGuiStyle& s = ImGui::GetStyle();

    // Geometry
    s.WindowRounding   = 0.f;
    s.ChildRounding    = 4.f;
    s.FrameRounding    = 4.f;
    s.PopupRounding    = 4.f;
    s.ScrollbarRounding= 4.f;
    s.GrabRounding     = 4.f;
    s.TabRounding      = 4.f;

    s.WindowBorderSize = 0.f;
    s.FrameBorderSize  = 0.f;
    s.ChildBorderSize  = 1.f;

    s.WindowPadding  = ImVec2(14, 12);
    s.FramePadding   = ImVec2(10, 6);
    s.ItemSpacing    = ImVec2(8, 7);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.IndentSpacing  = 18.f;
    s.ScrollbarSize  = 10.f;
    s.GrabMinSize    = 8.f;

    // Palette — dark navy / slate
    auto& c = s.Colors;
    c[ImGuiCol_Text]                = Col(218, 224, 240);
    c[ImGuiCol_TextDisabled]        = Col( 80, 100, 138);
    c[ImGuiCol_WindowBg]            = Col( 13,  16,  26);
    c[ImGuiCol_ChildBg]             = Col( 18,  22,  36);
    c[ImGuiCol_PopupBg]             = Col( 18,  22,  36);
    c[ImGuiCol_Border]              = Col( 32,  44,  72);
    c[ImGuiCol_BorderShadow]        = Col(  0,   0,   0,  0);
    c[ImGuiCol_FrameBg]             = Col( 22,  28,  48);
    c[ImGuiCol_FrameBgHovered]      = Col( 30,  38,  64);
    c[ImGuiCol_FrameBgActive]       = Col( 38,  50,  82);
    c[ImGuiCol_TitleBg]             = Col(  9,  12,  20);
    c[ImGuiCol_TitleBgActive]       = Col( 12,  16,  28);
    c[ImGuiCol_TitleBgCollapsed]    = Col(  9,  12,  20);
    c[ImGuiCol_MenuBarBg]           = Col( 13,  16,  26);
    c[ImGuiCol_ScrollbarBg]         = Col( 10,  13,  22);
    c[ImGuiCol_ScrollbarGrab]       = Col( 38,  60, 128);
    c[ImGuiCol_ScrollbarGrabHovered]= Col( 50,  80, 160);
    c[ImGuiCol_ScrollbarGrabActive] = Col( 60,  96, 190);
    c[ImGuiCol_CheckMark]           = Col( 90, 160, 255);
    c[ImGuiCol_SliderGrab]          = Col( 70, 130, 220);
    c[ImGuiCol_SliderGrabActive]    = Col( 90, 160, 255);
    c[ImGuiCol_Button]              = Col( 28,  50, 112);
    c[ImGuiCol_ButtonHovered]       = Col( 40,  70, 155);
    c[ImGuiCol_ButtonActive]        = Col( 55,  92, 200);
    c[ImGuiCol_Header]              = Col( 28,  50, 112);
    c[ImGuiCol_HeaderHovered]       = Col( 38,  65, 145);
    c[ImGuiCol_HeaderActive]        = Col( 50,  85, 185);
    c[ImGuiCol_Separator]           = Col( 28,  40,  68);
    c[ImGuiCol_SeparatorHovered]    = Col( 50,  80, 150);
    c[ImGuiCol_SeparatorActive]     = Col( 70, 110, 200);
    c[ImGuiCol_ResizeGrip]          = Col( 28,  50, 112,  80);
    c[ImGuiCol_ResizeGripHovered]   = Col( 50,  80, 160, 160);
    c[ImGuiCol_ResizeGripActive]    = Col( 70, 110, 210, 255);
    c[ImGuiCol_Tab]                 = Col( 14,  18,  32);
    c[ImGuiCol_TabHovered]          = Col( 36,  58, 130);
    c[ImGuiCol_TabSelected]         = Col( 28,  48, 110);
    c[ImGuiCol_TabSelectedOverline] = Col( 80, 148, 255);
    c[ImGuiCol_TabDimmed]           = Col( 12,  15,  28);
    c[ImGuiCol_TabDimmedSelected]   = Col( 20,  34,  78);
    c[ImGuiCol_PlotLines]           = Col( 80, 148, 255);
    c[ImGuiCol_PlotLinesHovered]    = Col(120, 190, 255);
    c[ImGuiCol_PlotHistogram]       = Col( 50, 120, 220);
    c[ImGuiCol_PlotHistogramHovered]= Col( 80, 160, 255);
    c[ImGuiCol_NavHighlight]        = Col( 80, 148, 255);
    c[ImGuiCol_ModalWindowDimBg]    = Col(  0,   0,   0, 100);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Small helpers for the UI
// ─────────────────────────────────────────────────────────────────────────────
static ImU32 StatusColor(StepStatus st)
{
    switch (st) {
        case ST_RUNNING: return IM_COL32(220, 168,  28, 255);
        case ST_OK:      return IM_COL32( 46, 196,  98, 255);
        case ST_FAIL:    return IM_COL32(210,  52,  52, 255);
        default:         return IM_COL32( 44,  60, 105, 255);
    }
}

static const char* StatusLabel(StepStatus st)
{
    switch (st) {
        case ST_RUNNING: return "Running";
        case ST_OK:      return "Done";
        case ST_FAIL:    return "Failed";
        default:         return "Idle";
    }
}

// Draw a filled circle as a status dot, then advance cursor
static void Dot(StepStatus st, float radius = 4.5f)
{
    ImVec2 p  = ImGui::GetCursorScreenPos();
    float  cy = p.y + ImGui::GetTextLineHeight() * 0.5f + 1.f;
    ImGui::GetWindowDrawList()->AddCircleFilled(
        { p.x + radius + 1.f, cy }, radius, StatusColor(st));
    ImGui::Dummy(ImVec2(radius * 2.f + 4.f, 0.f));
}

// Full-width separator with some vertical breathing room
static void HRule()
{
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Step row — icon + name + status badge
// ─────────────────────────────────────────────────────────────────────────────
static void StepRow(const char* label, StepStatus st, ImU32 bgCol)
{
    const float rowH = 34.f;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(bgCol));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.f);
    ImGui::BeginChild(label, ImVec2(-1.f, rowH), ImGuiChildFlags_Borders);

    // Vertical centre everything
    float textH = ImGui::GetTextLineHeight();
    float offsetY = (rowH - textH) * 0.5f - 2.f;
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + offsetY);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 6.f);

    // Dot
    Dot(st);
    ImGui::SameLine(0.f, 7.f);

    // Label
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + offsetY);
    ImGui::PushStyleColor(ImGuiCol_Text, Col(188, 202, 238));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();

    // Right-aligned badge
    const char* badge = StatusLabel(st);
    float badgeW = ImGui::CalcTextSize(badge).x;
    ImGui::SameLine(ImGui::GetWindowWidth() - badgeW - 14.f);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + offsetY);

    ImVec4 bc = ImGui::ColorConvertU32ToFloat4(StatusColor(st));
    if (st == ST_IDLE) bc = Col(52, 70, 115);   // dimmer for idle
    ImGui::PushStyleColor(ImGuiCol_Text, bc);
    ImGui::TextUnformatted(badge);
    ImGui::PopStyleColor();

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Main UI render
// ─────────────────────────────────────────────────────────────────────────────
static void RenderUI()
{
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,    0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,  0.f);
    ImGui::Begin("##root", nullptr,
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove       |
        ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);

    const bool busy = g_busy.load();
    const int  cd   = g_countdown.load();

    // ── Header / title ────────────────────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_Text, Col(100, 160, 255));
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize("Windows Disabler").x) * 0.5f);
    ImGui::Text("Windows Disabler");
    ImGui::PopStyleColor();

    ImGui::PushStyleColor(ImGuiCol_Text, Col(60, 80, 130));
    ImGui::SetCursorPosX((ImGui::GetWindowWidth()
        - ImGui::CalcTextSize("Windows Defender Manager").x) * 0.5f);
    ImGui::TextUnformatted("Windows Defender Manager");
    ImGui::PopStyleColor();

    HRule();

    // ── Tab bar ───────────────────────────────────────────────────────────────
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(18.f, 5.f));
    if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_None)) {
        if (ImGui::BeginTabItem(" Disable ")) { g_page = 0; ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(" Enable  ")) { g_page = 1; ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::PopStyleVar();
    ImGui::Spacing();

    // ── Description ───────────────────────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_Text, Col(72, 94, 148));
    if (g_page == 0)
        ImGui::TextWrapped("Turns off Windows Defender and locks Core Isolation + UAC.");
    else
        ImGui::TextWrapped("Restores Windows Defender and re-enables Core Isolation + UAC.");
    ImGui::PopStyleColor();

    ImGui::Spacing();

    // ── Step rows ─────────────────────────────────────────────────────────────
    const ImU32 rowBg = IM_COL32(20, 26, 44, 255);
    StepRow("Defender",                  g_ps[g_page].steps[0], rowBg);
    ImGui::Spacing();
    StepRow("Core Isolation  +  UAC",    g_ps[g_page].steps[1], rowBg);

    ImGui::Spacing();
    HRule();

    // ── Apply button ──────────────────────────────────────────────────────────
    if (busy) ImGui::BeginDisabled();

    ImVec4 btnN, btnH, btnA;
    if (g_page == 0) {
        btnN = Col( 28,  52, 120);
        btnH = Col( 40,  72, 160);
        btnA = Col( 55,  96, 200);
    } else {
        btnN = Col( 16,  60,  32);
        btnH = Col( 22,  84,  44);
        btnA = Col( 30, 110,  58);
    }

    ImGui::PushStyleColor(ImGuiCol_Button,        btnN);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, btnH);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  btnA);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.f, 9.f));

    const char* applyLabel = busy ? "  Working...  " : "  Apply  ";
    if (ImGui::Button(applyLabel, ImVec2(-1.f, 0.f)) && !busy) {
        int p = g_page;
        ResetPage(p);
        std::thread([p]{ RunTask(p); }).detach();
    }

    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);

    if (busy) ImGui::EndDisabled();

    // ── Restart button — pinned to bottom ─────────────────────────────────────
    const float restartH = 30.f;
    const float padY     = ImGui::GetStyle().WindowPadding.y;
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - restartH - padY);

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.f, 7.f));

    if (cd >= 0) {
        char buf[40];
        if (cd > 0) snprintf(buf, sizeof(buf), "Restarting in %d ...", cd);
        else        snprintf(buf, sizeof(buf), "Restarting ...");
        ImGui::PushStyleColor(ImGuiCol_Button,        Col(88, 18, 18));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Col(88, 18, 18));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  Col(88, 18, 18));
        ImGui::Button(buf, ImVec2(-1.f, 0.f));
        ImGui::PopStyleColor(3);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button,        Col(58, 14, 14));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Col(88, 20, 20));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  Col(118, 28, 28));
        if (ImGui::Button("  Restart  (5s countdown)  ", ImVec2(-1.f, 0.f))
                && !busy && cd < 0)
            std::thread(TaskRestart).detach();
        ImGui::PopStyleColor(3);
    }

    ImGui::PopStyleVar();
    ImGui::End();
}

// ─────────────────────────────────────────────────────────────────────────────
//  D3D11 boilerplate
// ─────────────────────────────────────────────────────────────────────────────
static bool CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount            = 2;
    sd.BufferDesc.Format      = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.Flags                  = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage            = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow           = hWnd;
    sd.SampleDesc.Count       = 1;
    sd.Windowed               = TRUE;
    sd.SwapEffect             = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL lvl[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    return D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
               0, lvl, 2, D3D11_SDK_VERSION, &sd, &g_sc, &g_dev, &fl, &g_ctx) == S_OK
           && [&]{ ID3D11Texture2D* bb = nullptr;
                   g_sc->GetBuffer(0, IID_PPV_ARGS(&bb));
                   g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
                   bb->Release(); return true; }();
}

static void CleanupDeviceD3D()
{
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    if (g_sc)  { g_sc->Release();  g_sc  = nullptr; }
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
}

static void CreateRT()
{
    ID3D11Texture2D* bb = nullptr;
    g_sc->GetBuffer(0, IID_PPV_ARGS(&bb));
    g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
    bb->Release();
}

static void CleanupRT()
{
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Window procedure
// ─────────────────────────────────────────────────────────────────────────────
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wP, LPARAM lP)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wP, lP)) return true;
    switch (msg) {
    case WM_SIZE:
        if (g_dev && wP != SIZE_MINIMIZED) {
            CleanupRT();
            g_sc->ResizeBuffers(0, (UINT)LOWORD(lP), (UINT)HIWORD(lP),
                                DXGI_FORMAT_UNKNOWN, 0);
            CreateRT();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wP & 0xFFF0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wP, lP);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Entry point
// ─────────────────────────────────────────────────────────────────────────────
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int)
{
    WNDCLASSEXA wc = { sizeof(wc), CS_CLASSDC, WndProc, 0, 0,
                       GetModuleHandle(nullptr), nullptr, nullptr,
                       nullptr, nullptr, "UTClass", nullptr };
    RegisterClassExA(&wc);

    const int W = 330, H = 320;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - W) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - H) / 2;

    g_hwnd = CreateWindowA(wc.lpszClassName, "Windows Disabler",
                           WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                           sx, sy, W, H, nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(g_hwnd)) return 1;
    ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    ApplyTheme();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    const float clearColor[4] = { 13/255.f, 16/255.f, 26/255.f, 1.f };

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        RenderUI();

        ImGui::Render();
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_sc->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(g_hwnd);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);
    return 0;
}
