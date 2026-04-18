#pragma once

/* Funciones de cadena/memoria para el kernel (sin libc).
 * memcpy/memset/memmove se exportan sin static porque el compilador
 * puede generar llamadas implícitas a ellas con -ffreestanding. */

void *memcpy (void *dst, const void *src, unsigned int n);
void *memset (void *dst, int c, unsigned int n);
void *memmove(void *dst, const void *src, unsigned int n);
int   memcmp (const void *a, const void *b, unsigned int n);

unsigned int k_strlen (const char *s);
int          k_strcmp (const char *a, const char *b);
int          k_strncmp(const char *a, const char *b, unsigned int n);
char        *k_strncpy(char *dst, const char *src, unsigned int n);
char        *k_strncat(char *dst, const char *src, unsigned int n);
const char  *k_strrchr(const char *s, int c);
char        *k_strtok (char *s, const char *delim);
