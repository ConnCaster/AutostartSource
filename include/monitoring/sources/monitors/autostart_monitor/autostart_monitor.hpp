#ifndef AUTOSTART_MONITOR_HPP
#define AUTOSTART_MONITOR_HPP

#include <optional>
#include <string>
#include <sys/fanotify.h>
#include <sys/types.h>
#include <vector>

#include "monitoring/events/monitor_event.hpp"

namespace monitoring {

inline constexpr const char* kDefaultAutostartRoot = "/etc/systemd/system";
inline constexpr const char* kDefaultWantsSuffix = ".wants";
inline constexpr const char* kDefaultRequiresSuffix = ".requires";
constexpr const char* kSystemdDependencyWants = "/etc/systemd/system/*.wants";
constexpr const char* kSystemdDependencyRequires = "/etc/systemd/system/*.requires";

struct AutostartMonitorConfig {
    std::vector<std::string> base_dirs{
        // kDefaultAutostartRoot
    };

    std::vector<std::string> dependency_dir_suffixes{
        // kDefaultWantsSuffix,
        // kDefaultRequiresSuffix
    };
};

struct AutostartRawChange {
    std::string path;
    pid_t pid = 0;
    pid_t ppid = 0;
    EventType type = EventType::Unknown;
};

class AutostartMonitor {
public:
    AutostartMonitor();
    explicit AutostartMonitor(AutostartMonitorConfig config);
    ~AutostartMonitor();

    /*
     * Методы настройки.
     * Их нужно вызывать до Init() / до запуска AutostartSource.
     */
    bool SetConfig(AutostartMonitorConfig config);
    bool AddBaseDirectory(std::string path);
    bool AddDependencyDirSuffix(std::string suffix);

    const AutostartMonitorConfig& GetConfig() const;

    int Init();
    void Shutdown();

    void PollOnce(std::vector<AutostartRawChange>& changes, int timeout_ms);

private:
    struct WatchedRoot {
        std::string path;
        int fd = -1;
    };

    int MarkBaseDirectory(const std::string& path);
    int MarkDependencyDirectory(const std::string& path);
    int RefreshWatches();

    bool IsDependencyDirectory(const std::string& path) const;
    bool IsDirectChildOfBaseRoot(const std::string& path) const;
    bool IsUnderWatchedDependencyRoot(const std::string& path) const;

    std::optional<std::string> ResolvePathFromDfidName(
        const fanotify_event_info_fid* fid
    ) const;

private:
    int fan_fd_{-1};

    AutostartMonitorConfig config_;

    /*
     * base_roots_ — это директории типа:
     * /etc/systemd/system
     *
     * watched_roots_ — это найденные dependency-директории:
     * /etc/systemd/system/multi-user.target.wants
     * /etc/systemd/system/foo.service.requires
     */
    std::vector<WatchedRoot> base_roots_;
    std::vector<WatchedRoot> watched_roots_;
};

}  // namespace monitoring

#endif  // AUTOSTART_MONITOR_HPP