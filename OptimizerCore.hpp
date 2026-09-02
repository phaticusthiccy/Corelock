#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <tlhelp32.h>
#include <powrprof.h>
#include <avrt.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// Required native Windows libraries
#pragma comment(lib, "Powrprof.lib")
#pragma comment(lib, "Avrt.lib")

namespace Corelock {

// ============================================================================
// GUID Definitions & Constants
// ============================================================================

// Windows High Performance Power Scheme: {8c5e7fda-e8bf-4a96-9a85-a6e231886e4c}
inline constexpr GUID HighPerformanceSchemeGuid = {
    0x8c5e7fda, 0xe8bf, 0x4a96, { 0x9a, 0x85, 0xa6, 0xe2, 0x31, 0x88, 0x6e, 0x4c }
};

// ============================================================================
// RAII Win32 Resource Wrappers
// ============================================================================

/**
 * @brief Move-only RAII wrapper for native Win32 HANDLE types.
 *
 * Correctly distinguishes between NULL and INVALID_HANDLE_VALUE, ensuring
 * that handles produced by OpenProcess, CreateToolhelp32Snapshot, and CreateEvent
 * are closed reliably without resource leaks.
 */
class UniqueHandle {
public:
    constexpr UniqueHandle() noexcept : handle_(nullptr) {}

    explicit UniqueHandle(HANDLE h) noexcept : handle_(h) {}

    ~UniqueHandle() noexcept {
        Reset();
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.Release()) {}

    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            Reset(other.Release());
        }
        return *this;
    }

    [[nodiscard]] HANDLE Get() const noexcept {
        return handle_;
    }

    [[nodiscard]] bool IsValid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    explicit operator bool() const noexcept {
        return IsValid();
    }

    HANDLE Release() noexcept {
        HANDLE temp = handle_;
        handle_ = nullptr;
        return temp;
    }

    void Reset(HANDLE newHandle = nullptr) noexcept {
        if (IsValid()) {
            ::CloseHandle(handle_);
        }
        handle_ = newHandle;
    }

    [[nodiscard]] HANDLE* Put() noexcept {
        Reset();
        return &handle_;
    }

private:
    HANDLE handle_{ nullptr };
};

/**
 * @brief RAII wrapper for MMCSS task registration handles.
 *
 * Automatically calls AvRevertMmThreadCharacteristics upon destruction.
 */
class UniqueMmcssTask {
public:
    constexpr UniqueMmcssTask() noexcept : taskHandle_(nullptr) {}

    explicit UniqueMmcssTask(HANDLE h) noexcept : taskHandle_(h) {}

    ~UniqueMmcssTask() noexcept {
        Reset();
    }

    UniqueMmcssTask(const UniqueMmcssTask&) = delete;
    UniqueMmcssTask& operator=(const UniqueMmcssTask&) = delete;

    UniqueMmcssTask(UniqueMmcssTask&& other) noexcept : taskHandle_(other.Release()) {}

    UniqueMmcssTask& operator=(UniqueMmcssTask&& other) noexcept {
        if (this != &other) {
            Reset(other.Release());
        }
        return *this;
    }

    [[nodiscard]] HANDLE Get() const noexcept {
        return taskHandle_;
    }

    [[nodiscard]] bool IsValid() const noexcept {
        return taskHandle_ != nullptr;
    }

    explicit operator bool() const noexcept {
        return IsValid();
    }

    HANDLE Release() noexcept {
        HANDLE temp = taskHandle_;
        taskHandle_ = nullptr;
        return temp;
    }

    void Reset(HANDLE newHandle = nullptr) noexcept {
        if (taskHandle_ != nullptr) {
            ::AvRevertMmThreadCharacteristics(taskHandle_);
            taskHandle_ = nullptr;
        }
        taskHandle_ = newHandle;
    }

private:
    HANDLE taskHandle_{ nullptr };
};

// ============================================================================
// Configuration & State Data Structures
// ============================================================================

enum class LogLevel {
    Trace,
    Info,
    Success,
    Warning,
    Error
};

/**
 * @brief CPU affinity policy for game thread isolation.
 */
enum class AffinityPolicy {
    /// Use all available system cores (no isolation)
    AllCores,

    /// Mask out logical CPU 0 (frees CPU 0 for OS DPCs, interrupts, network drivers)
    IsolateLogicalCpu0,

    /// Detect and mask out all logical threads of physical core 0 (SMT/HyperThreading aware)
    IsolatePhysicalCore0,

    /// Apply explicit user-defined affinity bitmask
    CustomMask
};

/**
 * @brief Configuration parameters for the GameOptimizer.
 */
struct OptimizerConfig {
    /// Name of the executable file to monitor (e.g. L"game.exe" or L"cs2.exe")
    std::wstring targetProcessName{ L"game.exe" };

