#include "monitoring/event_queue/event_queue.hpp"

#include <stdexcept>
#include <utility>

namespace monitoring {

EventQueue::EventQueue(std::size_t capacity) : capacity_(capacity) {
    if (capacity_ == 0) {
        throw std::runtime_error("EventQueue capacity must be > 0");
    }
}

bool EventQueue::Push(std::shared_ptr<MonitorEvent> event) {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (shutdown_ || queue_.size() >= capacity_) {
            return false;
        }
        queue_.push(std::move(event));
    }
    cv_.notify_one();
    return true;
}

std::shared_ptr<MonitorEvent> EventQueue::Pop() {
    std::unique_lock<std::mutex> lk(mutex_);
    cv_.wait(lk, [this] {
        return !queue_.empty() || shutdown_;
    });

    if (queue_.empty()) {
        return nullptr;
    }

    auto event = std::move(queue_.front());
    queue_.pop();
    return event;
}

std::shared_ptr<MonitorEvent> EventQueue::TryPop() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (queue_.empty()) {
        return nullptr;
    }

    auto event = std::move(queue_.front());
    queue_.pop();
    return event;
}

void EventQueue::Shutdown() {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        shutdown_ = true;
    }
    cv_.notify_all();
}

}  // namespace monitoring
