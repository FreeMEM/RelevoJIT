/* SPDX-License-Identifier: MPL-2.0
 *
 * <math.h> for the freestanding build. Only floor(), which the packed
 * decimal conversion uses (fpx_dec.c) on small values; the transcendental
 * functions are left out (fpx_trans.c, RELEVO_NO_LIBM).
 */
#ifndef RELEVO_PORT_MATH_H
#define RELEVO_PORT_MATH_H

static inline double relevo_port_floor(double x)
{
    /* Beyond 2^52 every double is an integer (and NaN stays NaN). */
    if (!(x > -4503599627370496.0 && x < 4503599627370496.0))
        return x;
    double t = (double)(long long)x;    /* rounds toward zero */
    return t > x ? t - 1.0 : t;
}

#define floor(x) relevo_port_floor(x)

#endif
