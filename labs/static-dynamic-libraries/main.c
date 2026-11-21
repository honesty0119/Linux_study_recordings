#include <limits.h>
#include <stdio.h>
#include "calc.h"

static int failures = 0;
static int checks = 0;

static void check(const char *label, int actual, int expected)
{
    ++checks;
    printf("[%s] %s: got %d, expected %d\n",
           actual == expected ? "PASS" : "FAIL", label, actual, expected);
    if (actual != expected) {
        ++failures;
    }
}

int main(void)

{
    int result = 0;

    check("add(12, 3)", calc_add(12, 3), 15);
    check("sub(12, 3)", calc_sub(12, 3), 9);
    check("mul(12, 3)", calc_mul(12, 3), 36);
    check("div(12, 3) status", calc_div(12, 3, &result), CALC_OK);
    check("div(12, 3) result", result, 4);
    check("sub(3, 12)", calc_sub(3, 12), -9);
    check("mul(-4, 3)", calc_mul(-4, 3), -12);
    check("mul(12, 0)", calc_mul(12, 0), 0);
    check("div(7, 2) status", calc_div(7, 2, &result), CALC_OK);
    check("div(7, 2) result", result, 3);
    check("div(-7, 2) status", calc_div(-7, 2, &result), CALC_OK);
    check("div(-7, 2) result", result, -3);

    result = 123;
    check("divide by zero", calc_div(7, 0, &result), CALC_DIV_ZERO);
    check("zero divisor keeps result", result, 123);
    check("division overflow", calc_div(INT_MIN, -1, &result), CALC_OVERFLOW);
    check("overflow keeps result", result, 123);
    check("null output pointer", calc_div(7, 2, NULL), CALC_INVALID_ARGUMENT);

    printf("Result: %d/%d checks passed.\n", checks - failures, checks);
    return failures == 0 ? 0 : 1;
}
