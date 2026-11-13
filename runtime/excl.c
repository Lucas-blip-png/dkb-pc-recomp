// The exclusive monitor's shared state: the reservation-granule versions (cpu_excl_ver) and the out-of-line store
// exclusives (excl_store8 ... excl_store128) that runtime/cpu.h declares for the lifted code. They are defined here
// once, outside the lifted modules, because asynchronous preemption must never stop a thread inside them (cpu.h,
// "exclusive monitor"; kernel.c checks at start that they lie outside the lifted code range).
#define CPU_EXCL_DEFINE
#include "cpu.h"
