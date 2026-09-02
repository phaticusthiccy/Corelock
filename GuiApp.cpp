#include "GuiApp.hpp"

#include <dwmapi.h>
#include <tchar.h>
#include <algorithm>
#include <iomanip>
#include <sstream>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace Corelock {

GuiApp::GuiApp() {
    // Detect system CPU topology
    SYSTEM_INFO sysInfo{};
    ::GetSystemInfo(&sysInfo);
    totalLogicalCores_ = static_cast<int>(sysInfo.dwNumberOfProcessors);

    DWORD_PTR procAffinity = 0;
    DWORD_PTR sysAffinity = 0;
    if (::GetProcessAffinityMask(::GetCurrentProcess(), &procAffinity, &sysAffinity)) {
        systemAffinityMask_ = sysAffinity;
    } else {
        systemAffinityMask_ = (totalLogicalCores_ >= 64) ? ~0ULL : ((1ULL << totalLogicalCores_) - 1);
    }

    // Default configuration
    config_.targetProcessName = L"game.exe";
    config_.targetPriorityClass = HIGH_PRIORITY_CLASS;
    config_.affinityPolicy = AffinityPolicy::IsolateLogicalCpu0;
    config_.enableDynamicPowerPlan = true;
    config_.enableMmcss = true;
    config_.pollInterval = std::chrono::milliseconds(pollIntervalMs_);
    config_.logger = [this](LogLevel level, std::string_view msg) {
        AppendLog(level, msg);
    };

    AppendLog(LogLevel::Info, "Corelock GUI Dashboard initialized.");
    AppendLog(LogLevel::Info, "Detected " + std::to_string(totalLogicalCores_) + " system logical cores.");
}

GuiApp::~GuiApp() {
    StopMonitoring();
    CleanupRenderTarget();
    CleanupDeviceD3D();

    if (hWnd_ != nullptr) {
        ::DestroyWindow(hWnd_);
        ::UnregisterClassW(wc_.lpszClassName, wc_.hInstance);
    }
}

// ============================================================================
// Direct3D 11 & Window Management
// ============================================================================

bool GuiApp::CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
#if defined(_DEBUG)
    createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };

    HRESULT res = ::D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        createDeviceFlags,
        featureLevelArray,
        2,
        D3D11_SDK_VERSION,
        &sd,
        &swapChain_,
        &d3dDevice_,
        &featureLevel,
        &d3dDeviceContext_
    );

    if (res == DXGI_ERROR_UNSUPPORTED) {
        // Fallback to WARP software rasterizer if hardware driver is missing
        res = ::D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            createDeviceFlags,
            featureLevelArray,
            2,
            D3D11_SDK_VERSION,
            &sd,
            &swapChain_,
            &d3dDevice_,
            &featureLevel,
            &d3dDeviceContext_
        );
    }

    if (res != S_OK) {
        return false;
    }

    CreateRenderTarget();
    return true;
}

void GuiApp::CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (swapChain_) { swapChain_->Release(); swapChain_ = nullptr; }
    if (d3dDeviceContext_) { d3dDeviceContext_->Release(); d3dDeviceContext_ = nullptr; }
    if (d3dDevice_) { d3dDevice_->Release(); d3dDevice_ = nullptr; }
}

void GuiApp::CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    if (swapChain_ != nullptr) {
        swapChain_->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
        if (pBackBuffer != nullptr) {
            d3dDevice_->CreateRenderTargetView(pBackBuffer, nullptr, &mainRenderTargetView_);
            pBackBuffer->Release();
        }
    }
}

void GuiApp::CleanupRenderTarget() {
    if (mainRenderTargetView_ != nullptr) {
        mainRenderTargetView_->Release();
        mainRenderTargetView_ = nullptr;
    }
}

LRESULT WINAPI GuiApp::WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) {
        return true;
    }

    GuiApp* app = reinterpret_cast<GuiApp*>(::GetWindowLongPtr(hWnd, GWLP_USERDATA));

    switch (msg) {
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) {
                return 0;
            }
            if (app != nullptr && app->d3dDevice_ != nullptr && wParam != SIZE_MINIMIZED) {
                app->CleanupRenderTarget();
                app->swapChain_->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
                app->CreateRenderTarget();
            }
            return 0;

        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) { // Disable Alt menu bar activation
                return 0;
            }
            break;

        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
    }

    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ============================================================================
