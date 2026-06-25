# Real-Time Audio DSP Constraints

You are writing code for a real-time, low-latency audio processing loop (`AudioEngine::process`). Because real-time audio threads are strictly time-bounded, standard programming patterns will cause catastrophic audio dropouts (stalls).

## 1. The Sacred Audio Loop Rules
When editing or adding code that executes within `AudioEngine::process`, you are strictly prohibited from writing:
* **NO Allocations:** Do not use `new`, `delete`, `malloc`, `free`, or initialize objects that dynamically allocate on the heap.
* **NO Resizing Container Operations:** Do not call operations that can trigger hidden heap reallocations (e.g., `std::vector::push_back`, `std::vector::insert`, `std::unordered_map` insertions). All buffers must be pre-allocated or fixed-size.
* **NO Blocking Primitives:** Do not introduce `std::mutex`, `std::lock_guard`, `std::unique_lock`, or condition variables. Inter-thread communication must exclusively use the lock-free `moodycamel::ReaderWriterQueue`.
* **NO System I/O:** Do not call disk readers/writers, console printing (`std::cout`, `printf`), or logging utilities within the loop.

## 2. Memory & Math Defenses
* **Bounds Checking:** When pulling sample indices from multi-channel audio vectors (like reading from `AudioClip::pcmData`), you must write explicit boundary guard clamps or size checks to prevent memory segmentation faults.
* **Fallback Assertions:** If an audio clip index calculations drift out-of-bounds, gracefully drop the output sample value to `0.0f` rather than letting the application crash.