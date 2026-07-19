/* Force-included (/FI) into shine's C sources when building with MSVC.
 * shine annotates locals with GCC's __attribute__((unused)); MSVC has no such
 * keyword, so expand the whole annotation to nothing. */
#pragma once
#define __attribute__(x)
