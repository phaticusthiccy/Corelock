#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include <chrono>
#include <optional>

#include "OptimizerCore.hpp"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")

namespace Corelock {

struct UiLogEntry {
    std::string timestamp;
    LogLevel level;
    std::string message;
};

struct RunningProcessItem {
    DWORD pid;
    std::wstring exeName;
};

/**
 * @brief Standalone DirectX 11 & Dear ImGui Graphical User Interface for Corelock.
 *
 * Features:
 * - Cyberpunk/titanium gamer aesthetic with dark mode and neon accents.
 * - Interactive CPU Core Affinity visualizer grid.
 * - Running process auto-discovery dropdown scanner.
 * - Live real-time diagnostics console with color-coded level filters.
 * - Hardware power state, MMCSS, and thread priority controls.
 */
class GuiApp {
public:
    GuiApp();
    ~GuiApp();

    GuiApp(const GuiApp&) = delete;
    GuiApp& operator=(const GuiApp&) = delete;

    /**
     * @brief Initializes Win32 window, DirectX 11 device/swapchain, ImGui context, and starts the render loop.
     * @param initialTarget Optional initial target executable name.
     * @return Exit code for Win32 application.
     */
    int Run(const std::wstring& initialTarget = L"game.exe");

    // Static window procedure for Win32 message dispatch
    static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    bool CreateDeviceD3D(HWND hWnd);
    void CleanupDeviceD3D();
    void CreateRenderTarget();
    void CleanupRenderTarget();

    void ApplyGamerTheme();
    void RefreshProcessList();
    void RenderUI();

    void RenderHeader();
    void RenderControlPanel();
    void RenderCoreVisualizer();
    void RenderSnapshotCard();
    void RenderLogConsole();

    void StartMonitoring();
    void StopMonitoring();
    void TriggerReScan();

    void BrowseForExecutable();
    void LoadConfigFromDisk();
    void SaveConfigToDisk();

    void AppendLog(LogLevel level, std::string_view msg);

private:
    HWND hWnd_{ nullptr };
    WNDCLASSEXW wc_{};

    ID3D11Device* d3dDevice_{ nullptr };
    ID3D11DeviceContext* d3dDeviceContext_{ nullptr };
    IDXGISwapChain* swapChain_{ nullptr };
    ID3D11RenderTargetView* mainRenderTargetView_{ nullptr };
    bool swapChainOccluded_{ false };

    // Optimizer instance & configuration
    std::unique_ptr<GameOptimizer> optimizer_;
    OptimizerConfig config_;

    // UI State
    char targetProcessBuf_[128]{ "game.exe" };
    int selectedAffinityPolicyIndex_{ 1 }; // Default: IsolateLogicalCpu0
    int selectedPriorityIndex_{ 0 };       // Default: HIGH_PRIORITY_CLASS
    bool enablePowerPlan_{ true };
    bool enableMmcss_{ true };
    bool enableHighResolutionTimer_{ true };
    int pollIntervalMs_{ 800 };

    // CPU Topology information
    int totalLogicalCores_{ 0 };
    DWORD_PTR systemAffinityMask_{ 0 };
    DWORD_PTR physicalCore0Mask_{ 1 };
    DWORD_PTR performanceCoresMask_{ 0 };
    bool hasHybridArchitecture_{ false };
    DWORD_PTR customAffinityMask_{ 0 };

    // Running process picker
    std::vector<RunningProcessItem> runningProcesses_;
    int selectedProcessIndex_{ -1 };
    char processFilterBuf_[64]{ "" };

    // Thread-safe log collection
    mutable std::mutex logMutex_;
    std::vector<UiLogEntry> logEntries_;
    bool autoScrollLogs_{ true };
    LogLevel filterLogLevel_{ LogLevel::Trace };

    // Session tracking
    std::chrono::steady_clock::time_point sessionStartTime_{};
};

} // namespace Corelock
