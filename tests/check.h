#pragma once
#include <cstdio>

// Shared by the AkiLauncherTests files: a failed CHECK prints and counts.
extern int g_failed;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            ++g_failed;                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                 \
    } while (0)
