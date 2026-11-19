#ifndef WR_TEST_CHECK_H
#define WR_TEST_CHECK_H

#include <stdio.h>
#include <stdlib.h>

#define CHECK(expression) do {                                              \
    if (!(expression)) {                                                    \
        fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__,            \
                #expression);                                               \
        exit(EXIT_FAILURE);                                                 \
    }                                                                       \
} while (0)

#endif
