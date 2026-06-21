#pragma once

// ButterflyBlades 公共类型定义
// 跨平台统一类型抽象

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <chrono>
#include <atomic>
#include <mutex>
#include <memory>

namespace bb {

// 错误码定义
enum class ErrorCode : int32_t {
    SUCCESS                 = 0,
    ERR_UNKNOWN             = -1,
    ERR_INVALID_PARAM       = -2,
    ERR_NOT_INITIALIZED     = -3,
    ERR_ALREADY_INITIALIZED = -4,
    ERR_MODULE_LOAD_FAIL    = -5,
    ERR_MODULE_NOT_FOUND    = -6,
    ERR_ACCESS_DENIED       = -7,
    ERR_NETWORK_FAIL        = -8,
    ERR_DRIVER_FAIL         = -9,
    ERR_INTEGRITY_VIOLATION = -10,
    ERR_SELF_PROTECT_FAIL   = -11,
    ERR_VIRUS_SIGNATURE     = -12,
};

// 进程信息
struct ProcessInfo {
    uint32_t pid;
    std::string name;
    std::string path;
    double cpu_usage;       // 0.0 ~ 100.0
    uint64_t memory_mb;
    uint64_t gpu_memory_mb;
    std::string user;
    uint32_t thread_count;
    bool is_system;
    std::chrono::steady_clock::time_point snapshot_time;
};

// CPU/内存/显存阈值配置
struct ResourceThreshold {
    double cpu_threshold;           // CPU 百分比
    uint64_t memory_threshold_mb;
    uint64_t gpu_threshold_mb;
    bool auto_kill;                 // 自动结束
    uint32_t grace_period_sec;      // 宽限期（秒）
};

// 线程状态
struct ThreadInfo {
    uint32_t tid;
    uint32_t pid;
    std::string name;
    bool is_stuck;                  // 是否卡住
    uint64_t cpu_time_ms;
    std::chrono::steady_clock::time_point last_active;
};

// 文件完整性信息
struct FileIntegrityInfo {
    std::string path;
    std::string sha256;
    std::string expected_sha256;
    bool valid;
    uint64_t size;
    std::chrono::system_clock::time_point last_check;
};

// 弹窗信息
struct PopupInfo {
    uint32_t pid;
    std::string process_name;
    std::string window_title;
    std::string window_class;
    uint64_t hwnd;
    std::chrono::steady_clock::time_point detected_time;
    bool blocked;
};

// 病毒扫描结果
struct VirusScanResult {
    std::string file_path;
    std::string virus_name;
    std::string signature_hash;
    bool is_infected;
    bool is_whitelisted;
    std::string db_source;  // public / custom
};

// 模块描述
struct ModuleDescriptor {
    std::string name;
    std::string version;
    std::string description;
    std::string author;
    bool enabled;
    bool is_loaded;
    uint32_t priority;      // 加载优先级，越小越高
};

// 远程指令
struct RemoteCommand {
    uint32_t command_id;
    std::string module;
    std::string action;
    std::string payload;    // JSON 格式参数
    bool require_response;
    std::chrono::steady_clock::time_point received_time;
};

// 远程指令响应
struct RemoteResponse {
    uint32_t command_id;
    ErrorCode code;
    std::string data;
};

// 事件类型
enum class EventType : uint32_t {
    PROCESS_HIGH_CPU       = 0x0001,
    PROCESS_HIGH_MEMORY    = 0x0002,
    PROCESS_HIGH_GPU       = 0x0004,
    PROCESS_KILLED         = 0x0008,
    THREAD_STUCK           = 0x0010,
    INTEGRITY_VIOLATION    = 0x0020,
    VIRUS_DETECTED         = 0x0040,
    POPUP_DETECTED         = 0x0080,
    POPUP_BLOCKED          = 0x0100,
    SELF_PROTECT_TRIGGERED = 0x0200,
    MODULE_LOADED          = 0x0400,
    MODULE_UNLOADED        = 0x0800,
    REMOTE_COMMAND         = 0x1000,
    SYSTEM_ERROR           = 0x2000,
};

using EventCallback = std::function<void(EventType, const std::string& data)>;

// 版本号
constexpr uint32_t BB_VERSION_MAJOR = 0;
constexpr uint32_t BB_VERSION_MINOR = 1;
constexpr uint32_t BB_VERSION_PATCH = 0;
constexpr const char* BB_VERSION_STR = "0.1.0";
constexpr const char* BB_CODENAME = "ButterflyBlades Alpha";

// 默认配置路径
#ifdef _WIN32
constexpr const char* DEFAULT_CONFIG_DIR = "C:\\ProgramData\\ButterflyBlades";
#elif defined(__linux__) || defined(__APPLE__)
constexpr const char* DEFAULT_CONFIG_DIR = "/etc/butterflyblades";
#endif

} // namespace bb
