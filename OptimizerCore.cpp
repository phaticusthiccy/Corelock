#include "OptimizerCore.hpp"

#include <iostream>
#include <sstream>
#include <iomanip>
#include <bitset>
#include <algorithm>

namespace Corelock {

// ============================================================================
// Constructor & Destructor
// ============================================================================

GameOptimizer::GameOptimizer(OptimizerConfig config)
    : config_(std::move(config)) {
}

GameOptimizer::~GameOptimizer() {
    Stop();
}

// ============================================================================
// Lifecycle Management
// ============================================================================

bool GameOptimizer::Start() {
    if (isRunning_.load()) {
        Log(LogLevel::Warning, "Start() called, but optimizer is already running.");
        return false;
    }

    isRunning_.store(true);
    workerThread_ = std::jthread([this](std::stop_token token) {
        WorkerThread(token);
    });

    Log(LogLevel::Info, "Optimizer background monitor started successfully.");
    return true;
}

void GameOptimizer::Stop() {
    if (!isRunning_.load()) {
        return;
    }

    Log(LogLevel::Info, "Stopping optimizer background monitor...");

    if (workerThread_.joinable()) {
        workerThread_.request_stop();
        workerThread_.join();
    }

    isRunning_.store(false);
    Log(LogLevel::Info, "Optimizer stopped.");
}

bool GameOptimizer::IsRunning() const noexcept {
    return isRunning_.load();
}

bool GameOptimizer::IsTargetOptimized() const noexcept {
    return isOptimized_.load();
}

std::optional<ProcessStateSnapshot> GameOptimizer::GetActiveSnapshot() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return activeSnapshot_;
}

// ============================================================================
// Worker Thread Loop (Asynchronous Zero-CPU Monitoring)
// ============================================================================

void GameOptimizer::WorkerThread(std::stop_token stopToken) {
    // Create manual-reset stop event for zero-CPU asynchronous interruptible waits
    UniqueHandle stopEvent(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!stopEvent.IsValid()) {
        Log(LogLevel::Error, "Failed to create Win32 stop event. Error: " + FormatWin32Error(::GetLastError()));
        isRunning_.store(false);
        return;
    }

    // Connect stopToken to stopEvent: instant signal on request_stop()
    std::stop_callback stopCallback(stopToken, [&stopEvent]() {
        ::SetEvent(stopEvent.Get());
    });

    std::string targetNameNarrow(config_.targetProcessName.begin(), config_.targetProcessName.end());
    Log(LogLevel::Info, "Monitoring process lifecycle for: '" + targetNameNarrow + "'");

    while (!stopToken.stop_requested()) {
        auto maybePid = FindTargetProcessId(config_.targetProcessName);

        if (!maybePid.has_value()) {
            // Target not running: sleep for pollInterval via waitable handle with 0% CPU consumption
            DWORD waitResult = ::WaitForSingleObject(stopEvent.Get(), static_cast<DWORD>(config_.pollInterval.count()));
            if (waitResult == WAIT_OBJECT_0) {
                // Stop requested during sleep
                break;
            }
            continue;
        }

        DWORD pid = *maybePid;
        Log(LogLevel::Info, "Target process detected: '" + targetNameNarrow + "' (PID: " + std::to_string(pid) + ")");

        // Acquire process handle with minimum required access rights
        // PROCESS_SET_INFORMATION: SetPriorityClass, SetProcessAffinityMask
        // PROCESS_QUERY_LIMITED_INFORMATION: GetPriorityClass, GetProcessAffinityMask
        // SYNCHRONIZE: WaitForSingleObject / WaitForMultipleObjects
        DWORD desiredAccess = PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE;
        UniqueHandle hProcess(::OpenProcess(desiredAccess, FALSE, pid));

        if (!hProcess.IsValid()) {
            DWORD err = ::GetLastError();
            Log(LogLevel::Error, "Failed to open process PID " + std::to_string(pid) + 
                                 ". Error: " + FormatWin32Error(err) + 
                                 " (Elevation / Administrator rights may be required if the game is running as Admin).");

            // Wait before retrying to prevent busy-looping if permissions fail
            DWORD waitResult = ::WaitForSingleObject(stopEvent.Get(), static_cast<DWORD>(config_.pollInterval.count()));
            if (waitResult == WAIT_OBJECT_0) {
                break;
            }
            continue;
        }

        // Initialize snapshot for this session
        ProcessStateSnapshot snapshot{};
        snapshot.processId = pid;

        if (OptimizeProcess(hProcess.Get(), pid, snapshot)) {
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                activeSnapshot_ = snapshot;
            }
            isOptimized_.store(true);
            Log(LogLevel::Success, "Target process successfully optimized! Entering zero-CPU monitoring state.");

            // Wait on both the stop event AND the process exit handle simultaneously.
            // 0% CPU consumption while the game runs. Sub-millisecond reaction when the game exits or stop is requested.
            HANDLE waitHandles[2] = { stopEvent.Get(), hProcess.Get() };
            DWORD waitResult = ::WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);

            if (waitResult == WAIT_OBJECT_0) {
                // Stop requested while game is still actively running
                Log(LogLevel::Info, "Optimizer shutdown requested while game is still running. Reverting states...");
                RestoreProcessState(hProcess.Get(), snapshot, /*processIsAlive=*/true);
                {
                    std::lock_guard<std::mutex> lock(stateMutex_);
                    activeSnapshot_.reset();
                }
                isOptimized_.store(false);
                break;
            } else if (waitResult == WAIT_OBJECT_0 + 1) {
                // Target process terminated
                Log(LogLevel::Info, "Target process (PID: " + std::to_string(pid) + ") terminated. Executing fault-tolerant restoration...");
                RestoreProcessState(hProcess.Get(), snapshot, /*processIsAlive=*/false);
                {
                    std::lock_guard<std::mutex> lock(stateMutex_);
                    activeSnapshot_.reset();
                }
                isOptimized_.store(false);
                Log(LogLevel::Info, "Restoration complete. Resuming monitoring for next launch...");
            } else {
                Log(LogLevel::Error, "Unexpected wait result in WaitForMultipleObjects: " + std::to_string(waitResult));
                break;
            }
        } else {
            Log(LogLevel::Warning, "Optimization phase failed. Retrying in next polling cycle.");
            DWORD waitResult = ::WaitForSingleObject(stopEvent.Get(), static_cast<DWORD>(config_.pollInterval.count()));
            if (waitResult == WAIT_OBJECT_0) {
                break;
            }
        }
    }

    isRunning_.store(false);
    Log(LogLevel::Info, "Worker thread terminated cleanly.");
}

