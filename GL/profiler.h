#pragma once

#include <stdint.h>

#define PROFILER_COMPILE 1
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
#define PROFILER_PUSH(S) 
#define PROFILER_CHECKPOINT(P) 
#define PROFILER_POP() 
#endif 