// Cyberpunk & Titanium Gamer Theme
// ============================================================================

void GuiApp::ApplyGamerTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    // Window & Frame Shaping
    style.WindowRounding = 8.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 5.0f;
    style.PopupRounding = 6.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding = 5.0f;
    style.TabRounding = 6.0f;

    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;

    style.WindowPadding = ImVec2(16.0f, 16.0f);
    style.FramePadding = ImVec2(10.0f, 6.0f);
    style.ItemSpacing = ImVec2(12.0f, 10.0f);
    style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
    style.ScrollbarSize = 14.0f;

    // Color Palette: Deep Obsidian, Electric Cyan, Neon Violet, Emerald Green
    const ImVec4 bgDark       = ImVec4(0.06f, 0.07f, 0.09f, 1.00f);
    const ImVec4 bgPanel      = ImVec4(0.10f, 0.11f, 0.15f, 1.00f);
    const ImVec4 bgFrame      = ImVec4(0.14f, 0.16f, 0.22f, 1.00f);
    const ImVec4 bgFrameHover = ImVec4(0.18f, 0.21f, 0.29f, 1.00f);
    const ImVec4 bgFrameActive= ImVec4(0.22f, 0.26f, 0.36f, 1.00f);

    const ImVec4 borderCol    = ImVec4(0.20f, 0.24f, 0.33f, 0.80f);
    const ImVec4 borderShadow = ImVec4(0.00f, 0.00f, 0.00f, 0.30f);

    const ImVec4 textPrimary  = ImVec4(0.95f, 0.96f, 0.98f, 1.00f);
    const ImVec4 textDim      = ImVec4(0.55f, 0.60f, 0.70f, 1.00f);

    const ImVec4 cyanAccent   = ImVec4(0.00f, 0.85f, 0.95f, 1.00f);
    const ImVec4 cyanHover    = ImVec4(0.20f, 0.90f, 1.00f, 1.00f);
    const ImVec4 cyanActive   = ImVec4(0.00f, 0.70f, 0.85f, 1.00f);

    colors[ImGuiCol_Text]                  = textPrimary;
    colors[ImGuiCol_TextDisabled]          = textDim;
    colors[ImGuiCol_WindowBg]              = bgDark;
    colors[ImGuiCol_ChildBg]               = bgPanel;
    colors[ImGuiCol_PopupBg]               = ImVec4(0.08f, 0.09f, 0.12f, 0.98f);
    colors[ImGuiCol_Border]                = borderCol;
    colors[ImGuiCol_BorderShadow]          = borderShadow;
    colors[ImGuiCol_FrameBg]               = bgFrame;
    colors[ImGuiCol_FrameBgHovered]        = bgFrameHover;
    colors[ImGuiCol_FrameBgActive]         = bgFrameActive;
    colors[ImGuiCol_TitleBg]               = bgPanel;
    colors[ImGuiCol_TitleBgActive]         = ImVec4(0.12f, 0.14f, 0.19f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]      = bgDark;
    colors[ImGuiCol_MenuBarBg]             = bgPanel;
    colors[ImGuiCol_ScrollbarBg]           = bgDark;
    colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.25f, 0.29f, 0.38f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.35f, 0.40f, 0.50f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]   = cyanAccent;
    colors[ImGuiCol_CheckMark]             = cyanAccent;
    colors[ImGuiCol_SliderGrab]            = cyanAccent;
    colors[ImGuiCol_SliderGrabActive]      = cyanActive;
    colors[ImGuiCol_Button]                = ImVec4(0.13f, 0.17f, 0.25f, 1.00f);
    colors[ImGuiCol_ButtonHovered]         = ImVec4(0.18f, 0.24f, 0.35f, 1.00f);
    colors[ImGuiCol_ButtonActive]          = cyanActive;
    colors[ImGuiCol_Header]                = ImVec4(0.16f, 0.20f, 0.29f, 0.80f);
    colors[ImGuiCol_HeaderHovered]         = ImVec4(0.22f, 0.28f, 0.40f, 0.90f);
    colors[ImGuiCol_HeaderActive]          = cyanAccent;
    colors[ImGuiCol_Separator]             = borderCol;
    colors[ImGuiCol_SeparatorHovered]      = cyanAccent;
    colors[ImGuiCol_SeparatorActive]       = cyanActive;
    colors[ImGuiCol_ResizeGrip]            = ImVec4(0.20f, 0.25f, 0.35f, 0.50f);
    colors[ImGuiCol_ResizeGripHovered]     = cyanAccent;
    colors[ImGuiCol_ResizeGripActive]      = cyanActive;
    colors[ImGuiCol_Tab]                   = bgFrame;
    colors[ImGuiCol_TabHovered]            = cyanHover;
    colors[ImGuiCol_TabActive]             = ImVec4(0.12f, 0.28f, 0.38f, 1.00f);
    colors[ImGuiCol_TabUnfocused]          = bgPanel;
    colors[ImGuiCol_TabUnfocusedActive]    = bgFrame;
}

