#pragma once

#include <functional>

// Calls fn(i) for every i in [0, count) on a process-wide pool of worker threads plus the calling
// thread, and returns once all items are done. Items are handed out one at a time, so fn must write
// only the outputs of its own item. On the pool, the first exception an item throws is rethrown here
// after the other items finish. A nested call, a single item or a pool without workers runs inline,
// where an exception propagates at once and the remaining items are not run.
void work_pool_run(int count, const std::function<void(int)>& fn);
