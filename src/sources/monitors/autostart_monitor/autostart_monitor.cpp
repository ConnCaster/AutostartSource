#include "monitoring/sources/monitors/autostart_monitor/autostart_monitor.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <string>
#include <unistd.h>
#include <utility>

namespace monitoring {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kBufSize = 16384;

constexpr uint64_t kAutostartActionMask =
    FAN_CREATE |
    FAN_DELETE |
    FAN_MOVED_FROM |
    FAN_MOVED_TO;

constexpr uint64_t kAutostartMarkMask =
    kAutostartActionMask |
    FAN_EVENT_ON_CHILD |
    FAN_ONDIR;

bool IsMonitoredEventOccured(uint64_t mask) {
    return (mask & kAutostartActionMask) != 0;
}

bool HasSuffix(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
        value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string NormalizeDirPath(std::string path) {
    if (path.empty()) {
        return {};
    }

    path = fs::path(path).lexically_normal().string();

    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }

    return path;
}

void NormalizeConfig(AutostartMonitorConfig& config) {
    for (auto& path : config.base_dirs) {
        path = NormalizeDirPath(std::move(path));
    }

    config.base_dirs.erase(
        std::remove_if(
            config.base_dirs.begin(),
            config.base_dirs.end(),
            [](const std::string& path) {
                return path.empty();
            }
        ),
        config.base_dirs.end()
    );

    std::sort(config.base_dirs.begin(), config.base_dirs.end());
    config.base_dirs.erase(
        std::unique(config.base_dirs.begin(), config.base_dirs.end()),
        config.base_dirs.end()
    );

    config.dependency_dir_suffixes.erase(
        std::remove_if(
            config.dependency_dir_suffixes.begin(),
            config.dependency_dir_suffixes.end(),
            [](const std::string& suffix) {
                return suffix.empty();
            }
        ),
        config.dependency_dir_suffixes.end()
    );

    std::sort(
        config.dependency_dir_suffixes.begin(),
        config.dependency_dir_suffixes.end()
    );

    config.dependency_dir_suffixes.erase(
        std::unique(
            config.dependency_dir_suffixes.begin(),
            config.dependency_dir_suffixes.end()
        ),
        config.dependency_dir_suffixes.end()
    );
}

bool IsUnderRoot(const std::string& path, const std::string& root) {
    return path == root ||
           path.rfind(root + "/", 0) == 0;
}

bool IsDirectChildOfRoot(const std::string& path, const std::string& root) {
    const std::string prefix = root + "/";

    if (path.rfind(prefix, 0) != 0) {
        return false;
    }

    const std::string tail = path.substr(prefix.size());

    return !tail.empty() && tail.find('/') == std::string::npos;
}

bool IsUsableName(const std::string& name) {
    return !name.empty() &&
           name != "." &&
           name != ".." &&
           name.find('/') == std::string::npos;
}

std::optional<std::string> ReadFdPath(int fd) {
    char proc_path[64];
    std::snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", fd);

    char path[PATH_MAX + 1];
    const ssize_t len = readlink(proc_path, path, PATH_MAX);

    if (len < 0) {
        return std::nullopt;
    }

    path[len] = '\0';
    return std::string(path);
}

std::string JoinPath(const std::string& dir, const std::string& name) {
    if (!dir.empty() && dir.back() == '/') {
        return dir + name;
    }

    return dir + "/" + name;
}

pid_t GetParentPid(pid_t pid) {
    if (pid <= 0) {
        return -1;
    }

    std::ifstream file("/proc/" + std::to_string(pid) + "/status");

    if (!file.is_open()) {
        return -1;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.rfind("PPid:", 0) == 0) {
            std::istringstream iss(line.substr(5));

            pid_t ppid = -1;
            iss >> ppid;

            return ppid;
        }
    }

    return -1;
}

EventType GetAutostartEventType(uint64_t mask) {
    if ((mask & (FAN_DELETE | FAN_MOVED_FROM)) != 0) {
        return EventType::AutostartRemoveTask;
    }

    if ((mask & (FAN_CREATE | FAN_MOVED_TO)) != 0) {
        return EventType::AutostartCreateTask;
    }

    return EventType::Unknown;
}

