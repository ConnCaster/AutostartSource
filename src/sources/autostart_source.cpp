#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "monitoring/events/monitor_event.hpp"
#include "monitoring/sources/autostart_source.hpp"
#include "monitoring/sources/monitors/autostart_monitor/autostart_monitor.hpp"

namespace monitoring {

    constexpr int kPollingTimeoutMs = 250;

    AutostartSource::AutostartSource(std::shared_ptr<IEventQueue> queue)
        : ThreadedSource(std::move(queue)),
          monitor_(std::make_unique<AutostartMonitor>()) {
        std::cout << "AutostartSource: constructed\n";
    }

    AutostartSource::~AutostartSource() {
        Stop();
    }

    void AutostartSource::Run() {
        if (monitor_->Init() != 0) {
            std::cerr << "AutostartSource: failed to init monitor\n";
            return;
        }

        while (is_running_.load()) {
            std::vector<AutostartRawChange> raw_changes;
            monitor_->PollOnce(raw_changes, kPollingTimeoutMs);

            for (auto& change : raw_changes) {
                auto event = std::make_shared<AutostartEvent>();

                event->timestamp = std::chrono::system_clock::now();
                event->pid = change.pid;
                event->ppid = change.ppid;
                event->result = 0;
                event->type = change.type;
                event->path = std::move(change.path);

                queue_->Push(std::move(event));
            }
        }

        monitor_->Shutdown();
    }

}  // namespace monitoring