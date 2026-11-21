#include <limits.h>
#include <stddef.h>
#include "calc.h"

int calc_div(int a, int b, int *result)
{
    if (result == NULL) {
        return CALC_INVALID_ARGUMENT;
    }
    if (b == 0) {
        return CALC_DIV_ZERO;
    }
    if (a == INT_MIN && b == -1) {
        return CALC_OVERFLOW;
    }
    *result = a / b;
    return CALC_OK;
}
