#pragma once

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <queue>

#include "monitoring/event_queue/i_event_queue.hpp"

namespace monitoring {

class EventQueue : public IEventQueue {
public:
    explicit EventQueue(std::size_t capacity);

    bool Push(std::shared_ptr<MonitorEvent> event) override;
    std::shared_ptr<MonitorEvent> Pop() override;
    std::shared_ptr<MonitorEvent> TryPop() override;
    void Shutdown() override;

private:
    std::queue<std::shared_ptr<MonitorEvent>> queue_;
    const std::size_t capacity_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool shutdown_{false};
};

}  // namespace monitoring
