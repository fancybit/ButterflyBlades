// ButterflyBlades Windows 平台实现
// 进程/窗口/文件/驱动/网络/系统操作

#ifdef _WIN32

#include <windows.h>
// Ensure NTSTATUS is properly defined before d3dkmthk.h (WORKAROUND for WIN32_LEAN_AND_MEAN)
#ifndef _NTDEF_
typedef _Return_type_success_(return >= 0) LONG NTSTATUS;
#define _NTDEF_
#endif
#include <d3dkmthk.h>
#include "../../include/core/core.h"
#include <psapi.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <wincrypt.h>
#include <iostream>
#include <sstream>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "gdi32.lib")

namespace bb::platform {

// ==================== ProcessOps ====================
class WindowsProcessOps : public ProcessOps {
public:
    std::vector<ProcessInfo> enumerate_processes() override {
        std::vector<ProcessInfo> result;
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return result;

        PROCESSENTRY32W pe32{};
        pe32.dwSize = sizeof(pe32);

        if (Process32FirstW(snapshot, &pe32)) {
            do {
                ProcessInfo info = get_process_info(pe32.th32ProcessID);
                result.push_back(std::move(info));
            } while (Process32NextW(snapshot, &pe32));
        }
        CloseHandle(snapshot);
        return result;
    }

    ProcessInfo get_process_info(uint32_t pid) override {
        ProcessInfo info{};
        info.pid = pid;
        info.snapshot_time = std::chrono::steady_clock::now();

        HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
        if (!hProcess) {
            info.name = "<access denied>";
            info.path = "<unknown>";
            return info;
        }

        // 进程名
        WCHAR exePath[MAX_PATH];
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(hProcess, 0, exePath, &size)) {
            int len = WideCharToMultiByte(CP_UTF8, 0, exePath, -1, nullptr, 0, nullptr, nullptr);
            info.path.resize(len - 1);
            WideCharToMultiByte(CP_UTF8, 0, exePath, -1, &info.path[0], len, nullptr, nullptr);

            std::string full(info.path);
            auto pos = full.find_last_of("\\/");
            info.name = (pos != std::string::npos) ? full.substr(pos + 1) : full;
        }

        // 内存信息
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof(pmc);
        if (GetProcessMemoryInfo(hProcess, (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
            info.memory_mb = pmc.WorkingSetSize / (1024 * 1024);
        }

        info.thread_count = get_threads(pid).size();
        info.cpu_usage = get_process_cpu(pid);
        info.is_system = (pid == 0 || pid == 4);
        info.gpu_memory_mb = get_process_gpu_memory(pid);

        CloseHandle(hProcess);
        return info;
    }

    ErrorCode kill_process(uint32_t pid, bool force) override {
        DWORD desiredAccess = PROCESS_TERMINATE;
        if (force) desiredAccess |= PROCESS_QUERY_INFORMATION;

        HANDLE hProcess = OpenProcess(desiredAccess, FALSE, pid);
        if (!hProcess) {
            return ErrorCode::ERR_ACCESS_DENIED;
        }

        UINT exitCode = force ? 1 : 0;
        BOOL ok = TerminateProcess(hProcess, exitCode);
        CloseHandle(hProcess);

        return ok ? ErrorCode::SUCCESS : ErrorCode::ERR_UNKNOWN;
    }

    double get_process_cpu(uint32_t pid) override {
        HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
        if (!hProcess) return 0.0;

        FILETIME createTime, exitTime, kernelTime, userTime;
        if (!GetProcessTimes(hProcess, &createTime, &exitTime, &kernelTime, &userTime)) {
            CloseHandle(hProcess);
            return 0.0;
        }

        ULARGE_INTEGER k, u;
        k.LowPart = kernelTime.dwLowDateTime;
        k.HighPart = kernelTime.dwHighDateTime;
        u.LowPart = userTime.dwLowDateTime;
        u.HighPart = userTime.dwHighDateTime;

        uint64_t totalTime = k.QuadPart + u.QuadPart;
        CloseHandle(hProcess);

        // 简易CPU计算（基于采样间隔）
        static std::unordered_map<uint32_t, std::pair<uint64_t, std::chrono::steady_clock::time_point>> cache;
        auto& [prevTime, prevStamp] = cache[pid];
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - prevStamp).count();

        double cpu = 0.0;
        if (elapsed > 0 && prevTime > 0) {
            uint64_t delta = totalTime - prevTime;
            cpu = (delta * 100.0) / elapsed;
        }

        prevTime = totalTime;
        prevStamp = now;
        return cpu;
    }

