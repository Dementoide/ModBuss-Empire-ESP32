/**
 * MODBUS_RTU.CPP
 * Codec RTU (F03/F04/F06) y transporte half-duplex por UART2 con DE por GPIO.
 *
 * La cabecera de 3 bytes de la respuesta usa el timeout de respuesta y el resto
 * un tope corto entre caracteres (una trama RTU llega sin pausas).
 */
#include "modbus_rtu.h"
#include "modbus_error.h"

#include <string.h>
#include <driver/uart.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static constexpr uart_port_t kUartBus = UART_NUM_2;
// Asentamiento del DE antes del primer bit (MAX485: µs; 1 ms cubre módulos lentos).
static constexpr uint32_t kAsentarDeMs = 1;
static constexpr uint32_t kEntreCaracteresMs = 50;

static uint32_t s_timeoutMs = 1000;
static int s_pinRx = -1;
static int s_pinDe = -1;

uint16_t mbCrc16(const uint8_t* datos, size_t largo)
{
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < largo; i++) {
    crc ^= datos[i];
    for (int b = 0; b < 8; b++) crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
  }
  return crc;
}

static size_t armar(uint8_t esclavo, uint8_t fn, uint16_t a, uint16_t b, uint8_t* out)
{
  out[0] = esclavo;
  out[1] = fn;
  out[2] = a >> 8;
  out[3] = a & 0xFF;
  out[4] = b >> 8;
  out[5] = b & 0xFF;
  const uint16_t crc = mbCrc16(out, 6);
  out[6] = crc & 0xFF;
  out[7] = crc >> 8;
  return 8;
}

size_t mbArmarLectura(uint8_t esclavo, uint8_t fn, uint16_t inicio, uint16_t cant, uint8_t* out)
{
  return armar(esclavo, fn, inicio, cant, out);
}

size_t mbArmarEscrituraSimple(uint8_t esclavo, uint16_t reg, uint16_t valor, uint8_t* out)
{
  return armar(esclavo, 0x06, reg, valor, out);
}

static bool crcOk(const uint8_t* trama, size_t largo)
{
  if (largo < 3) return false;
  const uint16_t crc = mbCrc16(trama, largo - 2);
  return trama[largo - 2] == (crc & 0xFF) && trama[largo - 1] == (crc >> 8);
}

// Cabecera común: esclavo, excepción y función. MB_ERR_SUCCESS = seguir.
static uint8_t validarCabecera(const uint8_t* rsp, size_t largo, uint8_t esclavo, uint8_t fn)
{
  if (largo < 5) return MB_ERR_RESPONSE_TIMED_OUT;
  if (rsp[0] != esclavo) return MB_ERR_INVALID_SLAVE_ID;
  if (rsp[1] == (fn | 0x80)) return crcOk(rsp, 5) ? rsp[2] : MB_ERR_INVALID_CRC;
  if (rsp[1] != fn) return MB_ERR_INVALID_FUNCTION;
  return MB_ERR_SUCCESS;
}

uint8_t mbParsearLectura(const uint8_t* rsp, size_t largo, uint8_t esclavo, uint8_t fn,
                         uint16_t cant, uint16_t* out)
{
  const uint8_t r = validarCabecera(rsp, largo, esclavo, fn);
  if (r != MB_ERR_SUCCESS) return r;
  const size_t esperado = 3 + 2u * cant + 2;
  if (rsp[2] != 2 * cant || largo < esperado) return MB_ERR_RESPONSE_TIMED_OUT;
  if (!crcOk(rsp, esperado)) return MB_ERR_INVALID_CRC;
  for (uint16_t i = 0; i < cant; i++) {
    out[i] = ((uint16_t)rsp[3 + 2 * i] << 8) | rsp[4 + 2 * i];
  }
  return MB_ERR_SUCCESS;
}

uint8_t mbParsearEscrituraSimple(const uint8_t* rsp, size_t largo, const uint8_t* peticion)
{
  const uint8_t r = validarCabecera(rsp, largo, peticion[0], 0x06);
  if (r != MB_ERR_SUCCESS) return r;
  if (largo < 8) return MB_ERR_RESPONSE_TIMED_OUT;
  if (!crcOk(rsp, 8)) return MB_ERR_INVALID_CRC;
  // Un eco distinto (registro o valor) es una respuesta que no cuadra.
  return memcmp(rsp, peticion, 8) == 0 ? MB_ERR_SUCCESS : MB_ERR_INVALID_FUNCTION;
}

