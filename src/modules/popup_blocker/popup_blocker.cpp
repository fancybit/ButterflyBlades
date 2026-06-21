// ButterflyBlades 弹窗自动识别捕获和关闭模块
// 自动检测并关闭广告弹窗、恶意弹窗等

#include "core.h"
#include <nlohmann/json.hpp>
#include <thread>
#include <atomic>
#include <unordered_set>
#include <regex>
#include <fstream>

namespace bb {

class PopupBlockerModule : public IModule {
private:
    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};
    std::unique_ptr<std::thread> blocker_thread_;
    EventCallback event_callback_;
    uint32_t scan_interval_ms_ = 1000;

    // 弹窗黑名单规则
    struct PopupRule {
        std::string field;      // "title" / "class" / "process"
        std::string pattern;    // 正则表达式
        bool enabled;
    };
    std::vector<PopupRule> rules_;
    std::mutex rules_mutex_;

    // 已处理弹窗去重（避免反复拦截）
    std::unordered_set<uint64_t> handled_hwnds_;
    std::mutex handled_mutex_;

    uint32_t block_count_ = 0;

public:
    ModuleDescriptor get_descriptor() const override {
        ModuleDescriptor desc;
        desc.name = "popup_blocker";
        desc.version = "0.1.0";
        desc.description = "Automatic popup/ad window detection and blocking module";
        desc.author = "ButterflyBlades Team";
        desc.enabled = false;
        desc.is_loaded = false;
        desc.priority = 30;
        return desc;
    }

    std::string get_name() const override { return "popup_blocker"; }

    ErrorCode init() override {
        // 加载内置弹窗规则
        add_builtin_rules();
        initialized_.store(true);
        return ErrorCode::SUCCESS;
    }

    ErrorCode start() override {
        if (!initialized_.load()) return ErrorCode::ERR_NOT_INITIALIZED;
        running_.store(true);

        blocker_thread_ = std::make_unique<std::thread>([this]() {
            scan_loop();
        });
        return ErrorCode::SUCCESS;
    }

