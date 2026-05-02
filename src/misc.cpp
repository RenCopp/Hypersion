#include "misc.h"

namespace hypersion {

std::ostream& operator<<(std::ostream& os, SyncCout::Sync s) {
    static std::mutex m;
    if (s == SyncCout::IO_LOCK)   m.lock();
    if (s == SyncCout::IO_UNLOCK) m.unlock();
    return os;
}

std::string engine_id() {
    std::ostringstream ss;
    ss << ENGINE_NAME << ' ' << ENGINE_VERSION;
    return ss.str();
}

}  // namespace hypersion