    uint64_t get_process_memory(uint32_t pid) override {
        return get_process_info(pid).memory_mb;
    }

    uint64_t get_process_gpu_memory(uint32_t pid) override {
        // Windows GPU 显存查询（简化实现）
        // 完整实现需通过 DXGI / D3DKMTQueryStatistics
        return 0;
    }

    std::vector<ThreadInfo> get_threads(uint32_t pid) override {
        std::vector<ThreadInfo> result;
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return result;

        THREADENTRY32 te32{};
        te32.dwSize = sizeof(te32);

        if (Thread32First(snapshot, &te32)) {
            do {
                if (te32.th32OwnerProcessID == pid) {
                    ThreadInfo ti{};
                    ti.tid = te32.th32ThreadID;
                    ti.pid = pid;
                    ti.last_active = std::chrono::steady_clock::now();
                    result.push_back(ti);
                }
            } while (Thread32Next(snapshot, &te32));
        }
        CloseHandle(snapshot);
        return result;
    }

    ErrorCode suspend_process(uint32_t pid) override {
        // 挂起所有线程
        auto threads = get_threads(pid);
        for (auto& t : threads) {
            HANDLE hThread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, t.tid);
            if (hThread) {
                SuspendThread(hThread);
                CloseHandle(hThread);
            }
        }
        return ErrorCode::SUCCESS;
    }

    ErrorCode resume_process(uint32_t pid) override {
        auto threads = get_threads(pid);
        for (auto& t : threads) {
            HANDLE hThread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, t.tid);
            if (hThread) {
                ResumeThread(hThread);
                CloseHandle(hThread);
            }
        }
        return ErrorCode::SUCCESS;
    }
};

