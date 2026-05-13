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

/**
 * @brief Конфигурация монитора автозапуска systemd.
 *
 * Конфигурация определяет:
 * - базовые директории, внутри которых нужно искать dependency-директории;
 * - суффиксы dependency-директорий, которые должны отслеживаться.
 *
 * По умолчанию монитор отслеживает директории:
 * - /etc/systemd/system/*.wants
 * - /etc/systemd/system/*.requires
 */
struct AutostartMonitorConfig {
    /**
     * @brief Базовые директории для поиска dependency-директорий.
     *
     * Например:
     * - /etc/systemd/system
     *
     * Внутри этих директорий монитор ищет прямых потомков, имена которых
     * заканчиваются на один из суффиксов из dependency_dir_suffixes.
     */
    std::vector<std::string> base_dirs{
        kDefaultAutostartRoot
    };

    /**
     * @brief Суффиксы dependency-директорий, которые нужно отслеживать.
     *
     * Например:
     * - .wants
     * - .requires
     *
     * Если директория внутри base_dirs заканчивается на один из этих суффиксов,
     * монитор добавляет её в список отслеживаемых директорий.
     */
    std::vector<std::string> dependency_dir_suffixes{
        kDefaultWantsSuffix,
        kDefaultRequiresSuffix
    };
};

/**
 * @brief Низкоуровневое событие изменения автозапуска.
 *
 * Структура используется AutostartMonitor для передачи информации в
 * AutostartSource. Затем AutostartSource преобразует её в полноценный
 * AutostartEvent и помещает в общую очередь событий мониторинга.
 */
struct AutostartRawEvent {
    std::string path;
    pid_t pid = 0;
    pid_t ppid = 0;
    EventType type = EventType::Unknown;
};

/**
 * @brief Монитор изменений записей автозапуска systemd.
 *
 * AutostartMonitor отслеживает создание и удаление файлов или символических
 * ссылок внутри dependency-директорий systemd:
 * - /etc/systemd/system/*.wants/
 * - /etc/systemd/system/*.requires/
 *
 * Также монитор отслеживает появление новых dependency-директорий в базовых
 * директориях и автоматически добавляет их в список наблюдения.
 */
class AutostartMonitor {
public:
    /**
     * @brief Создаёт монитор автозапуска с конфигурацией по умолчанию.
     */
    AutostartMonitor();
    /**
     * @brief Создаёт монитор автозапуска с пользовательской конфигурацией.
     *
     * @param config Конфигурация базовых директорий и суффиксов
     *               dependency-директорий.
     */
    AutostartMonitor(AutostartMonitorConfig config);
    ~AutostartMonitor();
    void Shutdown();

    /**
     * @brief Устанавливает новую конфигурацию монитора.
     *
     * Метод задаёт список базовых директорий и суффиксов dependency-директорий.
     * Его следует вызывать до Init(), пока монитор ещё не открыл fanotify fd
     * и не установил marks.
     *
     * @param config Новая конфигурация монитора.
     *
     * @return true, если конфигурация успешно установлена.
     * @return false, если конфигурация некорректна или монитор уже запущен.
     */
    bool SetConfig(AutostartMonitorConfig config);
    /**
     * @brief Добавляет базовую директорию для поиска dependency-директорий.
     *
     * Например, можно добавить:
     * /usr/lib/systemd/system
     * или
     * /lib/systemd/system
     *
     * Метод следует вызывать до Init().
     *
     * @param path Путь к базовой директории.
     *
     * @return true, если директория добавлена или уже присутствует в конфигурации.
     * @return false, если путь некорректен или монитор уже запущен.
     */
    bool AddBaseDirectory(std::string path);
    /**
     * @brief Добавляет суффикс dependency-директорий.
     *
     * Например:
     * - .wants
     * - .requires
     * - .custom-wants
     *
     * После добавления суффикса монитор будет искать в базовых директориях
     * прямых потомков, имена которых заканчиваются на этот суффикс.
     *
     * Метод следует вызывать до Init().
     *
     * @param suffix Суффикс имени директории.
     *
     * @return true, если суффикс добавлен или уже присутствует в конфигурации.
     * @return false, если суффикс некорректен или монитор уже запущен.
     */
    bool AddDependencyDirSuffix(std::string suffix);

    AutostartMonitorConfig GetConfig() const;

