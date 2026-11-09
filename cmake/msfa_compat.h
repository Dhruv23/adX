// Force-included before upstream msfa's sources when they are compiled as the dx7
// test oracle (cmake/AdxDependencies.cmake). synth.h otherwise reaches `#warning
// Memory barrier is not enabled` on MSVC; the oracle is single-threaded, so the
// barrier is nothing.
#pragma once
#define SynthMemoryBarrier()
