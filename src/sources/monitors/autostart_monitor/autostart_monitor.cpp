#include "monitoring/sources/monitors/autostart_monitor/autostart_monitor.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <glob.h>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <string>
#include <unistd.h>
#include <utility>

namespace monitoring {
namespace {

constexpr std::size_t kBufSize = 16384;
constexpr const char* kAutostartSystemdRoot = "/etc/systemd/system";
constexpr const char* kSystemdSuffixWants = ".wants";
constexpr const char* kSystemdSuffixRequires = ".requires";
constexpr const char* kSystemdDependencyWants = "/etc/systemd/system/*.wants";
constexpr const char* kSystemdDependencyRequires = "/etc/systemd/system/*.requires";


constexpr uint64_t kAutostartActionMask =
    FAN_CREATE |
    FAN_DELETE |
    FAN_MOVED_FROM |
    FAN_MOVED_TO;

constexpr uint64_t kAutostartMarkMask =
    kAutostartActionMask |
   FAN_EVENT_ON_CHILD |
   FAN_ONDIR;

bool IsInterestingChange(uint64_t mask) {
    return (mask & kAutostartActionMask) != 0;
}

bool HasSuffix(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool IsSystemdDependencyDir(const std::string& path) {
    return HasSuffix(path, kSystemdSuffixWants) || HasSuffix(path, kSystemdSuffixRequires);
}

bool IsUsableName(const std::string& name) {
    return !name.empty() &&
           name != "." &&
           name != ".." &&
           name.find('/') == std::string::npos;
}

bool IsDirectory(const std::string& path) {
    std::error_code ec;
    return std::filesystem::is_directory(path, ec);
}

bool IsUnderRoot(const std::string& path, const std::string& root) {
    return path == root ||
           path.rfind(root + "/", 0) == 0;
}

bool IsDirectChildOfSystemdRoot(const std::string& path) {
    const std::string root{kAutostartSystemdRoot};
    const std::string prefix = root + "/";

    if (path.rfind(prefix, 0) != 0) {
        return false;
    }

    const std::string tail = path.substr(prefix.size());
    return !tail.empty() && tail.find('/') == std::string::npos;
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

std::string JoinDirAndFilePath(const std::string& dir, const std::string& name) {
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

void AddGlobMatches(std::vector<std::string>& result, const char* pattern) {
    glob_t glob_result {};
    const int rc = glob(pattern, 0, nullptr, &glob_result);

    if (rc == 0) {
        for (std::size_t i = 0; i < glob_result.gl_pathc; ++i) {
            std::string path = glob_result.gl_pathv[i];

            if (IsDirectory(path) && IsSystemdDependencyDir(path)) {
                result.push_back(std::move(path));
            }
        }
    } else if (rc != GLOB_NOMATCH) {
        std::cerr << "glob(" << pattern << ") failed, rc=" << rc << std::endl;
    }

    globfree(&glob_result);
}

std::vector<std::string> CollectSystemdDependencyDirs() {
    std::vector<std::string> result;

    AddGlobMatches(result, kSystemdDependencyWants);
    AddGlobMatches(result, kSystemdDependencyRequires);

    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());

    return result;
}

}  // namespace

AutostartMonitor::~AutostartMonitor() {
    Shutdown();
}

void AutostartMonitor::Shutdown() {
    if (fan_fd_ >= 0) {
        close(fan_fd_);
        fan_fd_ = -1;
    }

    if (systemd_root_fd_ >= 0) {
        close(systemd_root_fd_);
        systemd_root_fd_ = -1;
    }

    for (auto& root : watched_roots_) {
        if (root.fd >= 0) {
            close(root.fd);
            root.fd = -1;
        }
    }

    watched_roots_.clear();
}

int AutostartMonitor::Init() {
    Shutdown();

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

    systemd_root_fd_ = open(
        kAutostartSystemdRoot,
        O_RDONLY | O_DIRECTORY | O_CLOEXEC
    );

    if (systemd_root_fd_ < 0) {
        const int saved_errno = errno;
        std::cerr << "open(" << kAutostartSystemdRoot << ") failed: "
                  << std::strerror(saved_errno)
                  << std::endl;
        Shutdown();
        return saved_errno;
    }

    /*
     * Маркируем сам /etc/systemd/system, чтобы заметить появление новых
     * *.wants / *.requires директорий и добавить их в watched_roots_.
     * События по обычным unit-файлам из /etc/systemd/system ниже не генерируются.
     */
    if (fanotify_mark(
            fan_fd_,
            FAN_MARK_ADD | FAN_MARK_ONLYDIR,
            kAutostartMarkMask,
            AT_FDCWD,
            kAutostartSystemdRoot
        ) < 0) {
        const int saved_errno = errno;

        std::cerr << "fanotify_mark(" << kAutostartSystemdRoot << ") failed: "
                  << std::strerror(saved_errno)
                  << " errno=" << saved_errno
                  << std::endl;

        Shutdown();
        return saved_errno;
    }

    return RefreshWatches();
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

    const auto dirs = CollectSystemdDependencyDirs();

    int first_error = 0;

    for (const auto& dir : dirs) {
        const int rc = MarkDependencyDirectory(dir);

        if (rc != 0 && first_error == 0) {
            first_error = rc;
        }
    }

    /*
     * Не считаем отсутствие *.wants / *.requires фатальной ошибкой:
     * /etc/systemd/system уже отмечен, и при появлении новой директории
     * RefreshWatches() будет вызван снова.
     */
    return first_error;
}

bool AutostartMonitor::IsUnderWatchedDependencyRoot(const std::string& path) const {
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

    const char* name_begin =
        reinterpret_cast<const char*>(handle->f_handle) +
        handle->handle_bytes;

    if (name_begin >= record_end) {
        return std::nullopt;
    }

    const std::size_t max_name_len =
        static_cast<std::size_t>(record_end - name_begin);

    const std::size_t name_len = strnlen(name_begin, max_name_len);

    if (name_len == max_name_len) {
        return std::nullopt;
    }

    std::string name{name_begin, name_len};

    if (!IsUsableName(name)) {
        return std::nullopt;
    }

    std::vector<int> mount_fds;

    if (systemd_root_fd_ >= 0) {
        mount_fds.push_back(systemd_root_fd_);
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

        return JoinDirAndFilePath(*parent_path, name);
    }

    return std::nullopt;
}

void AutostartMonitor::PollOnce(
    std::vector<AutostartRawChange>& changes,
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

            if (!IsInterestingChange(metadata->mask)) {
                metadata = FAN_EVENT_NEXT(metadata, remain);
                continue;
            }

            const EventType event_type = GetAutostartEventType(metadata->mask);

            if (event_type == EventType::Unknown) {
                metadata = FAN_EVENT_NEXT(metadata, remain);
                continue;
            }

            const char* info_ptr =
                reinterpret_cast<const char*>(metadata) + metadata->metadata_len;

            const char* event_end =
                reinterpret_cast<const char*>(metadata) + metadata->event_len;

            while (info_ptr + sizeof(fanotify_event_info_header) <= event_end) {
                const auto* hdr =
                    reinterpret_cast<const fanotify_event_info_header*>(info_ptr);

                if (hdr->len < sizeof(fanotify_event_info_header) ||
                    info_ptr + hdr->len > event_end) {
                    break;
                }

                if (hdr->info_type == FAN_EVENT_INFO_TYPE_DFID_NAME) {
                    const auto* fid =
                        reinterpret_cast<const fanotify_event_info_fid*>(info_ptr);

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
                                  << " path=" << *path
                                  << std::endl;

                        if (IsDirectChildOfSystemdRoot(*path) &&
                            IsSystemdDependencyDir(*path)) {
                            std::cout << "autostart dependency dir changed, refreshing watches: "
                                      << *path
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
                        AutostartRawChange change;
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