// ButterflyBlades 病毒扫描模块
// 支持公开病毒库 + 自定义病毒库 + 文件白名单

#include <atomic>
#include <thread>
#include "core.h"
#include <nlohmann/json.hpp>
#include <unordered_set>
#include <unordered_map>
#include <fstream>
#include <queue>
#include <regex>

namespace bb {

class VirusScanModule : public IModule {
private:
    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};
    std::unique_ptr<std::thread> scan_thread_;
    EventCallback event_callback_;
    bool scan_in_progress_{false};

    // 病毒特征库（签名哈希映射到病毒名）
    struct SignatureEntry {
        std::string hash;
        std::string virus_name;
        std::string source;  // "public" / "custom"
    };
    std::vector<SignatureEntry> signatures_;
    std::mutex sig_mutex_;

    // 文件白名单
    std::unordered_set<std::string> whitelist_;  // 路径
    std::mutex whitelist_mutex_;

    // 白名单模式（路径匹配 / 哈希匹配）
    std::unordered_set<std::string> whitelist_hashes_;
    std::mutex whitelist_hash_mutex_;

    // 扫描队列
    std::queue<std::string> scan_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;

    // 统计
    uint64_t total_scanned_ = 0;
    uint64_t total_detected_ = 0;
    uint64_t total_whitelisted_ = 0;

    // 排除目录
    std::vector<std::string> exclude_dirs_;
    std::mutex exclude_mutex_;

public:
    ModuleDescriptor get_descriptor() const override {
        ModuleDescriptor desc;
        desc.name = "virus_scan";
        desc.version = "0.1.0";
        desc.description = "Virus scanner with public/custom signature DB and whitelist";
        desc.author = "ButterflyBlades Team";
        desc.enabled = false;
        desc.is_loaded = false;
        desc.priority = 15;
        return desc;
    }

    std::string get_name() const override { return "virus_scan"; }

    ErrorCode init() override {
        // 默认排除目录
        exclude_dirs_ = {"C:\\Windows", "C:\\Program Files", "C:\\Program Files (x86)"};
        initialized_.store(true);
        return ErrorCode::SUCCESS;
    }

    ErrorCode start() override {
        if (!static_cast<bool>(initialized_)) return ErrorCode::ERR_NOT_INITIALIZED;
        running_.store(true);

        scan_thread_ = std::make_unique<std::thread>([this]() {
            scan_worker();
        });
        return ErrorCode::SUCCESS;
    }

