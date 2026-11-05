/**
 * CONSOLA.CPP
 * Lectura de líneas por UART0 con el driver del IDF y stdout por el mismo driver.
 */
#include "consola.h"

#include <stdio.h>
#include <driver/uart.h>
#include <driver/uart_vfs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static constexpr uart_port_t kUartConsola = UART_NUM_0;

void consolaIniciar()
{
  fflush(stdout);
  uart_driver_install(kUartConsola, 256, 0, 0, nullptr, 0);
  // stdout pasa por el driver: sin esto printf y el driver pelean por la FIFO.
  uart_vfs_dev_use_driver(kUartConsola);
  setvbuf(stdout, nullptr, _IONBF, 0);
}

static std::string recortar(const std::string& s)
{
  size_t a = s.find_first_not_of(" \t");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t");
  return s.substr(a, b - a + 1);
}

std::string leerLinea()
{
  // CRLF: el LF que sigue a un CR ya cerró la línea anterior y no es una vacía.
  static char ultimoFin = 0;
  std::string linea;
  fflush(stdout);
  while (true) {
    uint8_t c;
    if (uart_read_bytes(kUartConsola, &c, 1, portMAX_DELAY) != 1) continue;
    if (c == '\r' || c == '\n') {
      if (linea.empty() && ultimoFin == '\r' && c == '\n') { ultimoFin = 0; continue; }
      ultimoFin = (char)c;
      break;
    }
    ultimoFin = 0;
    if (c == 0x08 || c == 0x7F) {
      if (!linea.empty()) linea.pop_back();
      continue;
    }
    if (c >= 0x20) linea.push_back((char)c);
  }
  linea = recortar(linea);
  printf("> %s\n", linea.c_str());
  return linea;
}

bool hayEntrada()
{
  size_t pendiente = 0;
  uart_get_buffered_data_len(kUartConsola, &pendiente);
  return pendiente > 0;
}

void vaciarEntrada()
{
  vTaskDelay(pdMS_TO_TICKS(20));
  uart_flush_input(kUartConsola);
}