    ErrorCode stop() override {
        running_.store(false);
        if (blocker_thread_ && blocker_thread_->joinable()) {
            blocker_thread_->join();
            blocker_thread_.reset();
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
        try {
            std::ifstream file(config_path);
            nlohmann::json j;
            file >> j;

            std::lock_guard lock(rules_mutex_);
            for (auto& rule_json : j["rules"]) {
                PopupRule rule;
                rule.field = rule_json["field"];
                rule.pattern = rule_json["pattern"];
                rule.enabled = rule_json.value("enabled", true);
                rules_.push_back(rule);
            }
            if (j.contains("scan_interval_ms")) scan_interval_ms_ = j["scan_interval_ms"];
        } catch (...) {
            return ErrorCode::ERR_UNKNOWN;
        }
        return ErrorCode::SUCCESS;
    }

    ErrorCode save_config(const std::string& config_path) override {
        nlohmann::json j;
        j["rules"] = nlohmann::json::array();
        {
            std::lock_guard lock(rules_mutex_);
            for (auto& r : rules_) {
                nlohmann::json rule;
                rule["field"] = r.field;
                rule["pattern"] = r.pattern;
                rule["enabled"] = r.enabled;
                j["rules"].push_back(rule);
            }
        }
        j["scan_interval_ms"] = scan_interval_ms_;
        std::ofstream file(config_path);
        file << j.dump(4);
        return ErrorCode::SUCCESS;
    }

    ErrorCode handle_remote_command(const RemoteCommand& cmd, RemoteResponse& resp) override {
        resp.command_id = cmd.command_id;

        if (cmd.action == "add_rule") {
            try {
                auto j = nlohmann::json::parse(cmd.payload);
                PopupRule rule;
                rule.field = j["field"];
                rule.pattern = j["pattern"];
                rule.enabled = j.value("enabled", true);
                std::lock_guard lock(rules_mutex_);
                rules_.push_back(rule);
                resp.code = ErrorCode::SUCCESS;
            } catch (...) {
                resp.code = ErrorCode::ERR_INVALID_PARAM;
            }
        }
        else if (cmd.action == "remove_rule") {
            try {
                auto j = nlohmann::json::parse(cmd.payload);
                std::string pattern = j["pattern"];
                std::lock_guard lock(rules_mutex_);
                rules_.erase(
                    std::remove_if(rules_.begin(), rules_.end(),
                        [&](const PopupRule& r) { return r.pattern == pattern; }),
                    rules_.end());
                resp.code = ErrorCode::SUCCESS;
            } catch (...) {
                resp.code = ErrorCode::ERR_INVALID_PARAM;
            }
        }
        else if (cmd.action == "list_rules") {
            nlohmann::json j = nlohmann::json::array();
            std::lock_guard lock(rules_mutex_);
            for (auto& r : rules_) {
                nlohmann::json rule;
                rule["field"] = r.field;
                rule["pattern"] = r.pattern;
                rule["enabled"] = r.enabled;
                j.push_back(rule);
            }
            resp.data = j.dump();
            resp.code = ErrorCode::SUCCESS;
        }
        else if (cmd.action == "status") {
            nlohmann::json j;
            j["running"] = running_.load();
            j["rules_count"] = rules_.size();
            j["block_count"] = block_count_;
            resp.data = j.dump();
            resp.code = ErrorCode::SUCCESS;
        }
        else {
            resp.code = ErrorCode::ERR_INVALID_PARAM;
        }
        return resp.code;
    }

private:
    void add_builtin_rules() {
        // 常见广告弹窗特征
        struct { const char* field; const char* pattern; } defaults[] = {
            {"class", "#32770"},                          // 标准对话框
            {"title", ".*广告.*|.*推广.*|.*ad.*|.*pop.*"},
            {"title", ".*推荐.*|.*优惠.*|.*红包.*|.*福利.*"},
            {"process", ".*popup\\.exe|.*ad\\.exe"},
            {"title", ".*通知.*|.*弹窗.*|.*提醒.*"},
            {"class", ".*Popup.*|.*AdWindow.*"},
        };
        for (auto& d : defaults) {
            rules_.push_back({d.field, d.pattern, true});
        }
    }

    bool match_rule(const std::string& value, const std::string& pattern) {
        try {
            std::regex re(pattern, std::regex::icase | std::regex::ECMAScript);
            return std::regex_search(value, re);
        } catch (...) {
            return false;
        }
    }

    void scan_loop() {
        auto& svc = ServiceManager::instance();
        auto* winOps = svc.window_ops();
        auto* procOps = svc.process_ops();

        while (running_.load()) {
            auto windows = winOps->enumerate_windows();
            std::vector<PopupRule> rules_copy;
            {
                std::lock_guard lock(rules_mutex_);
                rules_copy = rules_;
            }

            for (auto& w : windows) {
                {
                    std::lock_guard lock(handled_mutex_);
                    if (handled_hwnds_.count(w.hwnd)) continue;
                }

                bool matched = false;
                std::string match_detail;

                for (auto& rule : rules_copy) {
                    if (!rule.enabled) continue;

                    std::string value;
                    if (rule.field == "title") value = w.window_title;
                    else if (rule.field == "class") value = w.window_class;
                    else if (rule.field == "process") {
                        auto info = procOps->get_process_info(w.pid);
                        value = info.name;
                    }
                    else continue;

                    if (match_rule(value, rule.pattern)) {
                        matched = true;
                        match_detail = rule.pattern;
                        break;
                    }
                }

                if (matched) {
                    w.blocked = true;
                    if (event_callback_) {
                        nlohmann::json j;
                        j["hwnd"] = w.hwnd;
                        j["title"] = w.window_title;
                        j["class"] = w.window_class;
                        j["pid"] = w.pid;
                        j["match_rule"] = match_detail;
                        event_callback_(EventType::POPUP_DETECTED, j.dump());
                    }

                    // 关闭弹窗
                    ErrorCode ret = winOps->close_window(w.hwnd);
                    if (ret == ErrorCode::SUCCESS) {
                        block_count_++;
                        if (event_callback_) {
                            nlohmann::json j;
                            j["hwnd"] = w.hwnd;
                            j["title"] = w.window_title;
                            event_callback_(EventType::POPUP_BLOCKED, j.dump());
                        }
                    }

                    {
                        std::lock_guard lock(handled_mutex_);
                        handled_hwnds_.insert(w.hwnd);
                    }
                }
            }

            // 定期清理已关闭窗口的hwnd记录
            {
                std::lock_guard lock(handled_mutex_);
                if (handled_hwnds_.size() > 10000) {
                    handled_hwnds_.clear();
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(scan_interval_ms_));
        }
    }
};

#ifndef BB_STATIC_MODULES
extern "C" IModule* create_module() {
    return new PopupBlockerModule();
}

extern "C" void destroy_module(IModule* mod) {
    delete mod;
}
#endif

} // namespace bb