    ErrorCode stop() override {
        running_.store(false);
        queue_cv_.notify_all();
        if (scan_thread_ && scan_thread_->joinable()) {
            scan_thread_->join();
            scan_thread_.reset();
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

    bool is_running() const override { return static_cast<bool>(running_); }
    bool is_initialized() const override { return static_cast<bool>(initialized_); }

    ErrorCode load_config(const std::string& config_path) override {
        try {
            std::ifstream file(config_path);
            nlohmann::json j;
            file >> j;

            // 加载病毒库
            {
                std::lock_guard lock(sig_mutex_);
                for (auto& sig : j["signatures"]) {
                    signatures_.push_back({
                        sig["hash"],
                        sig["virus_name"],
                        sig.value("source", "custom")
                    });
                }
            }

            // 加载白名单
            {
                std::lock_guard lock(whitelist_mutex_);
                for (auto& path : j["whitelist_paths"]) {
                    whitelist_.insert(path.get<std::string>());
                }
            }
            {
                std::lock_guard lock(whitelist_hash_mutex_);
                for (auto& hash : j["whitelist_hashes"]) {
                    whitelist_hashes_.insert(hash.get<std::string>());
                }
            }
        } catch (...) {
            return ErrorCode::ERR_UNKNOWN;
        }
        return ErrorCode::SUCCESS;
    }

    ErrorCode save_config(const std::string& config_path) override {
        nlohmann::json j;
        j["signatures"] = nlohmann::json::array();
        {
            std::lock_guard lock(sig_mutex_);
            for (auto& sig : signatures_) {
                nlohmann::json s;
                s["hash"] = sig.hash;
                s["virus_name"] = sig.virus_name;
                s["source"] = sig.source;
                j["signatures"].push_back(s);
            }
        }

        j["whitelist_paths"] = nlohmann::json::array();
        {
            std::lock_guard lock(whitelist_mutex_);
            for (auto& p : whitelist_) {
                j["whitelist_paths"].push_back(p);
            }
        }

        j["whitelist_hashes"] = nlohmann::json::array();
        {
            std::lock_guard lock(whitelist_hash_mutex_);
            for (auto& h : whitelist_hashes_) {
                j["whitelist_hashes"].push_back(h);
            }
        }

        std::ofstream file(config_path);
        file << j.dump(4);
        return ErrorCode::SUCCESS;
    }

    ErrorCode handle_remote_command(const RemoteCommand& cmd, RemoteResponse& resp) override {
        resp.command_id = cmd.command_id;

        if (cmd.action == "scan_path") {
            try {
                auto j = nlohmann::json::parse(cmd.payload);
                std::string path = j["path"];
                bool recursive = j.value("recursive", true);
                ErrorCode ec = queue_scan(path, recursive);
                resp.code = ec;
                if (ec == ErrorCode::SUCCESS) {
                    resp.data = R"({"status":"queued"})";
                }
            } catch (...) {
                resp.code = ErrorCode::ERR_INVALID_PARAM;
            }
        }
        else if (cmd.action == "add_signature") {
            try {
                auto j = nlohmann::json::parse(cmd.payload);
                std::lock_guard lock(sig_mutex_);
                signatures_.push_back({
                    j["hash"], j["virus_name"], j.value("source", "custom")
                });
                resp.code = ErrorCode::SUCCESS;
            } catch (...) {
                resp.code = ErrorCode::ERR_INVALID_PARAM;
            }
        }
        else if (cmd.action == "remove_signature") {
            try {
                auto j = nlohmann::json::parse(cmd.payload);
                std::string hash = j["hash"];
                std::lock_guard lock(sig_mutex_);
                signatures_.erase(
                    std::remove_if(signatures_.begin(), signatures_.end(),
                        [&](const SignatureEntry& s) { return s.hash == hash; }),
                    signatures_.end());
                resp.code = ErrorCode::SUCCESS;
            } catch (...) {
                resp.code = ErrorCode::ERR_INVALID_PARAM;
            }
        }
        else if (cmd.action == "add_whitelist") {
            try {
                auto j = nlohmann::json::parse(cmd.payload);
                if (j.contains("path")) {
                    std::lock_guard lock(whitelist_mutex_);
                    whitelist_.insert(j["path"].get<std::string>());
                }
                if (j.contains("hash")) {
                    std::lock_guard lock(whitelist_hash_mutex_);
                    whitelist_hashes_.insert(j["hash"].get<std::string>());
                }
                resp.code = ErrorCode::SUCCESS;
            } catch (...) {
                resp.code = ErrorCode::ERR_INVALID_PARAM;
            }
        }
        else if (cmd.action == "remove_whitelist") {
            try {
                auto j = nlohmann::json::parse(cmd.payload);
                if (j.contains("path")) {
                    std::lock_guard lock(whitelist_mutex_);
                    whitelist_.erase(j["path"].get<std::string>());
                }
                resp.code = ErrorCode::SUCCESS;
            } catch (...) {
                resp.code = ErrorCode::ERR_INVALID_PARAM;
            }
        }
        else if (cmd.action == "update_public_db") {
            resp.code = download_public_virus_db();
        }
        else if (cmd.action == "status") {
            nlohmann::json j;
            j["running"] = static_cast<bool>(running_);
            j["scan_in_progress"] = scan_in_progress_;
            j["signatures_count"] = signatures_.size();
            j["whitelist_count"] = whitelist_.size();
            j["total_scanned"] = total_scanned_;
            j["total_detected"] = total_detected_;
            j["total_whitelisted"] = total_whitelisted_;
            resp.data = j.dump();
            resp.code = ErrorCode::SUCCESS;
        }
        else {
            resp.code = ErrorCode::ERR_INVALID_PARAM;
        }
        return resp.code;
    }

private:
    ErrorCode queue_scan(const std::string& path, bool recursive) {
        {
            std::lock_guard lock(queue_mutex_);
            scan_queue_.push(path);
        }
        queue_cv_.notify_one();
        return ErrorCode::SUCCESS;
    }

    ErrorCode download_public_virus_db() {
        // 从公开病毒库源下载特征
        // 实际项目中应连接 VirusTotal / ClamAV 等
        return ErrorCode::SUCCESS;
    }

    bool is_excluded(const std::string& path) {
        std::lock_guard lock(exclude_mutex_);
        for (auto& d : exclude_dirs_) {
            if (path.find(d) == 0) return true;
        }
        return false;
    }

    bool is_whitelisted(const std::string& path, const std::string& hash) {
        {
            std::lock_guard lock(whitelist_mutex_);
            if (whitelist_.count(path)) return true;
        }
        {
            std::lock_guard lock(whitelist_hash_mutex_);
            if (whitelist_hashes_.count(hash)) return true;
        }
        return false;
    }

    std::string match_signature(const std::string& file_hash) {
        std::lock_guard lock(sig_mutex_);
        for (auto& sig : signatures_) {
            if (sig.hash == file_hash) {
                return sig.virus_name;
            }
        }
        return "";
    }

    VirusScanResult scan_file(const std::string& file_path) {
        auto& svc = ServiceManager::instance();
        auto* fileOps = svc.file_ops();

        VirusScanResult result;
        result.file_path = file_path;
        result.is_infected = false;
        result.is_whitelisted = false;

        // 排除系统目录
        if (is_excluded(file_path)) {
            return result; // 跳过
        }

        std::string hash = fileOps->compute_sha256(file_path);
        result.signature_hash = hash;

        if (hash.empty()) {
            return result;
        }

        // 检查白名单
        if (is_whitelisted(file_path, hash)) {
            result.is_whitelisted = true;
            total_whitelisted_++;
            return result;
        }

        // 匹配病毒库
        std::string virus_name = match_signature(hash);
        if (!virus_name.empty()) {
            result.is_infected = true;
            result.virus_name = virus_name;
            result.db_source = "database";

            total_detected_++;
            if (event_callback_) {
                nlohmann::json j;
                j["file"] = file_path;
                j["virus"] = virus_name;
                j["hash"] = hash;
                event_callback_(EventType::VIRUS_DETECTED, j.dump());
            }
        }

        return result;
    }

    void scan_worker() {
        auto& svc = ServiceManager::instance();
        auto* fileOps = svc.file_ops();

        while (running_.load()) {
            std::string target_path;

            {
                std::unique_lock lock(queue_mutex_);
                queue_cv_.wait_for(lock, std::chrono::seconds(5), [this]() {
                    return !static_cast<bool>(running_) || !scan_queue_.empty();
                });

                if (!static_cast<bool>(running_) && scan_queue_.empty()) break;
                if (scan_queue_.empty()) continue;

                target_path = scan_queue_.front();
                scan_queue_.pop();
            }

            scan_in_progress_ = true;

            auto files = fileOps->enumerate_files(target_path, true);
            for (auto& f : files) {
                if (!static_cast<bool>(running_)) break;
                scan_file(f);
                total_scanned_++;
            }

            scan_in_progress_ = false;
        }
    }
};

#ifndef BB_STATIC_MODULES
extern "C" IModule* create_module() {
    return new VirusScanModule();
}

extern "C" void destroy_module(IModule* mod) {
    delete mod;
}
#endif

} // namespace bb
