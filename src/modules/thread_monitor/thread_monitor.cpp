// ButterflyBlades 线程卡住检测模块
// 检测进程中长时间不响应的线程

#include "core.h"
#include <nlohmann/json.hpp>
#include <thread>
#include <atomic>
#include <unordered_map>

namespace bb {

class ThreadMonitorModule : public IModule {
private:
    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};
    std::unique_ptr<std::thread> monitor_thread_;
    EventCallback event_callback_;
    uint32_t stuck_threshold_ms_ = 30000;  // 30秒无响应视为卡住
    uint32_t check_interval_ms_ = 5000;

    struct ThreadSnapshot {
        uint32_t tid;
        uint32_t pid;
        uint64_t last_cpu_time;
        std::chrono::steady_clock::time_point last_check;
    };
    std::mutex snapshot_mutex_;
    std::unordered_map<uint32_t, ThreadSnapshot> thread_snapshots_;

public:
    ModuleDescriptor get_descriptor() const override {
        ModuleDescriptor desc;
        desc.name = "thread_monitor";
        desc.version = "0.1.0";
        desc.description = "Thread stuck detection module";
        desc.author = "ButterflyBlades Team";
        desc.enabled = false;
        desc.is_loaded = false;
        desc.priority = 20;
        return desc;
    }

    std::string get_name() const override { return "thread_monitor"; }

    ErrorCode init() override {
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
        return ErrorCode::SUCCESS;
    }

    ErrorCode save_config(const std::string& config_path) override {
        return ErrorCode::SUCCESS;
    }

    ErrorCode handle_remote_command(const RemoteCommand& cmd, RemoteResponse& resp) override {
        resp.command_id = cmd.command_id;

        if (cmd.action == "set_threshold") {
            try {
                auto j = nlohmann::json::parse(cmd.payload);
                if (j.contains("stuck_threshold_ms")) stuck_threshold_ms_ = j["stuck_threshold_ms"];
                if (j.contains("check_interval_ms")) check_interval_ms_ = j["check_interval_ms"];
                resp.code = ErrorCode::SUCCESS;
            } catch (...) {
                resp.code = ErrorCode::ERR_INVALID_PARAM;
            }
        }
        else if (cmd.action == "status") {
            nlohmann::json j;
            j["running"] = running_.load();
            j["tracked_threads"] = thread_snapshots_.size();
            j["stuck_threshold_ms"] = stuck_threshold_ms_;
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

        while (running_.load()) {
            auto processes = procOps->enumerate_processes();
            std::unordered_map<uint32_t, ThreadSnapshot> new_snapshots;
            auto now = std::chrono::steady_clock::now();

            for (auto& proc : processes) {
                if (proc.is_system) continue;
                auto threads = procOps->get_threads(proc.pid);

                for (auto& t : threads) {
                    ThreadSnapshot snap;
                    snap.tid = t.tid;
                    snap.pid = proc.pid;
                    snap.last_cpu_time = t.cpu_time_ms;
                    snap.last_check = now;

                    // 对比上一次快照
                    {
                        std::lock_guard lock(snapshot_mutex_);
                        auto it = thread_snapshots_.find(t.tid);
                        if (it != thread_snapshots_.end()) {
                            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                now - it->second.last_check).count();

                            if (elapsed > stuck_threshold_ms_ &&
                                snap.last_cpu_time == it->second.last_cpu_time) {
                                // 线程疑似卡住
                                if (event_callback_) {
                                    nlohmann::json j;
                                    j["tid"] = t.tid;
                                    j["pid"] = proc.pid;
                                    j["process"] = proc.name;
                                    j["stuck_duration_ms"] = elapsed;
                                    event_callback_(EventType::THREAD_STUCK, j.dump());
                                }
                            }
                        }
                    }

                    new_snapshots[t.tid] = snap;
                }
            }

            {
                std::lock_guard lock(snapshot_mutex_);
                thread_snapshots_ = std::move(new_snapshots);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(check_interval_ms_));
        }
    }
};

#ifndef BB_STATIC_MODULES
extern "C" IModule* create_module() {
    return new ThreadMonitorModule();
}

extern "C" void destroy_module(IModule* mod) {
    delete mod;
}
#endif

} // namespace bb