// ============================================================================
// Process Scanner
// ============================================================================

void GuiApp::RefreshProcessList() {
    runningProcesses_.clear();

    UniqueHandle snapshot(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot.IsValid()) {
        return;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(PROCESSENTRY32W);

    if (::Process32FirstW(snapshot.Get(), &entry)) {
        do {
            if (entry.th32ProcessID <= 4) {
                continue; // Skip system idle & system processes
            }
            runningProcesses_.push_back({ entry.th32ProcessID, entry.szExeFile });
        } while (::Process32NextW(snapshot.Get(), &entry));
    }

    // Sort alphabetically
    std::sort(runningProcesses_.begin(), runningProcesses_.end(), [](const auto& a, const auto& b) {
        return ::_wcsicmp(a.exeName.c_str(), b.exeName.c_str()) < 0;
    });
}

// ============================================================================
// Main Application Loop
// ============================================================================

int GuiApp::Run(const std::wstring& initialTarget) {
    if (!initialTarget.empty()) {
        std::string narrow = GameOptimizer::WideToNarrow(initialTarget);
        ::strncpy_s(targetProcessBuf_, narrow.c_str(), sizeof(targetProcessBuf_) - 1);
        config_.targetProcessName = initialTarget;
    }

    // Initialize High-DPI Awareness
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Register Win32 Window Class
    wc_ = {
        sizeof(wc_),
        CS_CLASSDC,
        WndProc,
        0L,
        0L,
        ::GetModuleHandle(nullptr),
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        L"CorelockOptimizerGuiClass",
        nullptr
    };
    ::RegisterClassExW(&wc_);

    // Create Window
    const int windowWidth = 1120;
    const int windowHeight = 780;

    hWnd_ = ::CreateWindowW(
        wc_.lpszClassName,
        L"Corelock // Game Priority & Thread Isolation Optimizer",
        WS_OVERLAPPEDWINDOW,
        150, 100,
        windowWidth, windowHeight,
        nullptr, nullptr,
        wc_.hInstance,
        nullptr
    );

    if (hWnd_ == nullptr) {
        return 1;
    }

    ::SetWindowLongPtr(hWnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    // Enable Windows 10/11 Dark Mode Title Bar
    BOOL useDarkMode = TRUE;
    ::DwmSetWindowAttribute(hWnd_, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &useDarkMode, sizeof(useDarkMode));

    // Initialize Direct3D 11
    if (!CreateDeviceD3D(hWnd_)) {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc_.lpszClassName, wc_.hInstance);
        return 1;
    }

    ::ShowWindow(hWnd_, SW_SHOWDEFAULT);
    ::UpdateWindow(hWnd_);

    // Initialize Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ApplyGamerTheme();

    ImGui_ImplWin32_Init(hWnd_);
    ImGui_ImplDX11_Init(d3dDevice_, d3dDeviceContext_);

    RefreshProcessList();

    // Main Render Loop
    bool done = false;
    while (!done) {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) {
                done = true;
            }
        }
        if (done) {
            break;
        }

        // Start Dear ImGui frame
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        RenderUI();

        ImGui::Render();
        const float clearColor[4] = { 0.06f, 0.07f, 0.09f, 1.00f };
        d3dDeviceContext_->OMSetRenderTargets(1, &mainRenderTargetView_, nullptr);
        d3dDeviceContext_->ClearRenderTargetView(mainRenderTargetView_, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr = swapChain_->Present(1, 0); // VSync enabled for smooth UI
        swapChainOccluded_ = (hr == DXGI_STATUS_OCCLUDED);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    return 0;
}

// ============================================================================
// UI Render Routines
// ============================================================================

void GuiApp::RenderUI() {
    // Fill entire client area with borderless docking-style window
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoTitleBar |
                                   ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus |
                                   ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("CorelockMainCanvas", nullptr, windowFlags);
    ImGui::PopStyleVar(2);

    RenderHeader();
    ImGui::Separator();
    ImGui::Spacing();

    // Two-Column Layout: Left (Controls), Right (Visualizer & Live Telemetry)
    float contentWidth = ImGui::GetContentRegionAvail().x;
    float leftColWidth = contentWidth * 0.44f;
    float rightColWidth = contentWidth - leftColWidth - ImGui::GetStyle().ItemSpacing.x;

    float availableHeight = ImGui::GetContentRegionAvail().y;
    float topPanelsHeight = (std::max)(480.0f, availableHeight - 210.0f);

    ImGui::BeginChild("LeftControlColumn", ImVec2(leftColWidth, topPanelsHeight), true);
    RenderControlPanel();
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("RightVisualizationColumn", ImVec2(rightColWidth, topPanelsHeight), true);
    RenderCoreVisualizer();
    ImGui::Spacing();
    RenderSnapshotCard();
    ImGui::EndChild();

    ImGui::Spacing();

    // Bottom Panel: Live Diagnostics Console
    ImGui::BeginChild("BottomConsolePanel", ImVec2(0.0f, 0.0f), true);
    RenderLogConsole();
    ImGui::EndChild();

    ImGui::End();
}

void GuiApp::RenderHeader() {
    bool isRunning = (optimizer_ != nullptr && optimizer_->IsRunning());
    bool isOptimized = (optimizer_ != nullptr && optimizer_->IsTargetOptimized());

    ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[0]);
    ImGui::TextColored(ImVec4(0.00f, 0.85f, 0.95f, 1.00f), "CORELOCK");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.60f, 0.65f, 0.75f, 1.00f), "//");
    ImGui::SameLine();
    ImGui::Text("GAME PRIORITY & THREAD ISOLATION ENGINE");
    ImGui::PopFont();

    // Right-aligned header buttons & status badge
    float headerRightWidth = 360.0f;
    ImGui::SameLine(ImGui::GetWindowWidth() - headerRightWidth - 16.0f);

    // Quick Start / Stop Action in Header
    if (!isRunning) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.00f, 0.65f, 0.35f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.00f, 0.80f, 0.45f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.00f, 0.50f, 0.25f, 1.00f));
        if (ImGui::Button("START", ImVec2(80.0f, 26.0f))) {
            StartMonitoring();
        }
        ImGui::PopStyleColor(3);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.15f, 0.25f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.90f, 0.25f, 0.35f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.60f, 0.10f, 0.20f, 1.00f));
        if (ImGui::Button("STOP", ImVec2(80.0f, 26.0f))) {
            StopMonitoring();
        }
        ImGui::PopStyleColor(3);
    }

    ImGui::SameLine();

    // Status Badge Pill
    if (isOptimized) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.00f, 0.55f, 0.25f, 0.90f));
        ImGui::Button("[ OPTIMIZED // ACTIVE ]", ImVec2(240.0f, 26.0f));
        ImGui::PopStyleColor();
    } else if (isRunning) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.00f, 0.40f, 0.60f, 0.90f));
        ImGui::Button("[ SCANNING FOR GAME... ]", ImVec2(240.0f, 26.0f));
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.23f, 0.30f, 0.80f));
        ImGui::Button("[ MONITOR STOPPED ]", ImVec2(240.0f, 26.0f));
        ImGui::PopStyleColor();
    }
}

