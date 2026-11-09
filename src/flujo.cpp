/**
 * FLUJO.CPP
 * Muestreo continuo de una lista de registros para la interfaz gráfica.
 *
 * Corre en la main task, dentro de leerLinea: mientras muestrea no hay menú.
 * Cualquier línea que llegue por la consola detiene el muestreo.
 */
#include "flujo.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <vector>

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "consola.h"
#include "lista_registros.h"
#include "modbus_error.h"
#include "modbus_rtu.h"

static const uint16_t kHuecoPorDefecto = 16;

// Valor de `clave=` en la línea (tokens separados por espacio). false si no está.
static bool valorDe(const std::string& linea, const char* clave, std::string* out)
{
  const std::string buscada = std::string(" ") + clave + "=";
  size_t p = linea.find(buscada);
  if (p == std::string::npos) return false;
  p += buscada.size();
  size_t fin = linea.find(' ', p);
  *out = linea.substr(p, fin == std::string::npos ? std::string::npos : fin - p);
  return true;
}

static long numeroDe(const std::string& linea, const char* clave, long defecto)
{
  std::string v;
  if (!valorDe(linea, clave, &v)) return defecto;
  char* fin = nullptr;
  long n = strtol(v.c_str(), &fin, 0);
  return (fin == v.c_str()) ? defecto : n;
}

static uint32_t msDesdeArranque()
{
  return (uint32_t)(esp_timer_get_time() / 1000);
}

// Plan de lectura: bloques a pedir y, por cada registro pedido, de qué bloque
// y posición sale su valor.
struct Plan {
  std::vector<BloqueLectura> bloques;
  std::vector<uint16_t> bloqueDe;
  std::vector<uint16_t> posicionEn;
  double tramaMs = 0;
};

static void armarPlan(const std::vector<uint16_t>& regs, uint16_t hueco, uint32_t baud, Plan* p)
{
  agruparBloques(regs, hueco, MB_MAX_REGISTROS, &p->bloques);
  p->bloqueDe.assign(regs.size(), 0);
  p->posicionEn.assign(regs.size(), 0);
  p->tramaMs = 0;
  for (size_t b = 0; b < p->bloques.size(); b++) {
    p->tramaMs += msTramaLectura(p->bloques[b].cant, baud);
  }
  for (size_t i = 0; i < regs.size(); i++) {
    for (size_t b = 0; b < p->bloques.size(); b++) {
      const BloqueLectura& bl = p->bloques[b];
      if (regs[i] >= bl.inicio && regs[i] < bl.inicio + bl.cant) {
        p->bloqueDe[i] = (uint16_t)b;
        p->posicionEn[i] = regs[i] - bl.inicio;
        break;
      }
    }
  }
}

static void imprimirCfg(const std::vector<uint16_t>& regs, const Plan& p, uint32_t silencioMs)
{
  printf("@cfg regs=%u bloques=%u trama_ms=%.1f silencio_ms=%u\n", (unsigned)regs.size(),
         (unsigned)p.bloques.size(), p.tramaMs, (unsigned)silencioMs);
}

// Espera `ms` atento a la consola. true si llegó una línea (hay que parar).
static bool esperarAtento(uint32_t ms)
{
  const int64_t fin = esp_timer_get_time() + (int64_t)ms * 1000;
  while (esp_timer_get_time() < fin) {
    if (hayEntrada()) return true;
    int64_t resto = (fin - esp_timer_get_time()) / 1000;
    vTaskDelay(pdMS_TO_TICKS(resto > 10 ? 10 : (resto > 0 ? resto : 1)));
  }
  return hayEntrada();
}

