#include "SpinLock.h"

namespace StreamLine::Locks {
    thread_local MCSLock::Node MCSLock::localNode_;
}