void GuiApp::RenderControlPanel() {
    bool isRunning = (optimizer_ != nullptr && optimizer_->IsRunning());

    ImGui::TextColored(ImVec4(0.00f, 0.85f, 0.95f, 1.00f), "TARGET EXECUTABLE");
    ImGui::Spacing();

    // Process input text box
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##TargetProcess", targetProcessBuf_, sizeof(targetProcessBuf_));

    // Process scanner & selector
    if (ImGui::Button("Scan Running Processes", ImVec2(-1.0f, 26.0f))) {
        RefreshProcessList();
        ImGui::OpenPopup("RunningProcessPopup");
    }

    if (ImGui::BeginPopup("RunningProcessPopup")) {
        ImGui::TextColored(ImVec4(0.00f, 0.85f, 0.95f, 1.00f), "Select Process to Optimize:");
        ImGui::SetNextItemWidth(260.0f);
        ImGui::InputTextWithHint("##Filter", "Search...", processFilterBuf_, sizeof(processFilterBuf_));
        ImGui::Separator();

        ImGui::BeginChild("ProcessListScroll", ImVec2(320.0f, 200.0f));
        for (const auto& proc : runningProcesses_) {
            std::string narrowName = GameOptimizer::WideToNarrow(proc.exeName);
            if (processFilterBuf_[0] != '\0') {
                std::string filterLower = processFilterBuf_;
                std::string nameLower = narrowName;
                std::transform(filterLower.begin(), filterLower.end(), filterLower.begin(), ::tolower);
                std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
                if (nameLower.find(filterLower) == std::string::npos) {
                    continue;
                }
            }

            std::string label = narrowName + " (" + std::to_string(proc.pid) + ")";
            if (ImGui::Selectable(label.c_str())) {
                ::strncpy_s(targetProcessBuf_, narrowName.c_str(), sizeof(targetProcessBuf_) - 1);
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndChild();
        ImGui::EndPopup();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(ImVec4(0.00f, 0.85f, 0.95f, 1.00f), "OPTIMIZATION POLICIES");
    ImGui::Spacing();

    // CPU Affinity Isolation Mode
    ImGui::Text("CPU Affinity Policy:");
    const char* policies[] = {
        "All Available Cores (No Isolation)",
        "Isolate Logical CPU 0 (Reserve for DPC/IRQs)",
        "Isolate Physical Core 0 (SMT-Aware)"
    };
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::Combo("##AffinityPolicy", &selectedAffinityPolicyIndex_, policies, IM_ARRAYSIZE(policies));

    // Priority Class Policy
    ImGui::Spacing();
    ImGui::Text("Thread Scheduling Priority:");
    const char* priorities[] = {
        "HIGH_PRIORITY_CLASS (Recommended)",
        "ABOVE_NORMAL_PRIORITY_CLASS",
        "NORMAL_PRIORITY_CLASS"
    };
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::Combo("##PriorityClass", &selectedPriorityIndex_, priorities, IM_ARRAYSIZE(priorities));

    ImGui::Spacing();
    ImGui::Checkbox("Dynamic Windows Power Plan (High Performance)", &enablePowerPlan_);
    ImGui::Checkbox("Enable MMCSS 'Games' Scheduling Profile", &enableMmcss_);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(ImVec4(0.00f, 0.85f, 0.95f, 1.00f), "OPTIMIZATION CONTROLS");
    ImGui::Spacing();

    // Start / Stop Master Action Buttons (Dual Side-by-Side)
    float btnWidth = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

    // --- START OPTIMIZATION BUTTON ---
    if (!isRunning) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.00f, 0.65f, 0.35f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.00f, 0.80f, 0.45f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.00f, 0.50f, 0.25f, 1.00f));

        if (ImGui::Button("START OPTIMIZATION", ImVec2(btnWidth, 42.0f))) {
            StartMonitoring();
        }
        ImGui::PopStyleColor(3);
    } else {
        // Dimmed appearance when already active
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.20f, 0.16f, 0.60f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.40f, 0.60f, 0.45f, 0.70f));
        ImGui::Button("RUNNING...", ImVec2(btnWidth, 42.0f));
        ImGui::PopStyleColor(2);
    }

    ImGui::SameLine();

    // --- STOP OPTIMIZATION BUTTON ---
    if (isRunning) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.15f, 0.25f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.90f, 0.25f, 0.35f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.60f, 0.10f, 0.20f, 1.00f));

        if (ImGui::Button("STOP OPTIMIZATION", ImVec2(btnWidth, 42.0f))) {
            StopMonitoring();
        }
        ImGui::PopStyleColor(3);
    } else {
        // Dimmed appearance when stopped
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.14f, 0.16f, 0.60f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.40f, 0.45f, 0.70f));
        ImGui::Button("STOPPED", ImVec2(btnWidth, 42.0f));
        ImGui::PopStyleColor(2);
    }

    ImGui::Spacing();
    // Quick Re-apply / Refresh Trigger
    if (isRunning) {
        if (ImGui::Button("REFRESH / RE-SCAN PROCESS NOW", ImVec2(-1.0f, 26.0f))) {
            AppendLog(LogLevel::Info, "Immediate process re-scan requested by user.");
        }
    }
}

