/**
 * FLUJO.H
 * Protocolo de líneas '@' con el que la interfaz gráfica del PC (gui/graficador.py)
 * pide muestras de registros sin pasar por los menús.
 *
 * PC -> ESP (una línea, se atiende desde cualquier menú):
 *   @hola
 *   @leer id=1 baud=9600 fn=3 regs=0,4,8-10,19 ms=500 [n=N] [hueco=16]
 *     ms = periodo de muestreo (0 = lo más rápido posible); n = cantidad de
 *     muestras (sin n, hasta que llegue cualquier línea, p. ej. "@stop");
 *     hueco = registros faltantes que se leen de más para unir tramas.
 * ESP -> PC:
 *   @ok ModbusEmpire flujo=1 max_regs=N
 *   @cfg regs=N bloques=B trama_ms=X silencio_ms=Y
 *        trama_ms = tiempo en el cable de todas las tramas de una muestra.
 *   @d <t_ms> <dur_ms> v0 v1 ...   valores crudos u16 en el orden pedido;
 *        dur_ms = lo que tardó la muestra (incluye la latencia del esclavo).
 *   @e <t_ms> <codigo_hex> <NOMBRE>   la muestra falló (MB_ERR_*).
 *   @aviso <texto> | @error <texto> | @fin
 */
#pragma once

#include <stdint.h>
#include <string>

// Máximo de registros por @leer (una línea @d por muestra).
static const size_t FLUJO_MAX_REGS = 64;

// Atiende una línea '@'. `idSesion`/`baudSesion` son los valores por defecto;
// al volver, el bus queda en el baudrate en que estaba antes (`baudSesion`).
void flujoComando(const std::string& linea, uint8_t idSesion, uint32_t baudSesion);