std::vector<std::string> CollectDependencyDirs(const AutostartMonitorConfig& config) {
    std::vector<std::string> result;

    for (const auto& base_dir : config.base_dirs) {
        std::error_code ec;

        fs::directory_iterator it(
            base_dir,
            fs::directory_options::skip_permission_denied,
            ec
        );

        if (ec) {
            std::cerr << "directory_iterator(" << base_dir << ") failed: "
                      << ec.message()
                      << std::endl;
            continue;
        }

        const fs::directory_iterator end;

        for (; it != end; it.increment(ec)) {
            if (ec) {
                std::cerr << "directory_iterator increment failed: "
                          << ec.message()
                          << std::endl;
                ec.clear();
                continue;
            }

            std::error_code entry_ec;

            if (!it->is_directory(entry_ec)) {
                continue;
            }

            const std::string path = it->path().string();

            const bool suffix_matches = std::any_of(
                config.dependency_dir_suffixes.begin(),
                config.dependency_dir_suffixes.end(),
                [&path](const std::string& suffix) {
                    return HasSuffix(path, suffix);
                }
            );

            if (suffix_matches) {
                result.push_back(path);
            }
        }
    }

    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());

    return result;
}

}  // namespace

AutostartMonitor::AutostartMonitor() {
    NormalizeConfig(config_);
}

AutostartMonitor::AutostartMonitor(AutostartMonitorConfig config)
    : config_(std::move(config)) {
    NormalizeConfig(config_);
}

bool AutostartMonitor::SetConfig(AutostartMonitorConfig config) {
    if (fan_fd_ >= 0) {
        std::cerr << "AutostartMonitor::SetConfig() must be called before Init()"
                  << std::endl;
        return false;
    }

    NormalizeConfig(config);

    if (config.base_dirs.empty()) {
        std::cerr << "AutostartMonitor config has no base directories"
                  << std::endl;
        return false;
    }

    if (config.dependency_dir_suffixes.empty()) {
        std::cerr << "AutostartMonitor config has no dependency suffixes"
                  << std::endl;
        return false;
    }

    config_ = std::move(config);
    return true;
}

bool AutostartMonitor::AddBaseDirectory(std::string path) {
    if (fan_fd_ >= 0) {
        std::cerr << "AutostartMonitor::AddBaseDirectory() must be called before Init()"
                  << std::endl;
        return false;
    }

    path = NormalizeDirPath(std::move(path));

    if (path.empty()) {
        return false;
    }

    if (std::find(config_.base_dirs.begin(), config_.base_dirs.end(), path) !=
        config_.base_dirs.end()) {
        return true;
    }

    config_.base_dirs.push_back(std::move(path));
    NormalizeConfig(config_);

    return true;
}

bool AutostartMonitor::AddDependencyDirSuffix(std::string suffix) {
    if (fan_fd_ >= 0) {
        std::cerr << "AutostartMonitor::AddDependencyDirSuffix() must be called before Init()"
                  << std::endl;
        return false;
    }

    if (suffix.empty()) {
        return false;
    }

    if (std::find(
            config_.dependency_dir_suffixes.begin(),
            config_.dependency_dir_suffixes.end(),
            suffix
        ) != config_.dependency_dir_suffixes.end()) {
        return true;
    }

    config_.dependency_dir_suffixes.push_back(std::move(suffix));
    NormalizeConfig(config_);

    return true;
}

AutostartMonitorConfig AutostartMonitor::GetConfig() const {
    return config_;
}

AutostartMonitor::~AutostartMonitor() {
    Shutdown();
}

void AutostartMonitor::Shutdown() {
    if (fan_fd_ >= 0) {
        close(fan_fd_);
        fan_fd_ = -1;
    }

    for (auto& root : base_roots_) {
        if (root.fd >= 0) {
            close(root.fd);
            root.fd = -1;
        }
    }

    for (auto& root : watched_roots_) {
        if (root.fd >= 0) {
            close(root.fd);
            root.fd = -1;
        }
    }

    base_roots_.clear();
    watched_roots_.clear();
}

int AutostartMonitor::Init() {
    Shutdown();

    NormalizeConfig(config_);

    if (config_.base_dirs.empty()) {
        std::cerr << "AutostartMonitor: no base directories configured"
                  << std::endl;
        return EINVAL;
    }

    if (config_.dependency_dir_suffixes.empty()) {
        std::cerr << "AutostartMonitor: no dependency suffixes configured"
                  << std::endl;
        return EINVAL;
    }

    const unsigned int kInitFlags =
        FAN_CLASS_NOTIF |
        FAN_CLOEXEC |
        FAN_NONBLOCK |
        FAN_REPORT_DIR_FID |
        FAN_REPORT_NAME;

    fan_fd_ = fanotify_init(kInitFlags, O_RDONLY | O_CLOEXEC | O_LARGEFILE);
    if (fan_fd_ < 0) {
        const int saved_errno = errno;
        std::cerr << "fanotify_init() failed: "
                  << std::strerror(saved_errno)
                  << std::endl;
        return saved_errno;
    }

    for (const auto& base_dir : config_.base_dirs) {
        const int rc = MarkBaseDirectory(base_dir);

        if (rc != 0) {
            Shutdown();
            return rc;
        }
    }

    const int rc = RefreshWatches();

    if (rc != 0) {
        Shutdown();
    }

    return rc;
}

