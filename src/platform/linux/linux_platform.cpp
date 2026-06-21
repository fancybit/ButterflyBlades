// Linux / macOS 平台抽象层（桩实现）
// 当前阶段：基础功能桩，完整实现后续版本追加

#include "../../include/common/platform.h"
#include <unistd.h>
#include <sys/utsname.h>
#include <pwd.h>
#include <fstream>
#include <cstring>
#include <chrono>

namespace bb {

// ========== Linux 进程操作 ==========
class LinuxProcessOps : public ProcessOps {
    std::vector<ProcessInfo> enum_processes() override {
        // 桩实现：通过 /proc 文件系统枚举进程
        std::vector<ProcessInfo> result;
        // TODO: 完整实现 /proc 枚举
        ProcessInfo pi;
        pi.pid = getpid();
        pi.ppid = getppid();
        pi.name = "ButterflyBlades";
        pi.user = "root";
        pi.cpu_usage = 0.0f;
        pi.memory_mb = 0.0f;
        pi.gpu_mb = 0.0f;
        pi.thread_count = 1;
        pi.start_time = time(nullptr);
        result.push_back(pi);
        return result;
    }

    ErrorCode kill_process(uint32_t pid, bool force) override {
        int sig = force ? SIGKILL : SIGTERM;
        if (::kill((pid_t)pid, sig) == 0) return ErrorCode::SUCCESS;
        return ErrorCode::ERR_OPERATION_FAILED;
    }

    std::string get_process_name(uint32_t pid) override {
        std::string path = "/proc/" + std::to_string(pid) + "/comm";
        std::ifstream f(path);
        std::string name;
        if (f >> name) return name;
        return "";
    }

    uint32_t get_process_id(const std::string& name) override {
        // 桩实现
        return 0;
    }
};

// ========== Linux 窗口操作 ==========
class LinuxWindowOps : public WindowOps {
    std::vector<WindowInfo> enum_windows() override {
        // 桩实现
        return {};
    }

    ErrorCode close_window(uint64_t handle, bool force) override {
        return ErrorCode::ERR_NOT_IMPLEMENTED;
    }

    ErrorCode send_key_to_window(uint64_t handle, uint32_t key_code) override {
        return ErrorCode::ERR_NOT_IMPLEMENTED;
    }
};

// ========== Linux 文件操作 ==========
class LinuxFileOps : public FileOps {
    bool file_exists(const std::string& path) override {
        return access(path.c_str(), F_OK) == 0;
    }

    ErrorCode delete_file(const std::string& path, bool permanent) override {
        if (unlink(path.c_str()) == 0) return ErrorCode::SUCCESS;
        return ErrorCode::ERR_OPERATION_FAILED;
    }

    std::string get_file_hash(const std::string& path, HashMethod method) override {
        // 桩：通过系统命令计算
        std::string cmd = "sha256sum " + path + " 2>/dev/null | cut -d' ' -f1";
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) return "";
        char buf[65] = {0};
        fread(buf, 1, 64, pipe);
        pclose(pipe);
        return std::string(buf);
    }

    bool create_directory(const std::string& path) override {
        return mkdir(path.c_str(), 0755) == 0;
    }

    ErrorCode copy_file(const std::string& src, const std::string& dst) override {
        std::ifstream in(src, std::ios::binary);
        std::ofstream out(dst, std::ios::binary);
        if (!in || !out) return ErrorCode::ERR_OPERATION_FAILED;
        out << in.rdbuf();
        return ErrorCode::SUCCESS;
    }
};

// ========== Linux 驱动操作（桩） ==========
class LinuxDriverOps : public DriverOps {
    ErrorCode load_driver(const std::string& path) override { return ErrorCode::ERR_NOT_IMPLEMENTED; }
    ErrorCode unload_driver(const std::string& name) override { return ErrorCode::ERR_NOT_IMPLEMENTED; }
    bool is_driver_loaded(const std::string& name) override { return false; }
    ErrorCode ioctl_call(uint32_t code, const std::vector<uint8_t>& in, std::vector<uint8_t>& out) override {
        return ErrorCode::ERR_NOT_IMPLEMENTED;
    }
};

// ========== Linux 网络操作 ==========
class LinuxNetworkOps : public NetworkOps {
    ErrorCode tcp_connect(const std::string& host, uint16_t port) override {
        // 桩实现
        return ErrorCode::ERR_NOT_IMPLEMENTED;
    }

    ErrorCode tcp_send(const std::vector<uint8_t>& data) override {
        return ErrorCode::ERR_NOT_IMPLEMENTED;
    }

    ErrorCode tcp_recv(std::vector<uint8_t>& data, uint32_t timeout_ms) override {
        return ErrorCode::ERR_NOT_IMPLEMENTED;
    }

    ErrorCode tcp_disconnect() override {
        return ErrorCode::ERR_NOT_IMPLEMENTED;
    }

    ErrorCode start_listener(uint16_t port, const std::string& bind_addr) override {
        return ErrorCode::ERR_NOT_IMPLEMENTED;
    }

    ErrorCode stop_listener() override {
        return ErrorCode::ERR_NOT_IMPLEMENTED;
    }
};

// ========== Linux 系统操作 ==========
class LinuxSystemOps : public SystemOps {
    std::string get_os_name() override {
        struct utsname buf;
        if (uname(&buf) == 0) {
            return std::string(buf.sysname) + " " + buf.release;
        }
        return "Linux";
    }

    std::string get_hostname() override {
        char buf[256] = {0};
        gethostname(buf, sizeof(buf));
        return buf;
    }

    std::string get_current_user() override {
        struct passwd* pw = getpwuid(getuid());
        if (pw) return pw->pw_name;
        return "unknown";
    }

    uint64_t get_total_memory() override {
        std::ifstream f("/proc/meminfo");
        std::string line;
        while (std::getline(f, line)) {
            if (line.find("MemTotal:") == 0) {
                uint64_t kb;
                sscanf(line.c_str(), "MemTotal: %lu", &kb);
                return kb / 1024; // MB
            }
        }
        return 0;
    }

    uint64_t get_available_memory() override {
        std::ifstream f("/proc/meminfo");
        std::string line;
        while (std::getline(f, line)) {
            if (line.find("MemAvailable:") == 0) {
                uint64_t kb;
                sscanf(line.c_str(), "MemAvailable: %lu", &kb);
                return kb / 1024;
            }
        }
        return 0;
    }

    float get_system_cpu_usage() override { return 0.0f; }

    float get_system_gpu_usage() override { return 0.0f; }

    ErrorCode shutdown_system(bool restart) override {
        // 需要 root 权限
        return ErrorCode::ERR_PERMISSION_DENIED;
    }
};

// ========== Linux 平台工厂函数 ==========
#ifdef __linux__
std::unique_ptr<ProcessOps> create_process_ops() { return std::make_unique<LinuxProcessOps>(); }
std::unique_ptr<WindowOps> create_window_ops() { return std::make_unique<LinuxWindowOps>(); }
std::unique_ptr<FileOps> create_file_ops() { return std::make_unique<LinuxFileOps>(); }
std::unique_ptr<DriverOps> create_driver_ops() { return std::make_unique<LinuxDriverOps>(); }
std::unique_ptr<NetworkOps> create_network_ops() { return std::make_unique<LinuxNetworkOps>(); }
std::unique_ptr<SystemOps> create_system_ops() { return std::make_unique<LinuxSystemOps>(); }
#endif

} // namespace bb
