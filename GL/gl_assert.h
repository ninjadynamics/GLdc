
#ifndef GL_ASSERT_H
#define GL_ASSERT_H

#ifndef NDEBUG
/* We're debugging, use normal assert */
#include <assert.h>
#define gl_assert assert
#else
/* Release mode, use our custom assert */
#include <stdio.h>
#include <stdlib.h>

/* The fault address lives behind a volatile pointer so the optimizer cannot
 * constant-propagate the dereference of address 1 and flag it (-Warray-bounds
 * fired on every gl_assert expansion at -O2+). The store below is still a
 * misaligned write at runtime; only the fatal path pays the extra load. */
static volatile int *volatile gl_assert_fault_addr = (volatile int *)1;

#define gl_assert(x) \
    do {\
        if(!(x)) {\
            fprintf(stderr, "Assertion failed at %s:%d\n", __FILE__, __LINE__);\
            /* Force an address-error exception instead of exit(1): exit drops \
             * silently back to the BIOS, while the fault lands on the host \
             * app's crash handler (HyperSolar's guru screen) with THIS PC, \
             * which addr2line resolves straight to this guard. */\
            *gl_assert_fault_addr = 0;\
            exit(1);\
        }\
    } while(0); \

#endif

#endif /* GL_ASSERT_H */

