
#ifndef NDEBUG
/* We're debugging, use normal assert */
#include <assert.h>
#define gl_assert assert
#else
/* Release mode, use our custom assert */
#include <stdio.h>
#include <stdlib.h>

#define gl_assert(x) \
    do {\
        if(!(x)) {\
            fprintf(stderr, "Assertion failed at %s:%d\n", __FILE__, __LINE__);\
            /* Force an address-error exception instead of exit(1): exit drops \
             * silently back to the BIOS, while the fault lands on the host \
             * app's crash handler (HyperSolar's guru screen) with THIS PC, \
             * which addr2line resolves straight to this guard. */\
            *(volatile int *)1 = 0;\
            exit(1);\
        }\
    } while(0); \

#endif

