#pragma once

namespace monitoring {

class IEventSource {
public:
    virtual ~IEventSource() = default;

    virtual int Start() = 0;
    virtual void Stop() = 0;
};

}  // namespace monitoring
