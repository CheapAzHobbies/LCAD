// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/kernel/Stop.h>

#include <atomic>

namespace lcad::kernel {

namespace {

std::atomic<bool>& request() {
    static std::atomic<bool> value{false};
    return value;
}

} // namespace

void requestStop() {
    request().store(true);
}

void clearStop() {
    request().store(false);
}

bool stopRequested() {
    return request().load();
}

} // namespace lcad::kernel
