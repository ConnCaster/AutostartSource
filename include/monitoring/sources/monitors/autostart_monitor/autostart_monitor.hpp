#ifndef AUTOSTART_MONITOR_HPP
#define AUTOSTART_MONITOR_HPP

#include <optional>
#include <string>
#include <sys/fanotify.h>
#include <sys/types.h>
#include <vector>

#include "monitoring/events/monitor_event.hpp"

namespace monitoring {

    struct AutostartRawChange {
        std::string path;
        pid_t pid = 0;
        pid_t ppid = 0;
        EventType type = EventType::Unknown;
    };

    class AutostartMonitor {
    public:
        AutostartMonitor() = default;
        ~AutostartMonitor();

        int Init();
        void Shutdown();

        void PollOnce(std::vector<AutostartRawChange>& changes, int timeout_ms);

    private:
        struct WatchedRoot {
            std::string path;
            int fd = -1;
        };

        int RefreshWatches();
        int MarkDependencyDirectory(const std::string& path);
        bool IsUnderWatchedDependencyRoot(const std::string& path) const;

        std::optional<std::string> ResolvePathFromDfidName(
            const fanotify_event_info_fid* fid
        ) const;

    private:
        int fan_fd_{-1};
        int systemd_root_fd_{-1};
        std::vector<WatchedRoot> watched_roots_;
    };

}  // namespace monitoring

#endif  // AUTOSTART_MONITOR_HPP