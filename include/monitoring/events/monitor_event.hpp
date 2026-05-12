#pragma once

#include <chrono>
#include <string>

namespace monitoring {

    enum class SourceType {
        Network,
        Fs,
        Process,
        Auth,
        Users,
        SysSwSettings,
        Scheduler,
        Autostart
    };

    enum class EventType : int {
        Unknown = 0,

        // Network
        NetworkConnectionOpened = 100,
        NetworkConnectionClosed,

        // Fs
        FsFileCreation = 200,
        FsDirCreation,
        FsFileRead,
        FsDirRead,
        FsFileChange,
        FsFileRemove,
        FsDirRemove,
        FsFileRename,
        FsDirRename,
        FsFileMove,
        FsDirMove,

        // Process
        ProcessStarted = 300,
        ProcessExited,

        // Auth
        AuthLogin = 400,

        // Users
        UsersAccountCreation = 500,
        UsersGroupCreation,
        UsersAccountChange,
        UsersGroupChange,
        UsersAccountRemove,
        UsersGroupRemove,
        UsersPasswordChange,
        UsersAccountBlock,
        UsersAccountUnblock,

        // SwSettings
        SysSwSettingsChange = 600,

        // Scheduler
        SchedulerTaskCreation = 650,
        SchedulerTaskChange,
        SchedulerTaskRemove,

        // Autostart
        AutostartCreateTask = 700,
        AutostartRemoveTask
    };

    struct MonitorEvent {
        pid_t pid = 0;
        pid_t ppid = 0;
        std::chrono::system_clock::time_point timestamp{};
        SourceType src_type = SourceType::Process;
        EventType type = EventType::Unknown;
        int result = 0;

        virtual ~MonitorEvent() = default;
    };

    struct AutostartEvent : MonitorEvent {
        std::string path;

        AutostartEvent() {
            src_type = SourceType::Autostart;
        }
    };

    inline std::string GetEventName(EventType type) {
        switch (type) {
            case EventType::Unknown:                  return "Unknown";

                // События сети
            case EventType::NetworkConnectionOpened:  return "NetworkConnectionOpened";
            case EventType::NetworkConnectionClosed:  return "NetworkConnectionClosed";

                // События файловой системы
            case EventType::FsFileCreation:           return "FsFileCreation";
            case EventType::FsDirCreation:            return "FsDirCreation";
            case EventType::FsFileRead:               return "FsFileRead";
            case EventType::FsDirRead:                return "FsDirRead";
            case EventType::FsFileChange:             return "FsFileChange";
            case EventType::FsFileRemove:             return "FsFileRemove";
            case EventType::FsDirRemove:              return "FsDirRemove";
            case EventType::FsFileRename:             return "FsFileRename";
            case EventType::FsDirRename:              return "FsDirRename";
            case EventType::FsFileMove:               return "FsFileMove";
            case EventType::FsDirMove:                return "FsDirMove";

                // События аутентификации
            case EventType::ProcessStarted:           return "ProcessStarted";
            case EventType::ProcessExited:            return "ProcessExited";
            case EventType::AuthLogin:                return "AuthLogin";

                // Cобытия пользователей/групп
            case EventType::UsersAccountCreation:    return "UsersAccountCreation";
            case EventType::UsersGroupCreation:      return "UsersGroupCreation";
            case EventType::UsersAccountChange:      return "UsersAccountChange";
            case EventType::UsersGroupChange:        return "UsersGroupChange";
            case EventType::UsersAccountRemove:      return "UsersAccountRemove";
            case EventType::UsersGroupRemove:        return "UsersGroupRemove";
            case EventType::UsersPasswordChange:     return "UsersPasswordChange";
            case EventType::UsersAccountBlock:       return "UsersAccountBlock";
            case EventType::UsersAccountUnblock:     return "UsersAccountUnblock";

                // События изменения настроек системного ПО
            case EventType::SysSwSettingsChange:         return "SysSwSettingsChange";

                // События изменения задач планировщика
            case EventType::SchedulerTaskCreation:      return "SchedulerTaskCreation";
            case EventType::SchedulerTaskChange:        return "SchedulerTaskChange";
            case EventType::SchedulerTaskRemove:        return "SchedulerTaskRemove";

                // События изменения задач автозапуска
            case EventType::AutostartCreateTask:        return "AutostartCreateTask";
            case EventType::AutostartRemoveTask:        return "AutostartRemoveTask";

        }
        return "?";
    }

}  // namespace monitoring
