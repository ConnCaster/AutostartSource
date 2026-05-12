#pragma once

#include <memory>

#include "monitoring/event_queue/i_event_queue.hpp"
#include "monitoring/sources/threaded_source.hpp"
#include "monitors/autostart_monitor/autostart_monitor.hpp"

namespace monitoring {

    class AutostartSource : public ThreadedSource {
    public:
        explicit AutostartSource(std::shared_ptr<IEventQueue> queue);
        AutostartSource(std::shared_ptr<IEventQueue> queue, AutostartMonitorConfig config);

        ~AutostartSource() override;

    protected:
        void Run() override;

    private:
        std::unique_ptr<AutostartMonitor> monitor_{};
    };

}  // namespace monitoring