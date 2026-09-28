/* crttest - the C runtime, exercised the way a real program uses it.
 *
 * Formatting is the part most likely to be subtly wrong and most likely to
 * matter: a program that prints a number a tenth of a percent out looks like
 * it works right up until somebody checks.  So every conversion is compared
 * against the text it is supposed to produce. */
#include "winapi.h"

static void fmt(const char *expect, const char *format, ...);

/* Comparing formatted output against what it should be, one case at a time. */
static char buf[256];

#define CASE(expect, ...) do {                          \
        sprintf(buf, __VA_ARGS__);                      \
        check(!strcmp(buf, expect), "\"" expect "\"");  \
        if (strcmp(buf, expect)) printf("       got \"%s\"\n", buf); \
    } while (0)

static int compare_ints(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

int main(void) {
    printf("crttest: the C runtime\n");

    printf(" integers\n");
    CASE("42", "%d", 42);
    CASE("-42", "%d", -42);
    CASE("0", "%d", 0);
    CASE("  42", "%4d", 42);
    CASE("42  ", "%-4d", 42);
    CASE("0042", "%04d", 42);
    CASE("+42", "%+d", 42);
    CASE("2a", "%x", 42);
    CASE("2A", "%X", 42);
    CASE("0x2a", "%#x", 42);
    CASE("52", "%o", 42);
    CASE("4294967254", "%u", (unsigned)-42);
    CASE("9223372036854775807", "%lld", 9223372036854775807LL);
    CASE("-9223372036854775808", "%lld", (long long)-9223372036854775807LL - 1);
    CASE("1234567890123", "%I64d", 1234567890123LL);

    printf(" text\n");
    CASE("hello", "%s", "hello");
    CASE("   hi", "%5s", "hi");
    CASE("hi   ", "%-5s", "hi");
    CASE("hel", "%.3s", "hello");
    CASE("A", "%c", 'A');
    CASE("100%", "100%%");
    CASE("a-b", "%s-%s", "a", "b");

    printf(" floating point\n");
    CASE("3.140000", "%f", 3.14);
    CASE("3.14", "%.2f", 3.14);
    CASE("3.1", "%.1f", 3.14159);
    CASE("-2.50", "%.2f", -2.5);
    CASE("0.00", "%.2f", 0.0);
    CASE("1.00", "%.2f", 0.999);
    CASE("10.0", "%.1f", 9.99);
    CASE("100", "%.0f", 100.0);
    CASE("1234.5678", "%.4f", 1234.5678);
    CASE("  3.14", "%6.2f", 3.14);
    CASE("1.500000e+00", "%e", 1.5);
    CASE("1.5", "%g", 1.5);

    printf(" mixed\n");
    CASE("x=5 y=2.50 name=bob", "x=%d y=%.2f name=%s", 5, 2.5, "bob");
    CASE("[  7][3.0 ][ab]", "[%3d][%-4.1f][%2s]", 7, 3.0, "ab");

    printf(" strings\n");
    char s[64];
    strcpy(s, "hello");
    strcat(s, ", world");
    check(!strcmp(s, "hello, world"), "strcpy then strcat");
    check(strlen(s) == 12, "strlen");
    check(strcmp("a", "b") < 0, "strcmp orders");
    check(strstr(s, "world") == s + 7, "strstr finds");
    check(strstr(s, "zebra") == NULL, "strstr misses");
    check(atoi("  -123") == -123, "atoi");

    printf(" memory\n");
    void *p = malloc(1000);
    check(p != NULL, "malloc returns memory");
    memset(p, 0xAB, 1000);
    check(((BYTE *)p)[999] == 0xAB, "the whole block is writable");
    free(p);

    char a[8], b[8];
    memset(a, 1, sizeof a);
    memcpy(b, a, sizeof a);
    check(memcmp(a, b, sizeof a) == 0, "memcpy then memcmp");

    printf(" maths\n");
    check(fabs(sqrt(16.0) - 4.0) < 1e-9, "sqrt(16) is 4");
    check(fabs(sqrt(2.0) - 1.4142135623730951) < 1e-9, "sqrt(2)");
    check(fabs(pow(2.0, 10.0) - 1024.0) < 1e-9, "2 to the tenth");
    check(fabs(sin(0.0)) < 1e-9, "sin(0) is 0");
    check(fabs(cos(0.0) - 1.0) < 1e-9, "cos(0) is 1");
    check(fabs(sin(1.5707963267948966) - 1.0) < 1e-6, "sin of a right angle is 1");

    printf(" sorting\n");
    int values[8] = { 5, 3, 9, 1, 7, 2, 8, 4 };
    qsort(values, 8, sizeof(int), compare_ints);
    int sorted = 1;
    for (int i = 1; i < 8; i++) if (values[i] < values[i - 1]) sorted = 0;
    check(sorted, "qsort puts eight numbers in order");
    check(values[0] == 1 && values[7] == 9, "the ends are right");

    printf(" the heap through kernel32\n");
    HANDLE heap = GetProcessHeap();
    void *block = HeapAlloc(heap, 8 /* zeroed */, 256);
    check(block != NULL, "HeapAlloc returns memory");
    check(HeapSize(heap, 0, block) == 256, "HeapSize remembers the size");
    int all_zero = 1;
    for (int i = 0; i < 256; i++) if (((BYTE *)block)[i]) all_zero = 0;
    check(all_zero, "HEAP_ZERO_MEMORY cleared it");
    check(HeapFree(heap, 0, block), "HeapFree accepts it back");

    return report("crttest");
}
