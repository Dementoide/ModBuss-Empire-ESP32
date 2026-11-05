/**
 * MODBUS_RTU.H
 * Maestro Modbus RTU (F03, F04, F06) sobre driver/uart del IDF, DE por GPIO.
 *
 * Derivado del maestro de node_minkai (perception/sensors/modbus/modbus_rtu).
 * Armado y parseo de tramas no tocan hardware: la suite Unity los prueba con
 * vectores conocidos. Resultado de cada intercambio: códigos MB_ERR_* de
 * modbus_error.h (0xE2 = nadie habló, 0xE0/0xE1/0xE3 = bytes que no cuadran,
 * 0x01..0x04 = excepción del esclavo).
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

// Máximo de registros por lectura F03/F04 que admite la norma.
static const uint16_t MB_MAX_REGISTROS = 125;

// Largo máximo de una respuesta RTU: esclavo + fn + bytes + datos + CRC.
static const size_t MB_MAX_RESPUESTA = 3 + 2 * MB_MAX_REGISTROS + 2;

uint16_t mbCrc16(const uint8_t* datos, size_t largo);

// Trama de lectura F03/F04 (8 bytes en `out`); devuelve su largo.
size_t mbArmarLectura(uint8_t esclavo, uint8_t fn, uint16_t inicio, uint16_t cant, uint8_t* out);

// Trama "Write Single Register" F06 (8 bytes en `out`); devuelve su largo.
size_t mbArmarEscrituraSimple(uint8_t esclavo, uint16_t reg, uint16_t valor, uint8_t* out);

// Valida la respuesta a una lectura y copia `cant` registros a `out`.
uint8_t mbParsearLectura(const uint8_t* rsp, size_t largo, uint8_t esclavo, uint8_t fn,
                         uint16_t cant, uint16_t* out);

// La respuesta correcta a F06 es el eco exacto de la petición (8 bytes).
uint8_t mbParsearEscrituraSimple(const uint8_t* rsp, size_t largo, const uint8_t* peticion);

// Abre el UART del bus (8N1) en los pines dados o cambia su baudrate si ya está
// abierto. `de` es el pin RE/DE del transceptor (alto = transmitir).
bool mbIniciar(uint32_t baud, int tx, int rx, int de);

// Tope de espera del primer byte de la respuesta (por defecto 1000 ms).
void mbFijarTimeoutMs(uint32_t ms);

// Intercambios completos. `fn` 3 = Holding, 4 = Input.
uint8_t mbLeer(uint8_t esclavo, uint8_t fn, uint16_t inicio, uint16_t cant, uint16_t* out);
uint8_t mbEscribirSimple(uint8_t esclavo, uint16_t reg, uint16_t valor);

// Nivel actual del pin RX del bus (-1 si el bus no está abierto).
int mbNivelRx();
