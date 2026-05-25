#ifndef CABE_OPTIONS_H
#define CABE_OPTIONS_H

#include <string>
#include <vector>

namespace cabe {

    struct DeviceConfig {
        std::string path;
    };

    struct Options {
        std::vector<DeviceConfig> devices;
    };

} // namespace cabe

#endif // CABE_OPTIONS_H
