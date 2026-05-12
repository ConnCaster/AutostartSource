#pragma once

#include <atomic>
#include <memory>
#include <thread>

#include "monitoring/event_queue/i_event_queue.hpp"
#include "monitoring/sources/i_event_source.hpp"

namespace monitoring {

class ThreadedSource : public IEventSource {
public:
    explicit ThreadedSource(std::shared_ptr<IEventQueue> queue);
    ~ThreadedSource() override;

    ThreadedSource(const ThreadedSource&) = delete;
    ThreadedSource& operator=(const ThreadedSource&) = delete;

    int Start() final;
    void Stop() final;

protected:
    virtual void Run() = 0;

    std::shared_ptr<IEventQueue> queue_;
    std::atomic<bool> is_running_{false};

private:
    std::thread loop_;
};

}  // namespace monitoring