    /**
     * @brief Инициализирует fanotify и устанавливает marks на директории.
     *
     * Метод открывает fanotify-дескриптор, отмечает базовые директории для
     * отслеживания появления новых dependency-директорий, затем находит уже
     * существующие dependency-директории и также устанавливает на них marks.
     *
     * @return 0 при успешной инициализации.
     * @return Код errno при ошибке.
     */
    int Init();
    /**
     * @brief Выполняет один цикл ожидания и чтения событий fanotify.
     *
     * Метод ожидает события не дольше timeout_ms миллисекунд. Найденные события
     * создания или удаления записей автозапуска помещаются в changes.
     *
     * @param changes Вектор, в который будут добавлены найденные события.
     * @param timeout_ms Таймаут ожидания события в миллисекундах.
     */
    void PollOnce(std::vector<AutostartRawEvent>& changes, int timeout_ms);

private:
    /**
    * @brief Описание директории, на которую установлен fanotify mark.
    */
    struct WatchedRoot {
        std::string path;
        int fd = -1;
    };

    /**
     * @brief Устанавливает fanotify mark на базовую директорию.
     *
     * Базовая директория используется для отслеживания появления новых
     * dependency-директорий с нужными суффиксами.
     *
     * @param path Путь к базовой директории.
     *
     * @return 0 при успехе.
     * @return Код errno при ошибке.
     */
    int MarkBaseDirectory(const std::string& path);
    /**
     * @brief Устанавливает fanotify mark на dependency-директорию.
     *
     * Dependency-директория используется для отслеживания создания и удаления
     * файлов или символических ссылок внутри неё.
     *
     * @param path Путь к dependency-директории.
     *
     * @return 0 при успехе.
     * @return Код errno при ошибке.
     */
    int MarkDependencyDirectory(const std::string& path);
    /**
     * @brief Обновляет список отслеживаемых dependency-директорий.
     *
     * Метод заново сканирует базовые директории, ищет в них директории с
     * подходящими суффиксами и устанавливает на них fanotify marks.
     *
     * Обычно вызывается при инициализации и при появлении новой директории
     * вида *.wants или *.requires.
     *
     * @return 0 при успехе.
     * @return Первый код errno, если хотя бы одну директорию не удалось отметить.
     */
    int RefreshWatches();

    /**
     * @brief Проверяет, является ли путь dependency-директорией.
     *
     * Проверка выполняется по суффиксам из конфигурации монитора.
     *
     * @param path Проверяемый путь.
     *
     * @return true, если путь заканчивается на один из настроенных суффиксов.
     * @return false в противном случае.
     */
    bool IsDependencyDirectory(const std::string& path) const;
    /**
     * @brief Проверяет, является ли путь прямым потомком одной из базовых директорий.
     *
     * Используется, чтобы отличать директории вида:
     * /etc/systemd/system/foo.wants
     *
     * от более глубоких вложенных путей.
     *
     * @param path Проверяемый путь.
     *
     * @return true, если path является прямым потомком одной из base_dirs.
     * @return false в противном случае.
     */
    bool IsDirectChildOfBaseRoot(const std::string& path) const;
    /**
     * @brief Проверяет, находится ли путь внутри отслеживаемой dependency-директории.
     *
     * Именно эта проверка определяет, будет ли файловое событие преобразовано
     * в событие мониторинга автозапуска.
     *
     * @param path Проверяемый путь.
     *
     * @return true, если путь находится внутри одной из watched_roots_.
     * @return false в противном случае.
     */
    bool IsUnderWatchedDependencyRoot(const std::string& path) const;

    /**
     * @brief Восстанавливает полный путь из fanotify DFID_NAME-записи.
     *
     * fanotify с FAN_REPORT_DIR_FID и FAN_REPORT_NAME передаёт файловый handle
     * родительской директории и имя изменённой записи. Метод открывает
     * родительскую директорию через open_by_handle_at(), получает её путь через
     * /proc/self/fd и объединяет путь директории с именем записи.
     *
     * @param fid Указатель на fanotify_event_info_fid с типом
     *            FAN_EVENT_INFO_TYPE_DFID_NAME.
     *
     * @return Полный путь к изменённой записи при успехе.
     * @return std::nullopt, если путь восстановить не удалось.
     */
    std::optional<std::string> ResolvePathFromDfidName(const fanotify_event_info_fid* fid) const;

private:
    int fan_fd_{-1};
    AutostartMonitorConfig config_;

    /**
     * @brief Базовые директории, на которые установлен fanotify mark.
     *
     * Примеры:
     * - /etc/systemd/system
     *
     * Эти директории нужны, чтобы замечать появление новых dependency-директорий.
     */
    std::vector<WatchedRoot> base_roots_;
    /**
     * @brief Отслеживаемые dependency-директории.
     *
     * Примеры:
     * - /etc/systemd/system/multi-user.target.wants
     * - /etc/systemd/system/foo.service.requires
     *
     * Внутри этих директорий монитор отслеживает создание и удаление записей
     * автозапуска.
     */
    std::vector<WatchedRoot> watched_roots_;
};

}  // namespace monitoring

#endif  // AUTOSTART_MONITOR_HPP