// ==================== WindowOps ====================
class WindowsWindowOps : public WindowOps {
public:
    std::vector<PopupInfo> enumerate_windows() override {
        std::vector<PopupInfo> result;

        EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
            auto* result = reinterpret_cast<std::vector<PopupInfo>*>(lParam);
            if (!IsWindowVisible(hwnd)) return TRUE;

            LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
            // 过滤掉没有标题栏的窗口 / 桌面窗口
            if (!(style & WS_CAPTION)) return TRUE;

            WCHAR title[256];
            GetWindowTextW(hwnd, title, 256);
            if (wcslen(title) == 0) return TRUE;

            WCHAR cls[256];
            GetClassNameW(hwnd, cls, 256);

            PopupInfo info{};
            info.hwnd = reinterpret_cast<uint64_t>(hwnd);
            info.detected_time = std::chrono::steady_clock::now();
            info.blocked = false;

            int len = WideCharToMultiByte(CP_UTF8, 0, title, -1, nullptr, 0, nullptr, nullptr);
            info.window_title.resize(len - 1);
            WideCharToMultiByte(CP_UTF8, 0, title, -1, &info.window_title[0], len, nullptr, nullptr);

            len = WideCharToMultiByte(CP_UTF8, 0, cls, -1, nullptr, 0, nullptr, nullptr);
            info.window_class.resize(len - 1);
            WideCharToMultiByte(CP_UTF8, 0, cls, -1, &info.window_class[0], len, nullptr, nullptr);

            GetWindowThreadProcessId(hwnd, reinterpret_cast<LPDWORD>(&info.pid));
            result->push_back(std::move(info));
            return TRUE;
        }, reinterpret_cast<LPARAM>(&result));

        return result;
    }

    ErrorCode close_window(uint64_t hwnd) override {
        HWND h = reinterpret_cast<HWND>(hwnd);
        PostMessage(h, WM_CLOSE, 0, 0);
        return ErrorCode::SUCCESS;
    }

    std::string get_window_title(uint64_t hwnd) override {
        WCHAR title[256];
        GetWindowTextW(reinterpret_cast<HWND>(hwnd), title, 256);
        int len = WideCharToMultiByte(CP_UTF8, 0, title, -1, nullptr, 0, nullptr, nullptr);
        std::string result(len - 1, 0);
        WideCharToMultiByte(CP_UTF8, 0, title, -1, &result[0], len, nullptr, nullptr);
        return result;
    }

    std::string get_window_class(uint64_t hwnd) override {
        WCHAR cls[256];
        GetClassNameW(reinterpret_cast<HWND>(hwnd), cls, 256);
        int len = WideCharToMultiByte(CP_UTF8, 0, cls, -1, nullptr, 0, nullptr, nullptr);
        std::string result(len - 1, 0);
        WideCharToMultiByte(CP_UTF8, 0, cls, -1, &result[0], len, nullptr, nullptr);
        return result;
    }

    uint32_t get_window_pid(uint64_t hwnd) override {
        DWORD pid = 0;
        GetWindowThreadProcessId(reinterpret_cast<HWND>(hwnd), &pid);
        return pid;
    }

    ErrorCode send_message(uint64_t hwnd, uint32_t msg, uint64_t wparam, int64_t lparam) override {
        PostMessage(reinterpret_cast<HWND>(hwnd), msg, (WPARAM)wparam, (LPARAM)lparam);
        return ErrorCode::SUCCESS;
    }
};

// ==================== FileOps ====================
class WindowsFileOps : public FileOps {
public:
    std::string compute_sha256(const std::string& path) override {
        HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) return "";

        HCRYPTPROV hProv = 0;
        HCRYPTHASH hHash = 0;
        std::string result;

        if (CryptAcquireContext(&hProv, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
            if (CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
                BYTE buffer[4096];
                DWORD bytesRead;
                while (ReadFile(hFile, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0) {
                    CryptHashData(hHash, buffer, bytesRead, 0);
                }

                BYTE hash[32];
                DWORD hashLen = 32;
                if (CryptGetHashParam(hHash, HP_HASHVAL, hash, &hashLen, 0)) {
                    std::ostringstream oss;
                    for (DWORD i = 0; i < hashLen; i++) {
                        oss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
                    }
                    result = oss.str();
                }
                CryptDestroyHash(hHash);
            }
            CryptReleaseContext(hProv, 0);
        }
        CloseHandle(hFile);
        return result;
    }

    std::vector<std::string> enumerate_files(const std::string& dir, bool recursive) override {
        std::vector<std::string> result;
        std::string pattern = dir + "\\*";

        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(pattern.c_str(), &fd);
        if (hFind == INVALID_HANDLE_VALUE) return result;

        do {
            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;

            std::string fullPath = dir + "\\" + fd.cFileName;

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (recursive) {
                    auto sub = enumerate_files(fullPath, true);
                    result.insert(result.end(), sub.begin(), sub.end());
                }
            } else {
                result.push_back(fullPath);
            }
        } while (FindNextFileA(hFind, &fd));

        FindClose(hFind);
        return result;
    }

    bool file_exists(const std::string& path) override {
        DWORD attr = GetFileAttributesA(path.c_str());
        return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
    }

    uint64_t file_size(const std::string& path) override {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fad)) return 0;
        return ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    }

    bool open_read_only(const std::string& path, std::vector<uint8_t>& buffer) override {
        HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) return false;

        DWORD size = GetFileSize(hFile, nullptr);
        buffer.resize(size);
        DWORD bytesRead;
        BOOL ok = ReadFile(hFile, buffer.data(), size, &bytesRead, nullptr);
        CloseHandle(hFile);
        return ok && bytesRead == size;
    }

    void* watch_directory(const std::string& dir, FileWatchCallback cb) override {
        // Windows ReadDirectoryChangesW 实现简化版
        return nullptr;
    }

    void unwatch_directory(void* handle) override {
        // 简化实现
    }
};