// ============================================================================
// Process Discovery via Toolhelp32 Snapshot
// ============================================================================

std::optional<DWORD> GameOptimizer::FindTargetProcessId(std::wstring_view processName) {
    UniqueHandle snapshot(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot.IsValid()) {
        return std::nullopt;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(PROCESSENTRY32W);

    if (!::Process32FirstW(snapshot.Get(), &entry)) {
        return std::nullopt;
    }

    do {
        // Case-insensitive comparison between executable file names
        if (::_wcsicmp(entry.szExeFile, processName.data()) == 0) {
            return entry.th32ProcessID;
        }
    } while (::Process32NextW(snapshot.Get(), &entry));

    return std::nullopt;
}

// ============================================================================
// Core Optimization Actions
// ============================================================================

bool GameOptimizer::OptimizeProcess(HANDLE hProcess, DWORD pid, ProcessStateSnapshot& snapshot) {
    Log(LogLevel::Info, "--- Capturing Pre-Optimization State Snapshot ---");

    // 1. Capture Original Priority Class
    DWORD originalPriority = ::GetPriorityClass(hProcess);
    if (originalPriority == 0) {
        DWORD err = ::GetLastError();
        Log(LogLevel::Error, "Failed to query priority class for PID " + std::to_string(pid) + ". Error: " + FormatWin32Error(err));
        return false;
    }
    snapshot.originalPriorityClass = originalPriority;
    Log(LogLevel::Info, "Original Priority Class: 0x" + std::to_string(originalPriority));

    // 2. Capture Original Process and System Affinity Masks
    DWORD_PTR procAffinity = 0;
    DWORD_PTR sysAffinity = 0;
    if (!::GetProcessAffinityMask(hProcess, &procAffinity, &sysAffinity)) {
        DWORD err = ::GetLastError();
        Log(LogLevel::Error, "Failed to query affinity mask for PID " + std::to_string(pid) + ". Error: " + FormatWin32Error(err));
        return false;
    }
    snapshot.originalProcessAffinity = procAffinity;
    snapshot.systemAffinity = sysAffinity;
    Log(LogLevel::Info, "Original Process Affinity: " + AffinityMaskToString(procAffinity));
    Log(LogLevel::Info, "System Available Affinity: " + AffinityMaskToString(sysAffinity));

    // 3. Capture and Switch System Power Plan
    if (config_.enableDynamicPowerPlan) {
        GUID* pActiveScheme = nullptr;
        DWORD pwrStatus = ::PowerGetActiveScheme(nullptr, &pActiveScheme);
        if (pwrStatus == ERROR_SUCCESS && pActiveScheme != nullptr) {
            snapshot.originalPowerScheme = *pActiveScheme;
            snapshot.powerSchemeCaptured = true;
            Log(LogLevel::Info, "Original Active Power Scheme: " + GuidToString(*pActiveScheme));
            ::LocalFree(pActiveScheme);

            // Switch to High Performance Scheme
            DWORD switchStatus = ::PowerSetActiveScheme(nullptr, &HighPerformanceSchemeGuid);
            if (switchStatus == ERROR_SUCCESS) {
                snapshot.powerSchemeSwitched = true;
                Log(LogLevel::Success, "Dynamic Power Plan: Switched to High Performance (" + GuidToString(HighPerformanceSchemeGuid) + ")");
            } else {
                Log(LogLevel::Warning, "Dynamic Power Plan: Failed to set High Performance scheme. Error: " + FormatWin32Error(switchStatus));
            }
        } else {
            Log(LogLevel::Warning, "Dynamic Power Plan: Could not query active power scheme. Error: " + FormatWin32Error(pwrStatus));
        }
    }

    // 4. Multimedia Class Scheduler Service (MMCSS) Integration
    if (config_.enableMmcss) {
        DWORD taskIndex = 0;
        HANDLE hMmcss = ::AvSetMmThreadCharacteristicsW(L"Games", &taskIndex);
        if (hMmcss != nullptr) {
            mmcssTask_.Reset(hMmcss);
            snapshot.mmcssActivated = true;
            Log(LogLevel::Success, "MMCSS: Associated monitoring thread with 'Games' profile (Task Index: " + std::to_string(taskIndex) + ")");
        } else {
            DWORD err = ::GetLastError();
            Log(LogLevel::Warning, "MMCSS: AvSetMmThreadCharacteristicsW failed. Error: " + FormatWin32Error(err));
        }
    }

    Log(LogLevel::Info, "--- Applying Process Optimizations ---");

    // 5. Elevate Priority Class (HIGH_PRIORITY_CLASS; avoid REALTIME)
    if (config_.targetPriorityClass != snapshot.originalPriorityClass) {
        if (::SetPriorityClass(hProcess, config_.targetPriorityClass)) {
            snapshot.priorityElevated = true;
            Log(LogLevel::Success, "Process Priority: Elevated to HIGH_PRIORITY_CLASS (0x00000080)");
        } else {
            DWORD err = ::GetLastError();
            Log(LogLevel::Error, "Process Priority: Failed to elevate priority class. Error: " + FormatWin32Error(err));
        }
    } else {
        Log(LogLevel::Info, "Process Priority: Target already configured with requested priority class.");
    }

    // 6. Calculate and Apply CPU Affinity Mask (Core Isolation)
    DWORD_PTR targetAffinity = ComputeTargetAffinity(procAffinity, sysAffinity);
    if (targetAffinity != procAffinity) {
        if (::SetProcessAffinityMask(hProcess, targetAffinity)) {
            snapshot.affinityModified = true;
            Log(LogLevel::Success, "CPU Affinity Isolation: Applied new mask -> " + AffinityMaskToString(targetAffinity));
        } else {
            DWORD err = ::GetLastError();
            Log(LogLevel::Error, "CPU Affinity Isolation: Failed to set mask. Error: " + FormatWin32Error(err));
        }
    } else {
        Log(LogLevel::Info, "CPU Affinity Isolation: Target process already running with desired affinity mask.");
    }

    return true;
}

// ============================================================================
// State Restoration Machine (Fault-Tolerant Rollback)
// ============================================================================

void GameOptimizer::RestoreProcessState(HANDLE hProcess, ProcessStateSnapshot& snapshot, bool processIsAlive) {
    Log(LogLevel::Info, "--- Initiating State Rollback ---");

    // Revert Process Priority and Affinity if process is still running
    if (processIsAlive && hProcess != nullptr) {
        if (snapshot.priorityElevated) {
            if (::SetPriorityClass(hProcess, snapshot.originalPriorityClass)) {
                Log(LogLevel::Info, "Restored original process priority class (0x" + std::to_string(snapshot.originalPriorityClass) + ").");
            } else {
                Log(LogLevel::Warning, "Failed to restore process priority. Error: " + FormatWin32Error(::GetLastError()));
            }
            snapshot.priorityElevated = false;
        }

        if (snapshot.affinityModified) {
            if (::SetProcessAffinityMask(hProcess, snapshot.originalProcessAffinity)) {
                Log(LogLevel::Info, "Restored original process affinity mask (" + AffinityMaskToString(snapshot.originalProcessAffinity) + ").");
            } else {
                Log(LogLevel::Warning, "Failed to restore process affinity mask. Error: " + FormatWin32Error(::GetLastError()));
            }
            snapshot.affinityModified = false;
        }
    } else {
        Log(LogLevel::Info, "Target process has exited; kernel automatically reclaimed process priority and affinity.");
    }

    // Revert System Power Scheme
    if (snapshot.powerSchemeSwitched && snapshot.powerSchemeCaptured) {
        DWORD status = ::PowerSetActiveScheme(nullptr, &snapshot.originalPowerScheme);
        if (status == ERROR_SUCCESS) {
            Log(LogLevel::Success, "Restored original Windows power plan: " + GuidToString(snapshot.originalPowerScheme));
            snapshot.powerSchemeSwitched = false;
        } else {
            Log(LogLevel::Warning, "Failed to revert power plan. Error: " + FormatWin32Error(status));
        }
    }

    // Revert MMCSS Task Characteristics
    if (snapshot.mmcssActivated) {
        mmcssTask_.Reset();
        snapshot.mmcssActivated = false;
        Log(LogLevel::Info, "Reverted MMCSS scheduling characteristics.");
    }
}

// ============================================================================
// Affinity Mask Computation & Topology Awareness
// ============================================================================

DWORD_PTR GameOptimizer::ComputeTargetAffinity(DWORD_PTR currentAffinity, DWORD_PTR systemAffinity) {
    switch (config_.affinityPolicy) {
        case AffinityPolicy::AllCores:
            return systemAffinity;

        case AffinityPolicy::IsolateLogicalCpu0: {
            // Mask out bit 0 (Logical Core 0)
            DWORD_PTR isolated = systemAffinity & ~static_cast<DWORD_PTR>(1);
            if (isolated != 0) {
                return isolated;
            }
            // Fallback to system affinity if system only has 1 core
            Log(LogLevel::Warning, "Single-core CPU detected; cannot isolate CPU 0. Falling back to all cores.");
            return systemAffinity;
        }

        case AffinityPolicy::IsolatePhysicalCore0: {
            // Detect and mask out all logical threads sharing Physical Core 0
            DWORD_PTR core0Mask = GetPhysicalCore0Mask(systemAffinity);
            DWORD_PTR isolated = systemAffinity & ~core0Mask;
            if (isolated != 0) {
                return isolated;
            }
            Log(LogLevel::Warning, "Insufficient physical cores to isolate Core 0. Falling back to Logical CPU 0 isolation.");
            DWORD_PTR fallback = systemAffinity & ~static_cast<DWORD_PTR>(1);
            return (fallback != 0) ? fallback : systemAffinity;
        }

        case AffinityPolicy::CustomMask: {
            // Ensure custom mask only selects cores present on the system
            DWORD_PTR sanitized = config_.customAffinityMask & systemAffinity;
            if (sanitized != 0) {
                return sanitized;
            }
            Log(LogLevel::Warning, "Custom affinity mask (0x" + AffinityMaskToString(config_.customAffinityMask) + 
                                 ") contains no active system cores. Falling back to system affinity.");
            return systemAffinity;
        }

        default:
            return systemAffinity;
    }
}

DWORD_PTR GameOptimizer::GetPhysicalCore0Mask(DWORD_PTR systemAffinity) {
    DWORD bufferSize = 0;
    // Determine required buffer size for logical processor relationship info
    ::GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bufferSize);
    if (bufferSize == 0) {
        // Fallback: bit 0 only
        return static_cast<DWORD_PTR>(1);
    }

    std::vector<uint8_t> buffer(bufferSize);
    auto* pInfo = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data());

    if (!::GetLogicalProcessorInformationEx(RelationProcessorCore, pInfo, &bufferSize)) {
        return static_cast<DWORD_PTR>(1);
    }

    DWORD offset = 0;
    while (offset < bufferSize) {
        auto* current = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data() + offset);
        if (current->Relationship == RelationProcessorCore) {
            // Examine group masks for this physical core
            for (WORD g = 0; g < current->Processor.GroupCount; ++g) {
                const auto& groupMask = current->Processor.GroupMask[g];
                // In group 0, check if this physical core owns logical core 0 (bit 0)
                if (groupMask.Group == 0 && (groupMask.Mask & 1ULL) != 0) {
                    return static_cast<DWORD_PTR>(groupMask.Mask) & systemAffinity;
                }
            }
        }
        offset += current->Size;
    }

    // Default fallback: bit 0
    return static_cast<DWORD_PTR>(1);
}

