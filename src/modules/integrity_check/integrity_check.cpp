// ButterflyBlades 核心代码完整性检测模块
// 检测自身文件和关键模块是否被篡改

#include "core.h"
#include <nlohmann/json.hpp>
#include <thread>
#include <atomic>
#include <fstream>
#include <sstream>

namespace bb {

class IntegrityCheckModule : public IModule {
private:
    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};
    std::unique_ptr<std::thread> check_thread_;
    EventCallback event_callback_;
    uint32_t check_interval_sec_ = 60;
    std::mutex config_mutex_;
    std::vector<FileIntegrityInfo> monitored_files_;
    std::mutex files_mutex_;

public:
    ModuleDescriptor get_descriptor() const override {
        ModuleDescriptor desc;
        desc.name = "integrity_check";
        desc.version = "0.1.0";
        desc.description = "Core code integrity verification module";
        desc.author = "ButterflyBlades Team";
        desc.enabled = false;
        desc.is_loaded = false;
        desc.priority = 5;  // 高优先级
        return desc;
    }

    std::string get_name() const override { return "integrity_check"; }

    ErrorCode init() override {
        // 初始监控自身可执行文件和已加载模块
        initialized_.store(true);
        return ErrorCode::SUCCESS;
    }

    ErrorCode start() override {
        if (!initialized_.load()) return ErrorCode::ERR_NOT_INITIALIZED;
        running_.store(true);

        check_thread_ = std::make_unique<std::thread>([this]() {
            integrity_loop();
        });
        return ErrorCode::SUCCESS;
    }

    ErrorCode stop() override {
        running_.store(false);
        if (check_thread_ && check_thread_->joinable()) {
            check_thread_->join();
            check_thread_.reset();
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
        // 从配置文件加载需要监控的文件清单及期望的SHA256
        std::ifstream file(config_path);
        if (!file.is_open()) return ErrorCode::ERR_UNKNOWN;

        std::lock_guard lock(files_mutex_);
        monitored_files_.clear();

        nlohmann::json j;
        file >> j;
        for (auto& item : j["files"]) {
            FileIntegrityInfo info;
            info.path = item["path"];
            info.expected_sha256 = item.value("sha256", "");
            info.valid = true;
            info.last_check = std::chrono::system_clock::now();
            monitored_files_.push_back(info);
        }
        return ErrorCode::SUCCESS;
    }

    ErrorCode save_config(const std::string& config_path) override {
        std::lock_guard lock(files_mutex_);
        nlohmann::json j;
        j["files"] = nlohmann::json::array();
        for (auto& info : monitored_files_) {
            nlohmann::json item;
            item["path"] = info.path;
            item["sha256"] = info.expected_sha256;
            j["files"].push_back(item);
        }
        std::ofstream file(config_path);
        file << j.dump(4);
        return ErrorCode::SUCCESS;
    }

    ErrorCode handle_remote_command(const RemoteCommand& cmd, RemoteResponse& resp) override {
        resp.command_id = cmd.command_id;

        if (cmd.action == "add_file") {
            try {
                auto j = nlohmann::json::parse(cmd.payload);
                FileIntegrityInfo info;
                info.path = j["path"];
                info.expected_sha256 = j.value("sha256", "");
                info.valid = true;
                info.last_check = std::chrono::system_clock::now();
                {
                    std::lock_guard lock(files_mutex_);
                    monitored_files_.push_back(info);
                }
                resp.code = ErrorCode::SUCCESS;
            } catch (...) {
                resp.code = ErrorCode::ERR_INVALID_PARAM;
            }
        }
        else if (cmd.action == "remove_file") {
            try {
                auto j = nlohmann::json::parse(cmd.payload);
                std::string path = j["path"];
                std::lock_guard lock(files_mutex_);
                monitored_files_.erase(
                    std::remove_if(monitored_files_.begin(), monitored_files_.end(),
                        [&](const FileIntegrityInfo& f) { return f.path == path; }),
                    monitored_files_.end());
                resp.code = ErrorCode::SUCCESS;
            } catch (...) {
                resp.code = ErrorCode::ERR_INVALID_PARAM;
            }
        }
        else if (cmd.action == "check_now") {
            run_integrity_check();
            resp.code = ErrorCode::SUCCESS;
        }
        else if (cmd.action == "list") {
            nlohmann::json j = nlohmann::json::array();
            std::lock_guard lock(files_mutex_);
            for (auto& f : monitored_files_) {
                nlohmann::json item;
                item["path"] = f.path;
                item["valid"] = f.valid;
                item["size"] = f.size;
                j.push_back(item);
            }
            resp.data = j.dump();
            resp.code = ErrorCode::SUCCESS;
        }
        else {
            resp.code = ErrorCode::ERR_INVALID_PARAM;
        }
        return resp.code;
    }

private:
    void integrity_loop() {
        while (running_.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(check_interval_sec_));
            if (!running_.load()) break;
            run_integrity_check();
        }
    }

    void run_integrity_check() {
        auto& svc = ServiceManager::instance();
        auto* fileOps = svc.file_ops();

        std::lock_guard lock(files_mutex_);
        for (auto& info : monitored_files_) {
            if (!fileOps->file_exists(info.path)) {
                info.valid = false;
                report_violation(info, "file_missing");
                continue;
            }

            info.size = fileOps->file_size(info.path);
            std::string current_hash = fileOps->compute_sha256(info.path);

            if (!info.expected_sha256.empty() && current_hash != info.expected_sha256) {
                info.valid = false;
                info.sha256 = current_hash;
                report_violation(info, "hash_mismatch");
            } else {
                info.valid = true;
                info.sha256 = current_hash;
                info.expected_sha256 = current_hash;
            }

            info.last_check = std::chrono::system_clock::now();
        }
    }

    void report_violation(const FileIntegrityInfo& info, const std::string& reason) {
        if (event_callback_) {
            nlohmann::json j;
            j["path"] = info.path;
            j["expected_sha256"] = info.expected_sha256;
            j["actual_sha256"] = info.sha256;
            j["reason"] = reason;
            event_callback_(EventType::INTEGRITY_VIOLATION, j.dump());
        }
    }
};

#ifndef BB_STATIC_MODULES
extern "C" IModule* create_module() {
    return new IntegrityCheckModule();
}

extern "C" void destroy_module(IModule* mod) {
    delete mod;
}
#endif

} // namespace bb