    /// Target priority class (defaults to HIGH_PRIORITY_CLASS; never REALTIME)
    DWORD targetPriorityClass{ HIGH_PRIORITY_CLASS };

    /// CPU affinity policy
    AffinityPolicy affinityPolicy{ AffinityPolicy::IsolateLogicalCpu0 };

    /// User-supplied bitmask (only evaluated if affinityPolicy == AffinityPolicy::CustomMask)
    DWORD_PTR customAffinityMask{ 0 };

    /// Dynamically elevate Windows power plan to High Performance during gameplay
    bool enableDynamicPowerPlan{ true };

    /// Enable Multimedia Class Scheduler Service (MMCSS) scheduling
    bool enableMmcss{ true };

    /// Polling frequency while waiting for target process creation
    std::chrono::milliseconds pollInterval{ 1000 };

    /// Custom logger callback; if nullptr, logs are emitted to std::cout / std::cerr
    std::function<void(LogLevel, std::string_view)> logger{ nullptr };
};

/**
 * @brief Snapshot of the target process and system state captured prior to optimization.
 *
 * Guarantees zero side-effects by enabling strict, fault-tolerant restoration upon
 * game termination or optimizer shutdown.
 */
struct ProcessStateSnapshot {
    DWORD processId{ 0 };
    DWORD originalPriorityClass{ 0 };
    DWORD_PTR originalProcessAffinity{ 0 };
    DWORD_PTR systemAffinity{ 0 };
    GUID originalPowerScheme{ 0 };

    // Mutation tracking flags for safe rollback
    bool powerSchemeCaptured{ false };
    bool priorityElevated{ false };
    bool affinityModified{ false };
    bool powerSchemeSwitched{ false };
    bool mmcssActivated{ false };
};

// ============================================================================
// Core Optimizer Class
// ============================================================================

/**
 * @brief Out-of-process optimizer for Windows thread scheduling, core affinity, and power states.
 *
 * Operates completely externally without DLL injection, code modification, or memory inspection.
 * Zero risk of triggering anti-cheat systems (BattlEye, EasyAntiCheat, Vanguard, Ricochet, VAC).
 */
class GameOptimizer {
public:
    explicit GameOptimizer(OptimizerConfig config);
    ~GameOptimizer();

    // Move-only, non-copyable
    GameOptimizer(const GameOptimizer&) = delete;
    GameOptimizer& operator=(const GameOptimizer&) = delete;
    GameOptimizer(GameOptimizer&&) noexcept = default;
    GameOptimizer& operator=(GameOptimizer&&) noexcept = default;

    /**
     * @brief Launches the background lifecycle monitor thread.
     * @return true if started successfully; false if already running.
     */
    bool Start();

    /**
     * @brief Signals the monitor thread to stop, restores original system states, and joins.
     */
    void Stop();

    /**
     * @brief Checks if the optimizer worker thread is currently active.
     */
    [[nodiscard]] bool IsRunning() const noexcept;

    /**
     * @brief Checks if the target process is currently active and optimized.
     */
    [[nodiscard]] bool IsTargetOptimized() const noexcept;

    /**
     * @brief Retrieves a copy of the active state snapshot, if any.
     */
    [[nodiscard]] std::optional<ProcessStateSnapshot> GetActiveSnapshot() const;

    /**
     * @brief Utility: Formats Win32 GetLastError() into a human-readable string.
     */
    static std::string FormatWin32Error(DWORD errorCode);

    /**
     * @brief Utility: Formats a GUID into standard registry string format: {XXXXXXXX-XXXX-XXXX-...}.
     */
    static std::string GuidToString(const GUID& guid);

    /**
     * @brief Utility: Formats a CPU affinity mask into hexadecimal and binary representation.
     */
    static std::string AffinityMaskToString(DWORD_PTR mask);

private:
    void WorkerThread(std::stop_token stopToken);

    std::optional<DWORD> FindTargetProcessId(std::wstring_view processName);

    bool OptimizeProcess(HANDLE hProcess, DWORD pid, ProcessStateSnapshot& snapshot);

    void RestoreProcessState(HANDLE hProcess, ProcessStateSnapshot& snapshot, bool processIsAlive);

    DWORD_PTR ComputeTargetAffinity(DWORD_PTR currentAffinity, DWORD_PTR systemAffinity);

    DWORD_PTR GetPhysicalCore0Mask(DWORD_PTR systemAffinity);

    void Log(LogLevel level, std::string_view message) const;

private:
    OptimizerConfig config_;
    std::jthread workerThread_;
    mutable std::mutex stateMutex_;

    std::atomic<bool> isRunning_{ false };
    std::atomic<bool> isOptimized_{ false };
    std::optional<ProcessStateSnapshot> activeSnapshot_;

    // MMCSS task handle for thread priority boosting
    UniqueMmcssTask mmcssTask_;
};

} // namespace Corelock
