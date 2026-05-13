#pragma once

#include <memory>

#include "monitoring/event_queue/i_event_queue.hpp"
#include "monitoring/sources/threaded_source.hpp"
#include "monitors/autostart_monitor/autostart_monitor.hpp"

namespace monitoring {

    /**
     * @brief Источник событий автозапуска systemd.
     *
     * AutostartSource запускает AutostartMonitor в отдельном потоке,
     * получает от него низкоуровневые изменения в директориях автозапуска
     * и преобразует их в события мониторинга AutostartEvent.
     *
     * По умолчанию монитор отслеживает создание и удаление записей
     * внутри директорий systemd-зависимостей, например:
     * - /etc/systemd/system/*.wants/
     * - /etc/systemd/system/*.requires/
     *
     * Конкретные базовые директории и суффиксы dependency-директорий
     * могут быть переопределены через AutostartMonitorConfig.
     */
    class AutostartSource : public ThreadedSource {
    public:
        /**
         * @brief Создаёт источник событий автозапуска с конфигурацией по умолчанию.
         *
         * @param queue Очередь событий мониторинга, в которую будут помещаться
         *              сформированные AutostartEvent.
         */
        AutostartSource(std::shared_ptr<IEventQueue> queue);
        /**
         * @brief Создаёт источник событий автозапуска с пользовательской конфигурацией.
         *
         * @param queue Очередь событий мониторинга, в которую будут помещаться
         *              сформированные AutostartEvent.
         * @param config Конфигурация монитора автозапуска: базовые директории
         *               и суффиксы dependency-директорий для отслеживания.
         */
        AutostartSource(std::shared_ptr<IEventQueue> queue, AutostartMonitorConfig config);

        /**
         * @brief Устанавливает конфигурацию внутреннего монитора автозапуска.
         *
         * Метод предназначен для настройки списка базовых директорий и суффиксов
         * dependency-директорий, которые должен отслеживать AutostartMonitor.
         *
         * Конфигурацию следует задавать до запуска источника, то есть до вызова
         * Start(). После запуска источник уже работает в отдельном потоке, а монитор
         * держит открытые fanotify-дескрипторы и установленные marks.
         *
         * @param config Новая конфигурация монитора автозапуска.
         */
        void SetMonitorConfig(const AutostartMonitorConfig& config);
        /**
         * @brief Возвращает текущую конфигурацию внутреннего монитора автозапуска.
         *
         * @return Копия текущей конфигурации AutostartMonitorConfig.
         */
        AutostartMonitorConfig GetMonitorConfig() const;

        ~AutostartSource() override;

    protected:
        /**
         * @brief Основной цикл работы источника событий автозапуска.
         *
         * Метод выполняется в рабочем потоке ThreadedSource.
         * Он инициализирует AutostartMonitor, периодически опрашивает fanotify,
         * преобразует полученные AutostartRawChange в AutostartEvent
         * и помещает их в очередь событий.
         */
        void Run() override;

    private:
        std::unique_ptr<AutostartMonitor> monitor_{};
    };

}  // namespace monitoring