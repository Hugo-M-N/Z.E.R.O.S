#include "kstring.h"

void *memcpy(void *dst, const void *src, unsigned int n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (unsigned int i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void *memset(void *dst, int c, unsigned int n) {
    unsigned char *d = (unsigned char *)dst;
    for (unsigned int i = 0; i < n; i++) d[i] = (unsigned char)c;
    return dst;
}

void *memmove(void *dst, const void *src, unsigned int n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s) {
        for (unsigned int i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (unsigned int i = n; i > 0; i--) d[i-1] = s[i-1];
    }
    return dst;
}

int memcmp(const void *a, const void *b, unsigned int n) {
    const unsigned char *p = (const unsigned char *)a;
    const unsigned char *q = (const unsigned char *)b;
    for (unsigned int i = 0; i < n; i++) {
        if (p[i] != q[i]) return (int)p[i] - (int)q[i];
    }
    return 0;
}

unsigned int k_strlen(const char *s) {
    unsigned int n = 0;
    while (s[n]) n++;
    return n;
}

int k_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int k_strncmp(const char *a, const char *b, unsigned int n) {
    for (unsigned int i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

char *k_strncpy(char *dst, const char *src, unsigned int n) {
    unsigned int i;
    for (i = 0; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

char *k_strncat(char *dst, const char *src, unsigned int n) {
    unsigned int dlen = k_strlen(dst);
    unsigned int i;
    for (i = 0; i < n && src[i]; i++) dst[dlen + i] = src[i];
    dst[dlen + i] = '\0';
    return dst;
}

const char *k_strrchr(const char *s, int c) {
    const char *last = 0;
    for (; *s; s++)
        if (*s == (char)c) last = s;
    if ((char)c == '\0') return s;
    return last;
}

char *k_strtok(char *s, const char *delim) {
    static char *saved = 0;
    if (s) saved = s;
    if (!saved || !*saved) return 0;

    /* Saltar delimitadores iniciales */
    while (*saved) {
        int is_delim = 0;
        for (const char *d = delim; *d; d++)
            if (*saved == *d) { is_delim = 1; break; }
        if (!is_delim) break;
        saved++;
    }
    if (!*saved) return 0;

    char *token = saved;
    while (*saved) {
        int is_delim = 0;
        for (const char *d = delim; *d; d++)
            if (*saved == *d) { is_delim = 1; break; }
        if (is_delim) { *saved++ = '\0'; break; }
        saved++;
    }
    return token;
}