int AutostartMonitor::MarkBaseDirectory(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);

    if (fd < 0) {
        const int saved_errno = errno;
        std::cerr << "open(" << path << ") failed: "
                  << std::strerror(saved_errno)
                  << std::endl;
        return saved_errno;
    }

    if (fanotify_mark(
            fan_fd_,
            FAN_MARK_ADD | FAN_MARK_ONLYDIR,
            kAutostartMarkMask,
            AT_FDCWD,
            path.c_str()
        ) < 0) {
        const int saved_errno = errno;

        std::cerr << "fanotify_mark(" << path << ") failed: "
                  << std::strerror(saved_errno)
                  << " errno=" << saved_errno
                  << std::endl;

        close(fd);
        return saved_errno;
        }

    base_roots_.push_back({path, fd});

    std::cout << "autostart base directory marked successfully: "
              << path
              << std::endl;

    return 0;
}

int AutostartMonitor::MarkDependencyDirectory(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);

    if (fd < 0) {
        const int saved_errno = errno;
        std::cerr << "open(" << path << ") failed: "
                  << std::strerror(saved_errno)
                  << std::endl;
        return saved_errno;
    }

    if (fanotify_mark(
            fan_fd_,
            FAN_MARK_ADD | FAN_MARK_ONLYDIR,
            kAutostartMarkMask,
            AT_FDCWD,
            path.c_str()
        ) < 0) {
        const int saved_errno = errno;

        std::cerr << "fanotify_mark(" << path << ") failed: "
                  << std::strerror(saved_errno)
                  << " errno=" << saved_errno
                  << std::endl;

        close(fd);
        return saved_errno;
        }

    watched_roots_.push_back({path, fd});

    std::cout << "autostart dependency directory marked successfully: "
              << path
              << std::endl;

    return 0;
}

int AutostartMonitor::RefreshWatches() {
    for (auto& root : watched_roots_) {
        if (root.fd >= 0) {
            close(root.fd);
            root.fd = -1;
        }
    }

    watched_roots_.clear();

    const auto dirs = CollectDependencyDirs(config_);

    int first_error = 0;

    for (const auto& dir : dirs) {
        const int rc = MarkDependencyDirectory(dir);

        if (rc != 0 && first_error == 0) {
            first_error = rc;
        }
    }

    /*
     * Отсутствие dependency-директорий не считаем ошибкой.
     * Базовые директории уже промаркированы, поэтому при создании новой
     * директории с нужным суффиксом монитор сможет вызвать RefreshWatches().
     */
    return first_error;
}

bool AutostartMonitor::IsDependencyDirectory(const std::string& path) const {
    return std::any_of(
        config_.dependency_dir_suffixes.begin(),
        config_.dependency_dir_suffixes.end(),
        [&path](const std::string& suffix) {
            return HasSuffix(path, suffix);
        }
    );
}

bool AutostartMonitor::IsDirectChildOfBaseRoot(const std::string& path) const {
    for (const auto& base_dir : config_.base_dirs) {
        if (IsDirectChildOfRoot(path, base_dir)) {
            return true;
        }
    }

    return false;
}

bool AutostartMonitor::IsUnderWatchedDependencyRoot(
        const std::string& path
    ) const {
    for (const auto& root : watched_roots_) {
        if (IsUnderRoot(path, root.path) && path != root.path) {
            return true;
        }
    }

    return false;
}

std::optional<std::string> AutostartMonitor::ResolvePathFromDfidName(
    const fanotify_event_info_fid* fid
) const {
    if (fid == nullptr) {
        return std::nullopt;
    }

    const auto* handle = reinterpret_cast<const file_handle*>(fid->handle);
    const char* record_begin = reinterpret_cast<const char*>(fid);
    const char* record_end = record_begin + fid->hdr.len;
    const char* name_begin = reinterpret_cast<const char*>(handle->f_handle) + handle->handle_bytes;

    if (name_begin >= record_end) {
        return std::nullopt;
    }

    const std::size_t max_name_len = static_cast<std::size_t>(record_end - name_begin);
    const std::size_t name_len = strnlen(name_begin, max_name_len);

    if (name_len == max_name_len) {
        return std::nullopt;
    }

    std::string name{name_begin, name_len};

    if (!IsUsableName(name)) {
        return std::nullopt;
    }

    std::vector<int> mount_fds;

    for (const auto& root : base_roots_) {
        if (root.fd >= 0) {
            mount_fds.push_back(root.fd);
        }
    }

    for (const auto& root : watched_roots_) {
        if (root.fd >= 0) {
            mount_fds.push_back(root.fd);
        }
    }

    for (int mount_fd : mount_fds) {
        int parent_fd = open_by_handle_at(
            mount_fd,
            const_cast<file_handle*>(handle),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC
        );

        if (parent_fd < 0) {
            continue;
        }

        auto parent_path = ReadFdPath(parent_fd);
        close(parent_fd);

        if (!parent_path.has_value()) {
            continue;
        }

        return JoinPath(parent_path.value(), name);
    }

    return std::nullopt;
}