// ==================== DriverOps ====================
class WindowsDriverOps : public DriverOps {
public:
    ErrorCode load_driver(const std::string& driver_path, const std::string& service_name) override {
        SC_HANDLE hSCManager = OpenSCManager(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
        if (!hSCManager) return ErrorCode::ERR_DRIVER_FAIL;

        std::wstring wName(service_name.begin(), service_name.end());
        std::wstring wPath(driver_path.begin(), driver_path.end());

        SC_HANDLE hService = CreateServiceW(
            hSCManager, wName.c_str(), wName.c_str(),
            SERVICE_START | SERVICE_STOP | DELETE,
            SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
            wPath.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);

        if (!hService) {
            hService = OpenServiceW(hSCManager, wName.c_str(), SERVICE_START);
        }

        if (hService) {
            StartService(hService, 0, nullptr);
            CloseServiceHandle(hService);
        }
        CloseServiceHandle(hSCManager);

        return (hService != nullptr) ? ErrorCode::SUCCESS : ErrorCode::ERR_DRIVER_FAIL;
    }

    ErrorCode unload_driver(const std::string& service_name) override {
        SC_HANDLE hSCManager = OpenSCManager(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!hSCManager) return ErrorCode::ERR_DRIVER_FAIL;

        std::wstring wName(service_name.begin(), service_name.end());
        SC_HANDLE hService = OpenServiceW(hSCManager, wName.c_str(), SERVICE_STOP | DELETE);
        if (hService) {
            SERVICE_STATUS status;
            ControlService(hService, SERVICE_CONTROL_STOP, &status);
            DeleteService(hService);
            CloseServiceHandle(hService);
        }
        CloseServiceHandle(hSCManager);
        return ErrorCode::SUCCESS;
    }

    ErrorCode ioctl(uint32_t control_code, const std::vector<uint8_t>& in_data,
                    std::vector<uint8_t>& out_data) override {
        // 需要驱动符号链接
        return ErrorCode::ERR_DRIVER_FAIL;
    }

    std::vector<ProcessInfo> enumerate_processes_kernel() override {
        // 驱动层进程枚举（检测隐藏进程）
        return {};
    }

    std::vector<std::string> enumerate_loaded_modules() override {
        return {};
    }

    bool is_driver_supported() override {
        return true;
    }
};

// ==================== NetworkOps ====================
class WindowsNetworkOps : public NetworkOps {
private:
    SOCKET socket_;
    SOCKET listen_socket_;
    bool connected_{false};

    static bool wsa_initialized_;
    static void init_wsa() {
        if (!wsa_initialized_) {
            WSADATA wsaData;
            WSAStartup(MAKEWORD(2, 2), &wsaData);
            wsa_initialized_ = true;
        }
    }

public:
    WindowsNetworkOps() : socket_(INVALID_SOCKET), listen_socket_(INVALID_SOCKET) {
        init_wsa();
    }

    ~WindowsNetworkOps() {
        tcp_disconnect();
        stop_listener();
    }

    ErrorCode tcp_connect(const std::string& host, uint16_t port) override {
        socket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket_ == INVALID_SOCKET) return ErrorCode::ERR_NETWORK_FAIL;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

        if (::connect(socket_, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
            return ErrorCode::ERR_NETWORK_FAIL;
        }
        connected_ = true;
        return ErrorCode::SUCCESS;
    }

    ErrorCode tcp_disconnect() override {
        if (socket_ != INVALID_SOCKET) {
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
        }
        connected_ = false;
        return ErrorCode::SUCCESS;
    }

