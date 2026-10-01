// Linux only: lets the program run on older systems than the one it was built on. A few maths
// and number-reading functions got new versions in recent glibc (2.38, 2.43) that the compiler
// picks by default; calls to them are sent (-Wl,--wrap, host/CMakeLists.txt) to the older
// versions every glibc since 2.2.5/2.7 has, which behave the same for this program. The rest
// of the program needs glibc 2.35 (Ubuntu 22.04, Fedora 36, Debian 12 and newer).

#include <stdarg.h>
#include <stdint.h>
#include <sys/random.h>

#define OLD(ret, name, args, version) \
    __asm__(".symver old_" #name "," #name "@" version); \
    ret old_##name args;

OLD(float, acosf, (float), "GLIBC_2.2.5")
OLD(float, asinf, (float), "GLIBC_2.2.5")
OLD(float, atan2f, (float, float), "GLIBC_2.2.5")
OLD(float, fmodf, (float, float), "GLIBC_2.2.5")
OLD(float, hypotf, (float, float), "GLIBC_2.2.5")
OLD(double, hypot, (double, double), "GLIBC_2.2.5")
OLD(float, sqrtf, (float), "GLIBC_2.2.5")
OLD(float, remainderf, (float, float), "GLIBC_2.2.5")
OLD(long, strtol, (const char *, char **, int), "GLIBC_2.2.5")
OLD(long long, strtoll, (const char *, char **, int), "GLIBC_2.2.5")
OLD(unsigned long, strtoul, (const char *, char **, int), "GLIBC_2.2.5")
OLD(unsigned long long, strtoull, (const char *, char **, int), "GLIBC_2.2.5")
OLD(int, __isoc99_vsscanf, (const char *, const char *, va_list), "GLIBC_2.7")

float __wrap_acosf(float x) { return old_acosf(x); }
float __wrap_asinf(float x) { return old_asinf(x); }
float __wrap_atan2f(float y, float x) { return old_atan2f(y, x); }
float __wrap_fmodf(float x, float y) { return old_fmodf(x, y); }
float __wrap_hypotf(float x, float y) { return old_hypotf(x, y); }
double __wrap_hypot(double x, double y) { return old_hypot(x, y); }
float __wrap_sqrtf(float x) { return old_sqrtf(x); }
float __wrap_remainderf(float x, float y) { return old_remainderf(x, y); }

// (glibc 2.36; the C++ library's random numbers use it.)
uint32_t __wrap_arc4random(void) {
    uint32_t value = 0;
    while (getrandom(&value, sizeof(value), 0) != sizeof(value)) {
    }
    return value;
}
long __wrap___isoc23_strtol(const char *s, char **end, int base) { return old_strtol(s, end, base); }
long long __wrap___isoc23_strtoll(const char *s, char **end, int base) { return old_strtoll(s, end, base); }
unsigned long __wrap___isoc23_strtoul(const char *s, char **end, int base) { return old_strtoul(s, end, base); }
unsigned long long __wrap___isoc23_strtoull(const char *s, char **end, int base) { return old_strtoull(s, end, base); }

int __wrap___isoc23_sscanf(const char *s, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int result = old___isoc99_vsscanf(s, format, args);
    va_end(args);
    return result;
}