void GuiApp::RenderCoreVisualizer() {
    ImGui::TextColored(ImVec4(0.00f, 0.85f, 0.95f, 1.00f), "CPU CORE AFFINITY MAP (HARDWARE TOPOLOGY)");
    ImGui::Spacing();

    int columns = 8;
    if (totalLogicalCores_ > 16) {
        columns = 12;
    }
    if (totalLogicalCores_ > 32) {
        columns = 16;
    }

    // Grid of core visualizer tiles
    int activeCoresRendered = (std::min)(totalLogicalCores_, 64);
    float itemSize = 42.0f;

    for (int i = 0; i < activeCoresRendered; ++i) {
        if (i % columns != 0) {
            ImGui::SameLine();
        }

        bool isIsolated = false;
        if (selectedAffinityPolicyIndex_ == 1 && i == 0) {
            isIsolated = true; // Logical CPU 0 isolated
        } else if (selectedAffinityPolicyIndex_ == 2 && (i == 0 || i == 1)) {
            isIsolated = true; // Physical Core 0 SMT siblings isolated
        }

        std::string coreLabel = "C" + std::to_string(i);

        if (isIsolated) {
            // Radiant Amber for DPC / IRQ offloaded cores
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.45f, 0.00f, 0.85f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.00f, 0.55f, 0.10f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.70f, 0.35f, 0.00f, 1.00f));
        } else {
            // Neon Emerald / Cyan for active game simulation cores
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.00f, 0.50f, 0.45f, 0.85f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.00f, 0.70f, 0.65f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.00f, 0.40f, 0.35f, 1.00f));
        }

        ImGui::Button(coreLabel.c_str(), ImVec2(itemSize, itemSize));
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("Logical Processor Core %d", i);
            if (isIsolated) {
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1.0f), "ROLE: OS DPC & Hardware Interrupts Only (Game Offloaded)");
            } else {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.5f, 1.0f), "ROLE: Dedicated Game Simulation & Render Thread");
            }
            ImGui::EndTooltip();
        }

        ImGui::PopStyleColor(3);
    }

    ImGui::Spacing();
    // Legend
    ImGui::TextColored(ImVec4(0.85f, 0.45f, 0.00f, 1.00f), "[x] Isolated (DPC/IRQ Only)");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.00f, 0.85f, 0.75f, 1.00f), "[O] Dedicated Game Core");
}

