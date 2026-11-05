/**
 * MODBUS_ERROR.H
 * Códigos de resultado Modbus (MB_ERR_*) con su nombre y causa probable.
 *
 * Tabla traída de node_minkai (perception/sensors/modbus/modbus_error). Lo
 * que importa en el banco: TIMED_OUT = nadie habló; INVALID_SLAVE_ID / CRC =
 * llegaron bytes que no cuadran (baudrate u otro equipo en el bus).
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

// Valores fijos (convención histórica de los maestros RTU): no reordenar.
#define MB_ERR_SUCCESS              0x00
#define MB_ERR_ILLEGAL_FUNCTION     0x01
#define MB_ERR_ILLEGAL_DATA_ADDRESS 0x02
#define MB_ERR_ILLEGAL_DATA_VALUE   0x03
#define MB_ERR_SLAVE_DEVICE_FAILURE 0x04
#define MB_ERR_INVALID_SLAVE_ID     0xE0
#define MB_ERR_INVALID_FUNCTION     0xE1
#define MB_ERR_RESPONSE_TIMED_OUT   0xE2
#define MB_ERR_INVALID_CRC          0xE3

struct InfoErrorModbus { uint8_t codigo; const char* nombre; const char* causa; };

static const InfoErrorModbus ERRORES_MODBUS[] = {
  { MB_ERR_SUCCESS, "SUCCESS", "sin error" },
  { MB_ERR_ILLEGAL_FUNCTION, "ILLEGAL_FUNCTION",
    "el esclavo respondio pero no soporta la funcion (probar F03 <-> F04)" },
  { MB_ERR_ILLEGAL_DATA_ADDRESS, "ILLEGAL_DATA_ADDRESS",
    "registro fuera de rango en el esclavo - revisar direccion/largo (40001 -> 0)" },
  { MB_ERR_ILLEGAL_DATA_VALUE, "ILLEGAL_DATA_VALUE", "valor o cantidad rechazada por el esclavo" },
  { MB_ERR_SLAVE_DEVICE_FAILURE, "SLAVE_DEVICE_FAILURE", "fallo interno del esclavo" },
  { MB_ERR_INVALID_SLAVE_ID, "INVALID_SLAVE_ID",
    "respondio OTRA direccion - baudrate erroneo u otro dispositivo en el bus" },
  { MB_ERR_INVALID_FUNCTION, "INVALID_FUNCTION",
    "la respuesta no corresponde a la peticion - trama desincronizada" },
  { MB_ERR_RESPONSE_TIMED_OUT, "RESPONSE_TIMED_OUT",
    "nadie respondio - sin alimentacion, A/B invertido, ID inexistente o DE trabado" },
  { MB_ERR_INVALID_CRC, "INVALID_CRC",
    "CRC malo - baudrate/paridad erroneos, ruido en el par o falta terminacion" },
};

// Una excepción (0x01..0x04) prueba que el esclavo existe y escuchó.
inline bool mbEsExcepcion(uint8_t codigo)
{
  return codigo >= MB_ERR_ILLEGAL_FUNCTION && codigo <= MB_ERR_SLAVE_DEVICE_FAILURE;
}

inline const InfoErrorModbus* buscarErrorModbus(uint8_t codigo)
{
  for (size_t i = 0; i < sizeof(ERRORES_MODBUS) / sizeof(ERRORES_MODBUS[0]); i++) {
    if (ERRORES_MODBUS[i].codigo == codigo) return &ERRORES_MODBUS[i];
  }
  return nullptr;
}

inline const char* nombreErrorModbus(uint8_t codigo)
{
  const InfoErrorModbus* e = buscarErrorModbus(codigo);
  return e ? e->nombre : "UNKNOWN";
}

inline const char* causaErrorModbus(uint8_t codigo)
{
  const InfoErrorModbus* e = buscarErrorModbus(codigo);
  return e ? e->causa : "codigo no mapeado";
}
