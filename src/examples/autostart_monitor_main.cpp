#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <thread>

#include "monitoring/event_queue/event_queue.hpp"
#include "monitoring/events/monitor_event.hpp"
#include "monitoring/sources/autostart_source.hpp"

volatile std::sig_atomic_t g_stop = 0;

void OnSignal(int) {
    g_stop = 1;
}

int main() {
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    auto queue = std::make_shared<monitoring::EventQueue>(4096);
    monitoring::AutostartSource source(queue);

    const int rc = source.Start();
    if (rc != 0) {
        std::cerr << "source.Start() failed: " << rc << '\n';
        return 1;
    }

    std::cerr << "main: entering event loop\n";

    while (!g_stop) {
        auto event = queue->TryPop();

        if (event) {
            if (event->src_type != monitoring::SourceType::Autostart) {
                continue;
            }

            auto autostart_event =
                std::static_pointer_cast<monitoring::AutostartEvent>(event);

            std::cout << "event=" << monitoring::GetEventName(autostart_event->type)
                      << " path=" << autostart_event->path
                      << " pid=" << autostart_event->pid
                      << " ppid=" << autostart_event->ppid
                      << " path=" << autostart_event->path
                      << std::endl;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    std::cerr << "main: stopping source and queue\n";

    source.Stop();
    queue->Shutdown();

    std::cerr << "main: exiting\n";
    return 0;
}