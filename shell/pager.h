#pragma once
#include <stddef.h>

/*
 * pager_show — muestra un buffer de texto con scroll interactivo.
 *
 * Si el texto cabe en una pantalla, lo imprime directamente.
 * Si no, entra en modo raw y permite navegar con:
 *   AvPág / Espacio / ↓   → avanzar una página / línea
 *   RePág / ↑              → retroceder una página / línea
 *   q                      → salir
 */
void pager_show(const char *buf, size_t len);
