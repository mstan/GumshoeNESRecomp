/*
 * recomp_stack.c — Recompiled function call stack tracking
 */
#include "recomp_stack.h"
#include <stdio.h>
#include <stdint.h>

const char *g_recomp_stack[RECOMP_STACK_DEPTH];
int         g_recomp_stack_top = 0;
const char *g_last_recomp_func = "(none)";

void recomp_stack_push(const char *name)
{
    if (g_recomp_stack_top < RECOMP_STACK_DEPTH)
        g_recomp_stack[g_recomp_stack_top++] = name;
    g_last_recomp_func = name;

    /* Recursion detector: if stack > 50, dump and abort */
    if (g_recomp_stack_top == 50) {
        extern uint64_t g_frame_count;
        fprintf(stderr, "\n[RECURSION] Stack depth hit 50 at frame %llu!\n",
                (unsigned long long)g_frame_count);
        for (int i = g_recomp_stack_top - 1; i >= 0; i--)
            fprintf(stderr, "  [%d] %s\n", i, g_recomp_stack[i] ? g_recomp_stack[i] : "?");
        fflush(stderr);
    }
}

void recomp_stack_pop(void)
{
    if (g_recomp_stack_top > 0)
        g_recomp_stack_top--;
    g_last_recomp_func = (g_recomp_stack_top > 0)
                        ? g_recomp_stack[g_recomp_stack_top - 1]
                        : "(none)";
}
