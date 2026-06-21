#pragma once

// ButterflyBlades 平台抽象层
// Windows/Linux/macOS/iOS/Android 统一接口

#include "types.h"
#include <string>
#include <vector>
#include <functional>

namespace bb::platform {

// ========== 进程操作 ==========
class ProcessOps {
public:
    virtual ~ProcessOps() = default;

    // 枚举所有进程
    virtual std::vector<ProcessInfo> enumerate_processes() = 0;

    // 获取单个进程详情
    virtual ProcessInfo get_process_info(uint32_t pid) = 0;

    // 结束进程
    virtual ErrorCode kill_process(uint32_t pid, bool force = false) = 0;

    // 获取进程CPU占用
    virtual double get_process_cpu(uint32_t pid) = 0;

    // 获取进程内存占用 (MB)
    virtual uint64_t get_process_memory(uint32_t pid) = 0;

    // 获取进程GPU显存占用 (MB)
    virtual uint64_t get_process_gpu_memory(uint32_t pid) = 0;

    // 获取进程线程列表
    virtual std::vector<ThreadInfo> get_threads(uint32_t pid) = 0;

    // 挂起进程
    virtual ErrorCode suspend_process(uint32_t pid) = 0;

    // 恢复进程
    virtual ErrorCode resume_process(uint32_t pid) = 0;
};

// ========== 窗口操作 ==========
class WindowOps {
public:
    virtual ~WindowOps() = default;

    // 枚举所有顶层窗口
    virtual std::vector<PopupInfo> enumerate_windows() = 0;

    // 关闭窗口
    virtual ErrorCode close_window(uint64_t hwnd) = 0;

    // 获取窗口标题
    virtual std::string get_window_title(uint64_t hwnd) = 0;

    // 获取窗口类名
    virtual std::string get_window_class(uint64_t hwnd) = 0;

    // 获取窗口所属进程ID
    virtual uint32_t get_window_pid(uint64_t hwnd) = 0;

    // 发送消息到窗口
    virtual ErrorCode send_message(uint64_t hwnd, uint32_t msg, uint64_t wparam, int64_t lparam) = 0;
};

// ========== 文件系统操作 ==========
class FileOps {
public:
    virtual ~FileOps() = default;

    // 计算文件SHA256
    virtual std::string compute_sha256(const std::string& path) = 0;

    // 枚举目录下所有文件
    virtual std::vector<std::string> enumerate_files(const std::string& dir, bool recursive) = 0;

    // 检查文件是否存在
    virtual bool file_exists(const std::string& path) = 0;

    // 获取文件大小
    virtual uint64_t file_size(const std::string& path) = 0;

    // 只读打开文件（用于扫描）
    virtual bool open_read_only(const std::string& path, std::vector<uint8_t>& buffer) = 0;

    // 监控目录变化
    using FileWatchCallback = std::function<void(const std::string& path, int action)>;
    virtual void* watch_directory(const std::string& dir, FileWatchCallback cb) = 0;
    virtual void unwatch_directory(void* handle) = 0;
};

// ========== 驱动操作（Windows 专属，其他平台实现空操作）==========
class DriverOps {
public:
    virtual ~DriverOps() = default;

    // 加载内核驱动
    virtual ErrorCode load_driver(const std::string& driver_path, const std::string& service_name) = 0;

    // 卸载内核驱动
    virtual ErrorCode unload_driver(const std::string& service_name) = 0;

    // 与驱动通信
    virtual ErrorCode ioctl(uint32_t control_code, const std::vector<uint8_t>& in_data,
                            std::vector<uint8_t>& out_data) = 0;

    // 驱动层进程枚举（可检测隐藏进程）
    virtual std::vector<ProcessInfo> enumerate_processes_kernel() = 0;

    // 驱动层模块枚举
    virtual std::vector<std::string> enumerate_loaded_modules() = 0;

    // 是否支持驱动操作
    virtual bool is_driver_supported() = 0;
};

// ========== 网络操作 ==========
class NetworkOps {
public:
    virtual ~NetworkOps() = default;

    // TCP 连接
    virtual ErrorCode tcp_connect(const std::string& host, uint16_t port) = 0;
    virtual ErrorCode tcp_disconnect() = 0;
    virtual ErrorCode tcp_send(const std::vector<uint8_t>& data) = 0;
    virtual ErrorCode tcp_recv(std::vector<uint8_t>& data, uint32_t timeout_ms) = 0;

    // 启动监听（用于远程控制台）
    virtual ErrorCode start_listener(uint16_t port, const std::string& bind_addr = "0.0.0.0") = 0;
    virtual ErrorCode stop_listener() = 0;

    // 连接状态
    virtual bool is_connected() = 0;
};

// ========== 系统信息 ==========
class SystemOps {
public:
    virtual ~SystemOps() = default;

    // 系统总内存 (MB)
    virtual uint64_t get_total_memory() = 0;

    // 系统可用内存 (MB)
    virtual uint64_t get_available_memory() = 0;

    // 系统总CPU使用率
    virtual double get_system_cpu_usage() = 0;

    // OS 名称
    virtual std::string get_os_name() = 0;

    // 主机名
    virtual std::string get_hostname() = 0;

    // 当前用户名
    virtual std::string get_current_user() = 0;
};

// ========== 平台工厂 ==========
std::unique_ptr<ProcessOps> create_process_ops();
std::unique_ptr<WindowOps> create_window_ops();
std::unique_ptr<FileOps> create_file_ops();
std::unique_ptr<DriverOps> create_driver_ops();
std::unique_ptr<NetworkOps> create_network_ops();
std::unique_ptr<SystemOps> create_system_ops();

} // namespace bb::platform
