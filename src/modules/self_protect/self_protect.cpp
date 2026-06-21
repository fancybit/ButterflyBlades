// ButterflyBlades 自我保护模块
// 防止自身进程被终止、文件被篡改、内存被注入

#include "core.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <thread>
#include <atomic>
#include <unordered_set>

#ifdef _WIN32
#include <windows.h>
#include <winternl.h>
#endif

namespace bb {

class SelfProtectModule : public IModule {
private:
    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};
    std::unique_ptr<std::thread> protect_thread_;
    EventCallback event_callback_;
    uint32_t check_interval_ms_ = 3000;

    // 受保护的关键文件列表
    std::unordered_set<std::string> protected_files_;
    std::mutex files_mutex_;

    // 反调试检测
    bool anti_debug_enabled_ = true;

    // 反注入检测
    bool anti_injection_enabled_ = true;

    // 进程保护状态
    bool process_protected_ = false;

    // 自身PID
    uint32_t self_pid_;

public:
    ModuleDescriptor get_descriptor() const override {
        ModuleDescriptor desc;
        desc.name = "self_protect";
        desc.version = "0.1.0";
        desc.description = "Self-protection: anti-kill, anti-tamper, anti-injection, anti-debug";
        desc.author = "ButterflyBlades Team";
        desc.enabled = false;
        desc.is_loaded = false;
        desc.priority = 1;  // 最高优先级，最先加载
        return desc;
    }

    std::string get_name() const override { return "self_protect"; }

    ErrorCode init() override {
#ifdef _WIN32
        self_pid_ = GetCurrentProcessId();
#else
        self_pid_ = getpid();
#endif

        // 添加默认保护文件
        protected_files_.insert("ButterflyBlades.exe");
        protected_files_.insert("bb_core.dll");
        protected_files_.insert("bb_driver.sys");

        initialized_.store(true);
        return ErrorCode::SUCCESS;
    }

    ErrorCode start() override {
        if (!initialized_.load()) return ErrorCode::ERR_NOT_INITIALIZED;

        // 启用进程保护
        enable_process_protection();

        running_.store(true);
        protect_thread_ = std::make_unique<std::thread>([this]() {
            protection_loop();
        });
        return ErrorCode::SUCCESS;
    }

    ErrorCode stop() override {
        running_.store(false);
        if (protect_thread_ && protect_thread_->joinable()) {
            protect_thread_->join();
            protect_thread_.reset();
        }
        disable_process_protection();
        return ErrorCode::SUCCESS;
    }

    ErrorCode shutdown() override {
        stop();
        initialized_.store(false);
        return ErrorCode::SUCCESS;
    }

    void set_event_callback(EventCallback callback) override {
        event_callback_ = std::move(callback);
    }

    bool is_running() const override { return running_.load(); }
    bool is_initialized() const override { return initialized_.load(); }

    ErrorCode load_config(const std::string& config_path) override {
        try {
            std::ifstream file(config_path);
            nlohmann::json j;
            file >> j;

            anti_debug_enabled_ = j.value("anti_debug", true);
            anti_injection_enabled_ = j.value("anti_injection", true);
            check_interval_ms_ = j.value("check_interval_ms", 3000);

            std::lock_guard lock(files_mutex_);
            for (auto& f : j["protected_files"]) {
                protected_files_.insert(f.get<std::string>());
            }
        } catch (...) {
            return ErrorCode::ERR_UNKNOWN;
        }
        return ErrorCode::SUCCESS;
    }

    ErrorCode save_config(const std::string& config_path) override {
        nlohmann::json j;
        j["anti_debug"] = anti_debug_enabled_;
        j["anti_injection"] = anti_injection_enabled_;
        j["check_interval_ms"] = check_interval_ms_;
        j["protected_files"] = nlohmann::json::array();
        {
            std::lock_guard lock(files_mutex_);
            for (auto& f : protected_files_) {
                j["protected_files"].push_back(f);
            }
        }
        std::ofstream file(config_path);
        file << j.dump(4);
        return ErrorCode::SUCCESS;
    }