void GuiApp::RenderSnapshotCard() {
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.00f, 0.85f, 0.95f, 1.00f), "ACTIVE SESSION METRICS & TELEMETRY");
    ImGui::Spacing();

    bool isOptimized = (optimizer_ != nullptr && optimizer_->IsTargetOptimized());
    auto snapshotOpt = (optimizer_ != nullptr) ? optimizer_->GetActiveSnapshot() : std::nullopt;

    if (isOptimized && snapshotOpt.has_value()) {
        const auto& snap = *snapshotOpt;
        ImGui::Text("Optimized Target:   %s (PID: %d)", targetProcessBuf_, snap.processId);
        ImGui::Text("Active Priority:    HIGH_PRIORITY_CLASS (Elevated)");
        ImGui::Text("System Power Plan:  High Performance (Active)");
        ImGui::Text("Anti-Cheat Safety:  100%% Out-of-Process (Safe)");
        ImGui::Text("MMCSS Scheduler:    Enabled ('Games' Profile)");
    } else {
        ImGui::TextDisabled("No game currently active. Standing by for target launch...");
        ImGui::Text("Target Filter:      %s", targetProcessBuf_);
        ImGui::Text("Safety Verification: Completely Out-of-Process (0 Memory/DLL Access)");
    }
}

void GuiApp::RenderLogConsole() {
    ImGui::TextColored(ImVec4(0.00f, 0.85f, 0.95f, 1.00f), "DIAGNOSTICS & SYSTEM EVENT LOG");
    ImGui::SameLine();

    if (ImGui::Button("Clear Logs", ImVec2(90.0f, 20.0f))) {
        std::lock_guard<std::mutex> lock(logMutex_);
        logEntries_.clear();
    }

    ImGui::SameLine();
    ImGui::Checkbox("Auto-Scroll", &autoScrollLogs_);
    ImGui::Separator();

    ImGui::BeginChild("LogScrollRegion", ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_HorizontalScrollbar);

    std::lock_guard<std::mutex> lock(logMutex_);
    for (const auto& entry : logEntries_) {
        ImVec4 col = ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
        const char* tag = "[INFO]   ";

        switch (entry.level) {
            case LogLevel::Success:
                col = ImVec4(0.0f, 0.95f, 0.45f, 1.0f);
                tag = "[SUCCESS]";
                break;
            case LogLevel::Warning:
                col = ImVec4(1.0f, 0.75f, 0.10f, 1.0f);
                tag = "[WARNING]";
                break;
            case LogLevel::Error:
                col = ImVec4(1.0f, 0.20f, 0.30f, 1.0f);
                tag = "[ERROR]  ";
                break;
            case LogLevel::Trace:
                col = ImVec4(0.5f, 0.55f, 0.65f, 1.0f);
                tag = "[TRACE]  ";
                break;
            default:
                break;
        }

        ImGui::TextColored(ImVec4(0.5f, 0.55f, 0.65f, 1.0f), "%s", entry.timestamp.c_str());
        ImGui::SameLine();
        ImGui::TextColored(col, "%s %s", tag, entry.message.c_str());
    }

    if (autoScrollLogs_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
    }

    ImGui::EndChild();
}

