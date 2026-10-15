/* Integer division helpers required by the ARM EABI: Cortex-M0 has no divide
 * instruction, so the compiler calls these for '/' and '%'.
 * The *divmod variants return quotient in r0 and remainder in r1, which is how
 * AAPCS returns a 64-bit value (low word r0, high word r1).
 */
#include <stdint.h>

static uint32_t udivmod(uint32_t n, uint32_t d, uint32_t *rem)
{
    uint32_t q = 0, r = 0;
    if (!d) { *rem = n; return 0xffffffffu; }
    if (n < d) { *rem = n; return 0; }
    int top = 31;                       /* no CLZ instruction on Cortex-M0 */
    while (!(n >> top)) --top;
    for (int i = top; i >= 0; --i) {
        r = (r << 1) | ((n >> i) & 1u);
        if (r >= d) { r -= d; q |= 1u << i; }
    }
    *rem = r;
    return q;
}

uint32_t __aeabi_uidiv(uint32_t n, uint32_t d)
{
    uint32_t r;
    return udivmod(n, d, &r);
}

uint64_t __aeabi_uidivmod(uint32_t n, uint32_t d)
{
    uint32_t r, q = udivmod(n, d, &r);
    return ((uint64_t)r << 32) | q;
}

int32_t __aeabi_idiv(int32_t n, int32_t d)
{
    uint32_t r, q = udivmod(n < 0 ? -(uint32_t)n : (uint32_t)n, d < 0 ? -(uint32_t)d : (uint32_t)d, &r);
    return (n < 0) != (d < 0) ? -(int32_t)q : (int32_t)q;
}

uint64_t __aeabi_idivmod(int32_t n, int32_t d)
{
    uint32_t r, q = udivmod(n < 0 ? -(uint32_t)n : (uint32_t)n, d < 0 ? -(uint32_t)d : (uint32_t)d, &r);
    int32_t sq = (n < 0) != (d < 0) ? -(int32_t)q : (int32_t)q;
    int32_t sr = n < 0 ? -(int32_t)r : (int32_t)r;      /* remainder takes the dividend's sign */
    return ((uint64_t)(uint32_t)sr << 32) | (uint32_t)sq;
}