    ErrorCode handle_remote_command(const RemoteCommand& cmd, RemoteResponse& resp) override {
        resp.command_id = cmd.command_id;

        if (cmd.action == "add_protected_file") {
            try {
                auto j = nlohmann::json::parse(cmd.payload);
                std::lock_guard lock(files_mutex_);
                protected_files_.insert(j["path"].get<std::string>());
                resp.code = ErrorCode::SUCCESS;
            } catch (...) {
                resp.code = ErrorCode::ERR_INVALID_PARAM;
            }
        }
        else if (cmd.action == "status") {
            nlohmann::json j;
            j["running"] = running_.load();
            j["process_protected"] = process_protected_;
            j["anti_debug"] = anti_debug_enabled_;
            j["anti_injection"] = anti_injection_enabled_;
            j["protected_files_count"] = protected_files_.size();
            j["pid"] = self_pid_;
            resp.data = j.dump();
            resp.code = ErrorCode::SUCCESS;
        }
        else if (cmd.action == "check_integrity") {
            // 立即执行完整性自检
            bool ok = self_integrity_check();
            nlohmann::json j;
            j["integrity_ok"] = ok;
            resp.data = j.dump();
            resp.code = ok ? ErrorCode::SUCCESS : ErrorCode::ERR_INTEGRITY_VIOLATION;
        }
        else {
            resp.code = ErrorCode::ERR_INVALID_PARAM;
        }
        return resp.code;
    }

private:
    void enable_process_protection() {
#ifdef _WIN32
        // Windows: 通过设置进程安全描述符防止被普通权限进程终止
        // 也通过 ObRegisterCallbacks（需内核驱动）阻止句柄获取
        // 用户态简化：设置进程为 critical process（Win8+）
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        if (ntdll) {
            using RtlSetProcessIsCritical_t = NTSTATUS(WINAPI*)(BOOLEAN, PBOOLEAN, BOOLEAN);
            auto RtlSetProcessIsCritical =
                (RtlSetProcessIsCritical_t)GetProcAddress(ntdll, "RtlSetProcessIsCritical");
            if (RtlSetProcessIsCritical) {
                RtlSetProcessIsCritical(TRUE, nullptr, FALSE);
                process_protected_ = true;
            }
        }
#endif
    }

    void disable_process_protection() {
#ifdef _WIN32
        if (process_protected_) {
            HMODULE ntdll = GetModuleHandleA("ntdll.dll");
            if (ntdll) {
                using RtlSetProcessIsCritical_t = NTSTATUS(WINAPI*)(BOOLEAN, PBOOLEAN, BOOLEAN);
                auto RtlSetProcessIsCritical =
                    (RtlSetProcessIsCritical_t)GetProcAddress(ntdll, "RtlSetProcessIsCritical");
                if (RtlSetProcessIsCritical) {
                    RtlSetProcessIsCritical(FALSE, nullptr, FALSE);
                    process_protected_ = false;
                }
            }
        }
#endif
    }

    bool detect_debugger() {
#ifdef _WIN32
        return IsDebuggerPresent() ||
               CheckRemoteDebuggerPresent(GetCurrentProcess(), nullptr);
#else
        return false;
#endif
    }

    bool self_integrity_check() {
        auto& svc = ServiceManager::instance();
        auto* fileOps = svc.file_ops();

        std::lock_guard lock(files_mutex_);
        for (auto& f : protected_files_) {
            if (!fileOps->file_exists(f)) {
                trigger_alert("protected_file_missing", f);
                return false;
            }
        }
        return true;
    }

    void trigger_alert(const std::string& type, const std::string& detail) {
        if (event_callback_) {
            nlohmann::json j;
            j["type"] = type;
            j["detail"] = detail;
            j["timestamp"] = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            event_callback_(EventType::SELF_PROTECT_TRIGGERED, j.dump());
        }
    }

    void protection_loop() {
        auto& svc = ServiceManager::instance();
        auto* procOps = svc.process_ops();

        while (running_.load()) {
            // 1. 反调试检测
            if (anti_debug_enabled_ && detect_debugger()) {
                trigger_alert("debugger_detected", "Debugger attached to process");
            }

            // 2. 自我进程完整性检查
            if (!self_integrity_check()) {
                trigger_alert("integrity_fail", "Protected file integrity violation");
            }

            // 3. 检查是否有其他进程尝试操作我们的句柄
            //    (全面实现需要内核驱动支持)

            std::this_thread::sleep_for(std::chrono::milliseconds(check_interval_ms_));
        }
    }
};

#ifndef BB_STATIC_MODULES
extern "C" IModule* create_module() {
    return new SelfProtectModule();
}

extern "C" void destroy_module(IModule* mod) {
    delete mod;
}
#endif

} // namespace bb