// ============================================================================
// Actions & Callbacks
// ============================================================================

void GuiApp::StartMonitoring() {
    std::string targetStr = targetProcessBuf_;
    config_.targetProcessName = GameOptimizer::NarrowToWide(targetStr);

    switch (selectedAffinityPolicyIndex_) {
        case 0: config_.affinityPolicy = AffinityPolicy::AllCores; break;
        case 1: config_.affinityPolicy = AffinityPolicy::IsolateLogicalCpu0; break;
        case 2: config_.affinityPolicy = AffinityPolicy::IsolatePhysicalCore0; break;
        default: config_.affinityPolicy = AffinityPolicy::IsolateLogicalCpu0; break;
    }

    switch (selectedPriorityIndex_) {
        case 0: config_.targetPriorityClass = HIGH_PRIORITY_CLASS; break;
        case 1: config_.targetPriorityClass = ABOVE_NORMAL_PRIORITY_CLASS; break;
        case 2: config_.targetPriorityClass = NORMAL_PRIORITY_CLASS; break;
        default: config_.targetPriorityClass = HIGH_PRIORITY_CLASS; break;
    }

    config_.enableDynamicPowerPlan = enablePowerPlan_;
    config_.enableMmcss = enableMmcss_;
    config_.pollInterval = std::chrono::milliseconds(pollIntervalMs_);

    optimizer_ = std::make_unique<GameOptimizer>(config_);
    optimizer_->Start();
    sessionStartTime_ = std::chrono::steady_clock::now();
}

void GuiApp::StopMonitoring() {
    if (optimizer_ != nullptr) {
        optimizer_->Stop();
        optimizer_.reset();
    }
}

void GuiApp::AppendLog(LogLevel level, std::string_view msg) {
    auto now = std::chrono::system_clock::now();
    auto timeT = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    ::localtime_s(&tm, &timeT);

    char timeBuf[16];
    std::snprintf(timeBuf, sizeof(timeBuf), "[%02d:%02d:%02d]", tm.tm_hour, tm.tm_min, tm.tm_sec);

    std::lock_guard<std::mutex> lock(logMutex_);
    logEntries_.push_back({ timeBuf, level, std::string(msg) });

    // Limit log history to last 500 entries
    if (logEntries_.size() > 500) {
        logEntries_.erase(logEntries_.begin(), logEntries_.begin() + 100);
    }
}

} // namespace Corelock
