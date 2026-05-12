#pragma once

#include <memory>

#include "monitoring/events/monitor_event.hpp"

namespace monitoring {

class IEventQueue {
public:
    virtual ~IEventQueue() = default;

    virtual bool Push(std::shared_ptr<MonitorEvent> event) = 0;
    virtual std::shared_ptr<MonitorEvent> Pop() = 0;
    virtual std::shared_ptr<MonitorEvent> TryPop() = 0;
    virtual void Shutdown() = 0;
};

}  // namespace monitoring
