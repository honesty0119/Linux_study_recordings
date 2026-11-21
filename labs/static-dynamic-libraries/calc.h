#ifndef CALC_H
#define CALC_H

/* Integer arithmetic: add/sub/mul require a result representable by int. */
int calc_add(int a, int b);
int calc_sub(int a, int b);
int calc_mul(int a, int b);

/* Division truncates toward zero. On error, *result is left unchanged. */
enum calc_status {
    CALC_OK = 0,
    CALC_DIV_ZERO = 1,
    CALC_OVERFLOW = 2,
    CALC_INVALID_ARGUMENT = 3
};
int calc_div(int a, int b, int *result);

#endif