// ============================================================================
// Utilities & Logging
// ============================================================================

std::string GameOptimizer::FormatWin32Error(DWORD errorCode) {
    if (errorCode == 0) {
        return "Operation completed successfully (0).";
    }

    LPSTR messageBuffer = nullptr;
    DWORD size = ::FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        errorCode,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&messageBuffer),
        0,
        nullptr
    );

    std::string result;
    if (size > 0 && messageBuffer != nullptr) {
        result = messageBuffer;
        // Trim trailing newlines
        while (!result.empty() && (result.back() == '\r' || result.back() == '\n')) {
            result.pop_back();
        }
        ::LocalFree(messageBuffer);
    } else {
        result = "Unknown Win32 error";
    }

    std::ostringstream ss;
    ss << result << " (Error Code: " << errorCode << " / 0x" << std::hex << errorCode << ")";
    return ss.str();
}

std::string GameOptimizer::GuidToString(const GUID& guid) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer),
                  "{%08lX-%04hX-%04hX-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                  guid.Data1, guid.Data2, guid.Data3,
                  guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
                  guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    return std::string(buffer);
}

std::string GameOptimizer::AffinityMaskToString(DWORD_PTR mask) {
    std::ostringstream ss;
    ss << "0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(sizeof(DWORD_PTR) * 2) << mask;

    // Generate active core list representation, e.g. [1, 2, 3, 4, 5, 6, 7]
    std::vector<int> activeCores;
    for (size_t i = 0; i < sizeof(DWORD_PTR) * 8; ++i) {
        if ((mask & (static_cast<DWORD_PTR>(1) << i)) != 0) {
            activeCores.push_back(static_cast<int>(i));
        }
    }

    ss << " (Cores: ";
    if (activeCores.empty()) {
        ss << "None";
    } else {
        for (size_t i = 0; i < activeCores.size(); ++i) {
            ss << activeCores[i];
            if (i + 1 < activeCores.size()) {
                ss << ", ";
            }
        }
    }
    ss << ")";

    return ss.str();
}

void GameOptimizer::Log(LogLevel level, std::string_view message) const {
    if (config_.logger) {
        config_.logger(level, message);
        return;
    }

    // Default console logger with timestamps and level tags
    auto now = std::chrono::system_clock::now();
    auto timeT = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    ::localtime_s(&tm, &timeT);

    std::ostringstream prefix;
    prefix << "[" << std::setfill('0') << std::setw(2) << tm.tm_hour << ":"
           << std::setfill('0') << std::setw(2) << tm.tm_min << ":"
           << std::setfill('0') << std::setw(2) << tm.tm_sec << "] ";

    switch (level) {
        case LogLevel::Trace:   prefix << "[TRACE]   "; break;
        case LogLevel::Info:    prefix << "[INFO]    "; break;
        case LogLevel::Success: prefix << "[SUCCESS] "; break;
        case LogLevel::Warning: prefix << "[WARNING] "; break;
        case LogLevel::Error:   prefix << "[ERROR]   "; break;
    }

    if (level == LogLevel::Error) {
        std::cerr << prefix.str() << message << std::endl;
    } else {
        std::cout << prefix.str() << message << std::endl;
    }
}

} // namespace Corelock
