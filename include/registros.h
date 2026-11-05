/**
 * REGISTROS.H
 * Tipos de dato Modbus y su decodificación desde registros crudos de 16 bits.
 *
 * Sin hardware ni IDF: lo prueba la suite Unity (test/test_registros).
 * Modbus transmite cada registro big-endian; un valor de 32 bits ocupa dos
 * registros y el orden de las palabras depende del fabricante (ABCD o CDAB).
 */
#pragma once

#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

enum TipoDato : uint8_t { T_U16, T_S16, T_U32, T_S32, T_F32 };

// ABCD = palabra alta primero (lo habitual); CDAB = palabra baja primero.
enum OrdenPalabras : uint8_t { ORDEN_ABCD, ORDEN_CDAB };

inline uint8_t palabrasDeTipo(TipoDato t)
{
  return (t == T_U16 || t == T_S16) ? 1 : 2;
}

inline const char* nombreTipo(TipoDato t)
{
  switch (t) {
    case T_U16: return "u16";
    case T_S16: return "s16";
    case T_U32: return "u32";
    case T_S32: return "s32";
    case T_F32: return "f32";
  }
  return "u16";
}

inline const char* nombreOrden(OrdenPalabras o)
{
  return o == ORDEN_CDAB ? "CDAB" : "ABCD";
}

inline bool igualSinMayus(const char* a, const char* b)
{
  while (*a && *b) {
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    a++;
    b++;
  }
  return *a == *b;
}

// Acepta "u16", "s16", "u32", "s32", "f32" (o "float"); false si no lo reconoce.
inline bool parsearTipo(const char* s, TipoDato* out)
{
  if (s == nullptr) return false;
  if (igualSinMayus(s, "u16")) { *out = T_U16; return true; }
  if (igualSinMayus(s, "s16")) { *out = T_S16; return true; }
  if (igualSinMayus(s, "u32")) { *out = T_U32; return true; }
  if (igualSinMayus(s, "s32")) { *out = T_S32; return true; }
  if (igualSinMayus(s, "f32") || igualSinMayus(s, "float")) { *out = T_F32; return true; }
  return false;
}

inline bool parsearOrden(const char* s, OrdenPalabras* out)
{
  if (s == nullptr) return false;
  if (igualSinMayus(s, "ABCD")) { *out = ORDEN_ABCD; return true; }
  if (igualSinMayus(s, "CDAB")) { *out = ORDEN_CDAB; return true; }
  return false;
}

// Número decimal o hexadecimal ("0x0200"). Un cero a la izquierda NO es octal
// ("010" = 10): en los manuales de sensores "0010" es decimal.
inline bool parsearNumero(const char* s, long* out)
{
  if (s == nullptr) return false;
  while (*s == ' ') s++;
  if (*s == '\0') return false;
  int base = 10;
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    base = 16;
    s += 2;
    if (*s == '\0') return false;
  }
  char* fin = nullptr;
  long v = strtol(s, &fin, base);
  while (fin && *fin == ' ') fin++;
  if (fin == nullptr || *fin != '\0') return false;
  *out = v;
  return true;
}

// Junta dos registros en 32 bits según el orden de palabras.
inline uint32_t unir32(uint16_t r0, uint16_t r1, OrdenPalabras orden)
{
  return orden == ORDEN_ABCD ? ((uint32_t)r0 << 16) | r1 : ((uint32_t)r1 << 16) | r0;
}

inline float bitsAFloat(uint32_t bits)
{
  float f;
  memcpy(&f, &bits, sizeof(f));
  return f;
}

// Valor crudo (sin escala) de `regs`, que debe traer palabrasDeTipo(tipo) registros.
inline double decodificar(const uint16_t* regs, TipoDato tipo, OrdenPalabras orden)
{
  switch (tipo) {
    case T_U16: return (double)regs[0];
    case T_S16: return (double)(int16_t)regs[0];
    case T_U32: return (double)unir32(regs[0], regs[1], orden);
    case T_S32: return (double)(int32_t)unir32(regs[0], regs[1], orden);
    case T_F32: return (double)bitsAFloat(unir32(regs[0], regs[1], orden));
  }
  return 0.0;
}

// Magnitud final: crudo * mult + off.
inline double aplicarEscala(double crudo, double mult, double off)
{
  return crudo * mult + off;
}
