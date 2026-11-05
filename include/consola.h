/**
 * CONSOLA.H
 * Entrada de líneas por la consola UART0 (driver del IDF) para los menús.
 *
 * La salida es printf (stdout por el mismo driver). Acepta fin de línea LF,
 * CR o CRLF y borra con backspace; cada línea leída se repite con "> ".
 */
#pragma once

#include <string>

void consolaIniciar();

// Bloquea hasta recibir una línea; la devuelve sin espacios en los extremos.
std::string leerLinea();

// true si llegó algo por la consola (no bloquea ni consume).
bool hayEntrada();

// Descarta lo pendiente (p. ej. la tecla que cortó un ciclo).
void vaciarEntrada();