    ErrorCode tcp_send(const std::vector<uint8_t>& data) override {
        if (!connected_) return ErrorCode::ERR_NETWORK_FAIL;
        int sent = ::send(socket_, (const char*)data.data(), (int)data.size(), 0);
        return (sent == (int)data.size()) ? ErrorCode::SUCCESS : ErrorCode::ERR_NETWORK_FAIL;
    }

    ErrorCode tcp_recv(std::vector<uint8_t>& data, uint32_t timeout_ms) override {
        if (!connected_) return ErrorCode::ERR_NETWORK_FAIL;

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(socket_, &fds);

        timeval tv{};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        if (select(0, &fds, nullptr, nullptr, &tv) <= 0) {
            data.clear();
            return ErrorCode::ERR_NETWORK_FAIL;
        }

        uint8_t buffer[65536];
        int recvd = ::recv(socket_, (char*)buffer, sizeof(buffer), 0);
        if (recvd <= 0) {
            data.clear();
            tcp_disconnect();
            return ErrorCode::ERR_NETWORK_FAIL;
        }
        data.assign(buffer, buffer + recvd);
        return ErrorCode::SUCCESS;
    }

    ErrorCode start_listener(uint16_t port, const std::string& bind_addr) override {
        listen_socket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_socket_ == INVALID_SOCKET) return ErrorCode::ERR_NETWORK_FAIL;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr);

        if (::bind(listen_socket_, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
            closesocket(listen_socket_);
            listen_socket_ = INVALID_SOCKET;
            return ErrorCode::ERR_NETWORK_FAIL;
        }

        if (::listen(listen_socket_, SOMAXCONN) == SOCKET_ERROR) {
            closesocket(listen_socket_);
            listen_socket_ = INVALID_SOCKET;
            return ErrorCode::ERR_NETWORK_FAIL;
        }
        return ErrorCode::SUCCESS;
    }

    ErrorCode stop_listener() override {
        if (listen_socket_ != INVALID_SOCKET) {
            closesocket(listen_socket_);
            listen_socket_ = INVALID_SOCKET;
        }
        return ErrorCode::SUCCESS;
    }

    bool is_connected() override { return connected_; }
};

bool WindowsNetworkOps::wsa_initialized_ = false;

// ==================== SystemOps ====================
class WindowsSystemOps : public SystemOps {
public:
    uint64_t get_total_memory() override {
        MEMORYSTATUSEX mem{};
        mem.dwLength = sizeof(mem);
        GlobalMemoryStatusEx(&mem);
        return mem.ullTotalPhys / (1024 * 1024);
    }

    uint64_t get_available_memory() override {
        MEMORYSTATUSEX mem{};
        mem.dwLength = sizeof(mem);
        GlobalMemoryStatusEx(&mem);
        return mem.ullAvailPhys / (1024 * 1024);
    }

    double get_system_cpu_usage() override {
        // 简化实现
        return 0.0;
    }

    std::string get_os_name() override {
        return "Windows 11";
    }

    std::string get_hostname() override {
        char buf[256];
        DWORD size = sizeof(buf);
        GetComputerNameA(buf, &size);
        return buf;
    }

    std::string get_current_user() override {
        char buf[256];
        DWORD size = sizeof(buf);
        GetUserNameA(buf, &size);
        return buf;
    }
};

// ==================== 平台工厂 ====================
std::unique_ptr<ProcessOps> create_process_ops() {
    return std::make_unique<WindowsProcessOps>();
}
std::unique_ptr<WindowOps> create_window_ops() {
    return std::make_unique<WindowsWindowOps>();
}
std::unique_ptr<FileOps> create_file_ops() {
    return std::make_unique<WindowsFileOps>();
}
std::unique_ptr<DriverOps> create_driver_ops() {
    return std::make_unique<WindowsDriverOps>();
}
std::unique_ptr<NetworkOps> create_network_ops() {
    return std::make_unique<WindowsNetworkOps>();
}
std::unique_ptr<SystemOps> create_system_ops() {
    return std::make_unique<WindowsSystemOps>();
}

} // namespace bb::platform

#endif // _WIN32
