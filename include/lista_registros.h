/**
 * LISTA_REGISTROS.H
 * Lista de registros escrita a mano ("0,4,8-10,0x13") y su agrupación en las
 * lecturas Modbus que la cubren.
 *
 * Sin hardware ni IDF: lo prueba la suite Unity (test/test_registros).
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <ctype.h>
#include <algorithm>
#include <vector>

struct BloqueLectura {
  uint16_t inicio;
  uint16_t cant;
};

// Un número decimal o hex ("0x13") al inicio de `s`; avanza `s`. false si no hay.
inline bool leerNumeroLista(const char** s, long* out)
{
  const char* p = *s;
  while (*p == ' ') p++;
  int base = 10;
  if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
  if (!isxdigit((unsigned char)*p)) return false;
  char* fin = nullptr;
  long v = strtol(p, &fin, base);
  if (fin == p) return false;
  while (*fin == ' ') fin++;
  *s = fin;
  *out = v;
  return true;
}

// Registros en el orden pedido, sin repetidos. Acepta números sueltos e
// intervalos inclusivos separados por coma: "0,4,8-10,0x13". false si la
// sintaxis falla, una dirección pasa de 0xFFFF, un intervalo está al revés o
// el total supera `maxRegs`.
inline bool parsearListaRegistros(const char* s, std::vector<uint16_t>* out, size_t maxRegs)
{
  out->clear();
  if (s == nullptr) return false;
  while (true) {
    long a;
    if (!leerNumeroLista(&s, &a)) return false;
    long b = a;
    if (*s == '-') {
      s++;
      if (!leerNumeroLista(&s, &b)) return false;
    }
    if (a < 0 || b > 0xFFFF || b < a) return false;
    for (long r = a; r <= b; r++) {
      if (std::find(out->begin(), out->end(), (uint16_t)r) != out->end()) continue;
      if (out->size() >= maxRegs) return false;
      out->push_back((uint16_t)r);
    }
    if (*s == '\0') return !out->empty();
    if (*s != ',') return false;
    s++;
  }
}

// Bloques ordenados que cubren `regs` con el mínimo de tramas: une dos
// registros si entre ellos faltan a lo sumo `maxHueco` (leer un registro de
// más cuesta 2 bytes; una trama nueva, la latencia del esclavo) y el bloque no
// pasa de `maxCant`. maxHueco 0 = solo une registros consecutivos.
inline void agruparBloques(const std::vector<uint16_t>& regs, uint16_t maxHueco, uint16_t maxCant,
                           std::vector<BloqueLectura>* out)
{
  out->clear();
  std::vector<uint16_t> ord(regs);
  std::sort(ord.begin(), ord.end());
  ord.erase(std::unique(ord.begin(), ord.end()), ord.end());
  for (uint16_t r : ord) {
    if (!out->empty()) {
      BloqueLectura& b = out->back();
      const uint32_t fin = (uint32_t)b.inicio + b.cant;   // primer registro fuera del bloque
      const uint32_t nuevaCant = (uint32_t)r - b.inicio + 1;
      if ((uint32_t)r - fin <= maxHueco && nuevaCant <= maxCant) {
        b.cant = (uint16_t)nuevaCant;
        continue;
      }
    }
    out->push_back({r, 1});
  }
}

// Tiempo en el cable de un intercambio F03/F04 de `cant` registros (ms):
// petición de 8 bytes, respuesta de 5 + 2*cant, 10 bits por byte (8N1).
inline double msTramaLectura(uint16_t cant, uint32_t baud)
{
  return (8.0 + 5.0 + 2.0 * cant) * 10.0 * 1000.0 / (double)baud;
}

// Silencio mínimo entre tramas RTU (3,5 caracteres), con 1 ms de margen.
inline uint32_t msSilencioEntreTramas(uint32_t baud)
{
  return (35000u + baud - 1) / baud + 1;
}
