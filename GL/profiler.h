#pragma once

#include <stdint.h>

#define PROFILER_COMPILE 0
#ifdef PROFILER_COMPILE
#define PROFILER_PUSH(S) profiler_push(S)
#define PROFILER_CHECKPOINT(P) profiler_checkpoint(P)
#define PROFILER_POP() profiler_pop()
void profiler_enable();
void profiler_disable();

typedef struct {
    char name[64];
    uint64_t start_time_in_us;
} Profiler;


Profiler* profiler_push(const char* name);
void profiler_checkpoint(const char* name);
void profiler_pop();

void profiler_print_stats();
#else 
#define PROFILER_PUSH(S) do {} while (0)
#define PROFILER_CHECKPOINT(P) do {} while (0)
#define PROFILER_POP() do {} while (0)
#endif 