void AutostartMonitor::PollOnce(
    std::vector<AutostartRawEvent>& changes,
    int timeout_ms
) {
    if (fan_fd_ < 0) {
        std::cerr << "fanotify descriptor is not initialized" << std::endl;
        return;
    }

    pollfd poll_fd {};
    poll_fd.fd = fan_fd_;
    poll_fd.events = POLLIN;

    const int poll_rc = poll(&poll_fd, 1, timeout_ms);

    if (poll_rc < 0) {
        const int saved_errno = errno;

        if (saved_errno == EINTR) {
            return;
        }

        std::cerr << "poll() failed: "
                  << std::strerror(saved_errno)
                  << std::endl;
        return;
    }

    if (poll_rc == 0) {
        return;
    }

    if (poll_fd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        std::cerr << "fanotify fd error, revents=0x"
                  << std::hex << poll_fd.revents
                  << std::dec << std::endl;
        return;
    }

    char buffer[kBufSize];

    for (;;) {
        const ssize_t len = read(fan_fd_, buffer, sizeof(buffer));

        if (len < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                return;
            }

            const int saved_errno = errno;
            std::cerr << "fanotify read error: "
                      << std::strerror(saved_errno)
                      << std::endl;
            return;
        }

        if (len == 0) {
            return;
        }

        auto* metadata = reinterpret_cast<fanotify_event_metadata*>(buffer);
        ssize_t remain = len;

        while (FAN_EVENT_OK(metadata, remain)) {
            if (metadata->vers != FANOTIFY_METADATA_VERSION) {
                std::cerr << "fanotify metadata version mismatch" << std::endl;
                return;
            }

            if ((metadata->mask & FAN_Q_OVERFLOW) != 0) {
                std::cerr << "fanotify queue overflow" << std::endl;
                metadata = FAN_EVENT_NEXT(metadata, remain);
                continue;
            }

            if (!IsMonitoredEventOccured(metadata->mask)) {
                metadata = FAN_EVENT_NEXT(metadata, remain);
                continue;
            }

            const EventType event_type = GetAutostartEventType(metadata->mask);

            if (event_type == EventType::Unknown) {
                metadata = FAN_EVENT_NEXT(metadata, remain);
                continue;
            }

            const char* info_ptr = reinterpret_cast<const char*>(metadata) + metadata->metadata_len;
            const char* event_end = reinterpret_cast<const char*>(metadata) + metadata->event_len;

            while (info_ptr + sizeof(fanotify_event_info_header) <= event_end) {
                const auto* hdr = reinterpret_cast<const fanotify_event_info_header*>(info_ptr);

                if (hdr->len < sizeof(fanotify_event_info_header) ||
                    info_ptr + hdr->len > event_end) {
                    break;
                }

                if (hdr->info_type == FAN_EVENT_INFO_TYPE_DFID_NAME) {
                    const auto* fid = reinterpret_cast<const fanotify_event_info_fid*>(info_ptr);

                    auto path = ResolvePathFromDfidName(fid);

                    if (!path.has_value()) {
                        info_ptr += hdr->len;
                        continue;
                    }

                    /*
                     * Если в /etc/systemd/system появилась новая директория
                     * вида *.wants или *.requires, обновляем список watched roots.
                     * Саму директорию как событие автозапуска не отправляем.
                     */
                    if ((metadata->mask & FAN_ONDIR) != 0) {
                        std::cout << "autostart directory event: mask=0x"
                              << std::hex << metadata->mask
                              << std::dec
                              << " path=" << path.value()
                              << std::endl;
                        if (IsDirectChildOfBaseRoot(path.value()) &&
                            IsDependencyDirectory(path.value())) {
                            std::cout << "autostart dependency dir changed, refreshing watches: "
                                  << path.value()
                                  << std::endl;
                            RefreshWatches();
                            }

                        info_ptr += hdr->len;
                        continue;
                    }

                    /*
                     * Нужны только файлы/ссылки внутри:
                     * /etc/systemd/system/*.wants/
                     * /etc/systemd/system/*.requires/
                     */
                    if (IsUnderWatchedDependencyRoot(path.value())) {
                        AutostartRawEvent change;
                        change.path = std::move(path.value());
                        change.pid = metadata->pid;
                        change.ppid = GetParentPid(metadata->pid);
                        change.type = event_type;

                        changes.push_back(std::move(change));
                    }
                }

                info_ptr += hdr->len;
            }

            metadata = FAN_EVENT_NEXT(metadata, remain);
        }
    }
}

}  // namespace monitoring