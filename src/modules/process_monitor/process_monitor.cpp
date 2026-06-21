// ButterflyBlades 进程监控模块
// 监控 CPU/内存/GPU显存占用超标的进程，支持自动结束

#include "core.h"
#include <nlohmann/json.hpp>
#include <thread>
#include <atomic>
#include <unordered_map>

using json = nlohmann::json;

namespace bb {

class ProcessMonitorModule : public IModule {
private:
    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};
    std::unique_ptr<std::thread> monitor_thread_;
    EventCallback event_callback_;
    ResourceThreshold threshold_;
    std::mutex config_mutex_;

    // 记录进程超阈值持续时间（用于宽限期判断）
    struct ViolationRecord {
        uint32_t pid;
        std::chrono::steady_clock::time_point first_violation;
        bool alerted;
    };
    std::unordered_map<uint32_t, ViolationRecord> violations_;
    std::mutex violation_mutex_;

public:
    ModuleDescriptor get_descriptor() const override {
        ModuleDescriptor desc;
        desc.name = "process_monitor";
        desc.version = "0.1.0";
        desc.description = "CPU/Memory/GPU high usage process monitor with auto-kill";
        desc.author = "ButterflyBlades Team";
        desc.enabled = false;
        desc.is_loaded = false;
        desc.priority = 10;
        return desc;
    }

    std::string get_name() const override { return "process_monitor"; }

    ErrorCode init() override {
        // 默认阈值
        threshold_.cpu_threshold = 80.0;
        threshold_.memory_threshold_mb = 4096;  // 4GB
        threshold_.gpu_threshold_mb = 2048;     // 2GB
        threshold_.auto_kill = false;
        threshold_.grace_period_sec = 30;

        initialized_.store(true);
        return ErrorCode::SUCCESS;
    }

    ErrorCode start() override {
        if (!initialized_.load()) return ErrorCode::ERR_NOT_INITIALIZED;
        running_.store(true);

        monitor_thread_ = std::make_unique<std::thread>([this]() {
            monitor_loop();
        });

        return ErrorCode::SUCCESS;
    }

    ErrorCode stop() override {
        running_.store(false);
        if (monitor_thread_ && monitor_thread_->joinable()) {
            monitor_thread_->join();
            monitor_thread_.reset();
        }
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
        // 从JSON加载配置
        return ErrorCode::SUCCESS;
    }

    ErrorCode save_config(const std::string& config_path) override {
        return ErrorCode::SUCCESS;
    }

    ErrorCode handle_remote_command(const RemoteCommand& cmd, RemoteResponse& resp) override {
        resp.command_id = cmd.command_id;

        if (cmd.action == "get_thresholds") {
            json j;
            j["cpu_threshold"] = threshold_.cpu_threshold;
            j["memory_threshold_mb"] = threshold_.memory_threshold_mb;
            j["gpu_threshold_mb"] = threshold_.gpu_threshold_mb;
            j["auto_kill"] = threshold_.auto_kill;
            j["grace_period_sec"] = threshold_.grace_period_sec;
            resp.data = j.dump();
            resp.code = ErrorCode::SUCCESS;
        }
        else if (cmd.action == "set_thresholds") {
            try {
                json j = json::parse(cmd.payload);
                std::lock_guard lock(config_mutex_);
                if (j.contains("cpu_threshold")) threshold_.cpu_threshold = j["cpu_threshold"];
                if (j.contains("memory_threshold_mb")) threshold_.memory_threshold_mb = j["memory_threshold_mb"];
                if (j.contains("gpu_threshold_mb")) threshold_.gpu_threshold_mb = j["gpu_threshold_mb"];
                if (j.contains("auto_kill")) threshold_.auto_kill = j["auto_kill"];
                if (j.contains("grace_period_sec")) threshold_.grace_period_sec = j["grace_period_sec"];
                resp.code = ErrorCode::SUCCESS;
            } catch (...) {
                resp.code = ErrorCode::ERR_INVALID_PARAM;
            }
        }
        else if (cmd.action == "status") {
            json j;
            j["running"] = running_.load();
            j["violations_active"] = violations_.size();
            resp.data = j.dump();
            resp.code = ErrorCode::SUCCESS;
        }
        else {
            resp.code = ErrorCode::ERR_INVALID_PARAM;
        }

        return resp.code;
    }

private:
    void monitor_loop() {
        auto& svc = ServiceManager::instance();
        auto* procOps = svc.process_ops();
        auto* sysOps = svc.system_ops();

        while (running_.load()) {
            ResourceThreshold thresh;
            {
                std::lock_guard lock(config_mutex_);
                thresh = threshold_;
            }

            auto processes = procOps->enumerate_processes();
            auto now = std::chrono::steady_clock::now();

            for (auto& proc : processes) {
                if (proc.is_system || proc.pid == 0 || proc.pid == 4) continue;

                bool high_cpu = proc.cpu_usage > thresh.cpu_threshold;
                bool high_mem = proc.memory_mb > thresh.memory_threshold_mb;
                bool high_gpu = proc.gpu_memory_mb > thresh.gpu_threshold_mb;

                if (!high_cpu && !high_mem && !high_gpu) {
                    // 恢复正常，清除记录
                    std::lock_guard lock(violation_mutex_);
                    violations_.erase(proc.pid);
                    continue;
                }

                // 记录违规
                std::lock_guard lock(violation_mutex_);
                auto& rec = violations_[proc.pid];
                if (rec.pid == 0) {
                    rec.pid = proc.pid;
                    rec.first_violation = now;
                    rec.alerted = false;
                }

                auto duration = std::chrono::duration_cast<std::chrono::seconds>(
                    now - rec.first_violation).count();

                // 告警
                if (!rec.alerted && event_callback_) {
                    json j;
                    j["pid"] = proc.pid;
                    j["name"] = proc.name;
                    j["cpu"] = proc.cpu_usage;
                    j["memory_mb"] = proc.memory_mb;
                    j["gpu_mb"] = proc.gpu_memory_mb;
                    j["duration_sec"] = duration;

                    EventType evt = EventType::PROCESS_HIGH_CPU;
                    if (high_gpu) evt = EventType::PROCESS_HIGH_GPU;
                    else if (high_mem) evt = EventType::PROCESS_HIGH_MEMORY;

                    event_callback_(evt, j.dump());
                    rec.alerted = true;
                }

                // 宽限期后自动结束
                if (thresh.auto_kill && duration >= (int64_t)thresh.grace_period_sec) {
                    procOps->kill_process(proc.pid, true);
                    if (event_callback_) {
                        json j;
                        j["pid"] = proc.pid;
                        j["name"] = proc.name;
                        j["reason"] = "auto_kill";
                        event_callback_(EventType::PROCESS_KILLED, j.dump());
                    }
                    violations_.erase(proc.pid);
                }
            }

            // 清理已退出进程的记录
            {
                std::lock_guard lock(violation_mutex_);
                std::vector<uint32_t> to_remove;
                for (auto& [pid, rec] : violations_) {
                    bool found = false;
                    for (auto& p : processes) {
                        if (p.pid == pid) { found = true; break; }
                    }
                    if (!found) to_remove.push_back(pid);
                }
                for (auto pid : to_remove) violations_.erase(pid);
            }

            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }
};

// 模块导出
#ifndef BB_STATIC_MODULES
extern "C" IModule* create_module() {
    return new ProcessMonitorModule();
}

extern "C" void destroy_module(IModule* mod) {
    delete mod;
}
#endif

} // namespace bb
