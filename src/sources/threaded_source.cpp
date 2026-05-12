#include "monitoring/sources/threaded_source.hpp"

#include <cerrno>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace monitoring {

ThreadedSource::ThreadedSource(std::shared_ptr<IEventQueue> queue)
    : queue_(std::move(queue)) {
    if (!queue_) {
        throw std::invalid_argument("ThreadedSource queue must not be null");
    }
}

ThreadedSource::~ThreadedSource() {
    Stop();
}

int ThreadedSource::Start() {
    bool expected = false;
    if (!is_running_.compare_exchange_strong(expected, true)) {
        return EALREADY;
    }

    try {
        loop_ = std::thread([this] {
            try {
                Run();
            } catch (const std::exception& e) {
                std::cerr << "ThreadedSource::Run() failed: " << e.what() << '\n';
            } catch (...) {
                std::cerr << "ThreadedSource::Run() failed: unknown exception\n";
            }
            is_running_.store(false);
        });
    } catch (const std::system_error& e) {
        is_running_.store(false);
        return e.code().value() != 0 ? e.code().value() : EAGAIN;
    }

    return 0;
}

void ThreadedSource::Stop() {
    is_running_.store(false);

    if (!loop_.joinable()) {
        return;
    }

    if (loop_.get_id() == std::this_thread::get_id()) {
        loop_.detach();
        return;
    }

    loop_.join();
}

}  // namespace monitoring