static void leerContinuo(const std::string& linea, uint8_t idSesion, uint32_t baudSesion)
{
  const long id = numeroDe(linea, "id", idSesion);
  const long baud = numeroDe(linea, "baud", (long)baudSesion);
  const long fn = numeroDe(linea, "fn", 3);
  const long periodo = numeroDe(linea, "ms", 1000);
  const long n = numeroDe(linea, "n", 0);
  long hueco = numeroDe(linea, "hueco", kHuecoPorDefecto);
  std::string lista;
  std::vector<uint16_t> regs;

  if (id < 0 || id > 247) { printf("@error id fuera de 0..247\n"); return; }
  if (baud < 300 || baud > 1000000) { printf("@error baud invalido\n"); return; }
  if (fn != 3 && fn != 4) { printf("@error fn debe ser 3 o 4\n"); return; }
  if (periodo < 0) { printf("@error ms negativo\n"); return; }
  if (hueco < 0 || hueco > 124) hueco = kHuecoPorDefecto;
  if (!valorDe(linea, "regs", &lista) || !parsearListaRegistros(lista.c_str(), &regs, FLUJO_MAX_REGS)) {
    printf("@error regs invalido (ej. regs=0,4,8-10,0x13; maximo %u)\n", (unsigned)FLUJO_MAX_REGS);
    return;
  }

  if (!mbFijarBaud((uint32_t)baud)) { printf("@error bus Modbus no abierto\n"); return; }
  const uint32_t silencio = msSilencioEntreTramas((uint32_t)baud);
  Plan plan;
  armarPlan(regs, (uint16_t)hueco, (uint32_t)baud, &plan);
  imprimirCfg(regs, plan, silencio);
  vaciarEntrada();

  std::vector<uint16_t> buf(MB_MAX_REGISTROS);
  std::vector<std::vector<uint16_t>> datos(plan.bloques.size());
  long hechas = 0;
  char linea_d[16 + 7 * FLUJO_MAX_REGS + 32];

  while (n == 0 || hechas < n) {
    const int64_t t0 = esp_timer_get_time();
    uint8_t res = MB_ERR_SUCCESS;
    for (size_t b = 0; b < plan.bloques.size() && res == MB_ERR_SUCCESS; b++) {
      if (b > 0) vTaskDelay(pdMS_TO_TICKS(silencio));
      const BloqueLectura& bl = plan.bloques[b];
      res = mbLeer((uint8_t)id, (uint8_t)fn, bl.inicio, bl.cant, buf.data());
      if (res == MB_ERR_SUCCESS) datos[b].assign(buf.begin(), buf.begin() + bl.cant);
    }

    // Un esclavo que no tiene los registros intermedios rechaza el bloque
    // unido: se rehace el plan leyendo solo lo pedido.
    if (res == MB_ERR_ILLEGAL_DATA_ADDRESS && hueco > 0 && plan.bloques.size() < regs.size()) {
      hueco = 0;
      armarPlan(regs, 0, (uint32_t)baud, &plan);
      datos.assign(plan.bloques.size(), {});
      printf("@aviso el esclavo rechaza registros intermedios: leo solo los pedidos\n");
      imprimirCfg(regs, plan, silencio);
      if (esperarAtento(silencio)) break;
      continue;
    }

    const uint32_t t = (uint32_t)(t0 / 1000);
    const uint32_t dur = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    if (res == MB_ERR_SUCCESS) {
      int k = snprintf(linea_d, sizeof(linea_d), "@d %lu %lu", (unsigned long)t, (unsigned long)dur);
      for (size_t i = 0; i < regs.size() && k < (int)sizeof(linea_d) - 8; i++) {
        k += snprintf(linea_d + k, sizeof(linea_d) - k, " %u",
                      datos[plan.bloqueDe[i]][plan.posicionEn[i]]);
      }
      printf("%s\n", linea_d);
    } else {
      printf("@e %lu %02X %s\n", (unsigned long)t, res, nombreErrorModbus(res));
    }
    hechas++;

    // El silencio entre tramas se respeta siempre, aunque el periodo sea 0.
    const uint32_t transcurrido = msDesdeArranque() - t;
    uint32_t espera = (periodo > (long)transcurrido) ? (uint32_t)periodo - transcurrido : 0;
    if (espera < silencio) espera = silencio;
    if (esperarAtento(espera)) break;
  }

  vaciarEntrada();
  mbFijarBaud(baudSesion);
  printf("@fin\n");
}

void flujoComando(const std::string& linea, uint8_t idSesion, uint32_t baudSesion)
{
  std::string cmd = linea.substr(0, linea.find(' '));
  if (cmd == "@hola") {
    printf("@ok ModbusEmpire flujo=1 max_regs=%u\n", (unsigned)FLUJO_MAX_REGS);
  } else if (cmd == "@leer") {
    leerContinuo(linea, idSesion, baudSesion);
  } else if (cmd == "@stop") {
    // Llega cuando no hay muestreo en curso: nada que detener.
  } else {
    printf("@error comando desconocido: %s\n", cmd.c_str());
  }
}