bool mbIniciar(uint32_t baud, int tx, int rx, int de)
{
  if (uart_is_driver_installed(kUartBus)) {
    return uart_set_baudrate(kUartBus, baud) == ESP_OK;
  }
  gpio_reset_pin((gpio_num_t)de);
  gpio_set_direction((gpio_num_t)de, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)de, 0);

  uart_config_t cfg = {};
  cfg.baud_rate = (int)baud;
  cfg.data_bits = UART_DATA_8_BITS;
  cfg.parity = UART_PARITY_DISABLE;
  cfg.stop_bits = UART_STOP_BITS_1;
  cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  cfg.source_clk = UART_SCLK_DEFAULT;
  if (uart_driver_install(kUartBus, 512, 0, 0, nullptr, 0) != ESP_OK) return false;
  if (uart_param_config(kUartBus, &cfg) != ESP_OK) return false;
  if (uart_set_pin(kUartBus, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) return false;
  s_pinRx = rx;
  s_pinDe = de;
  return true;
}

void mbFijarTimeoutMs(uint32_t ms)
{
  s_timeoutMs = ms;
}

// Envía `peticion` y recibe la respuesta en `rsp`; devuelve los bytes leídos.
static size_t intercambiar(const uint8_t* peticion, size_t largoPeticion, uint8_t* rsp)
{
  uart_flush_input(kUartBus);
  gpio_set_level((gpio_num_t)s_pinDe, 1);
  vTaskDelay(pdMS_TO_TICKS(kAsentarDeMs));
  uart_write_bytes(kUartBus, peticion, largoPeticion);
  uart_wait_tx_done(kUartBus, pdMS_TO_TICKS(100));
  gpio_set_level((gpio_num_t)s_pinDe, 0);

  int got = uart_read_bytes(kUartBus, rsp, 3, pdMS_TO_TICKS(s_timeoutMs));
  if (got < 3) return got < 0 ? 0 : (size_t)got;

  const uint8_t fn = peticion[1];
  size_t resto;
  if (rsp[1] & 0x80) resto = 2;              // excepción: código + CRC
  else if (fn == 0x06) resto = 5;            // eco: 8 bytes en total
  else resto = (size_t)rsp[2] + 2;           // datos + CRC
  if (3 + resto > MB_MAX_RESPUESTA) resto = MB_MAX_RESPUESTA - 3;

  int mas = uart_read_bytes(kUartBus, rsp + 3, resto, pdMS_TO_TICKS(kEntreCaracteresMs));
  if (mas < 0) mas = 0;
  return 3 + (size_t)mas;
}

uint8_t mbLeer(uint8_t esclavo, uint8_t fn, uint16_t inicio, uint16_t cant, uint16_t* out)
{
  if (fn != 3 && fn != 4) return MB_ERR_ILLEGAL_FUNCTION;
  if (cant == 0 || cant > MB_MAX_REGISTROS) return MB_ERR_ILLEGAL_DATA_VALUE;
  if (!uart_is_driver_installed(kUartBus)) return MB_ERR_RESPONSE_TIMED_OUT;

  uint8_t req[8];
  uint8_t rsp[MB_MAX_RESPUESTA];
  const size_t n = intercambiar(req, mbArmarLectura(esclavo, fn, inicio, cant, req), rsp);
  return mbParsearLectura(rsp, n, esclavo, fn, cant, out);
}

uint8_t mbEscribirSimple(uint8_t esclavo, uint16_t reg, uint16_t valor)
{
  if (!uart_is_driver_installed(kUartBus)) return MB_ERR_RESPONSE_TIMED_OUT;
  uint8_t req[8];
  uint8_t rsp[MB_MAX_RESPUESTA];
  const size_t n = intercambiar(req, mbArmarEscrituraSimple(esclavo, reg, valor, req), rsp);
  return mbParsearEscrituraSimple(rsp, n, req);
}

int mbNivelRx()
{
  if (s_pinRx < 0) return -1;
  return gpio_get_level((gpio_num_t)s_pinRx);
}
