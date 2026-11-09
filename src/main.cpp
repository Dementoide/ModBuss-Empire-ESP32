/**
 * MAIN.CPP
 * Consola de banco Modbus RTU: sensores del JSON, lectura manual de registros,
 * escaneo de ID/baudrate y diagnóstico del bus RS485.
 *
 * Todo corre en la main task (app_main no retorna). Las magnitudes se calculan
 * como crudo * M + O sobre el tipo de dato elegido (registros.h).
 */

// La suite Unity trae su propio app_main (test/).
#ifndef TEST_MODE

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <math.h>
#include <string>
#include <vector>

#include <driver/gpio.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cJSON.h>

#include "instrucciones.h"
#include "registros.h"
#include "modbus_error.h"
#include "modbus_rtu.h"
#include "consola.h"
#include "almacen.h"
#include "flujo.h"

#define RX_PIN 39
#define TX_PIN 33
#define RE_DE_PIN 32
#define RS485_PWR_PIN 13
#define SENSOR_PWR_PIN 12

static const bool LOGICA_RS485_INVERSA = true;
static const bool LOGICA_SENSOR_INVERSA = false;

static const char* RUTA_SENSORES = ALMACEN_BASE "/sensores.json";
static const size_t MAX_VARIABLES_TABLA = 32;
static const uint32_t BAUDRATES[] = {2400, 4800, 9600, 19200};
static const int NUM_BAUDRATES = sizeof(BAUDRATES) / sizeof(BAUDRATES[0]);

// Un registro elegido a mano (o cargado del JSON) que la sesión sabe leer.
struct Variable {
  std::string etiqueta;
  uint16_t reg;
  uint8_t fn;            // 3 = Holding (F03), 4 = Input (F04)
  TipoDato tipo;
  OrdenPalabras orden;
  double mult;
  double off;
  std::string uni;
};

// Estado de la sesión: con qué esclavo se habla y qué registros se leen.
static uint8_t g_id = 1;
static uint32_t g_baud = 9600;
static int g_regIdConfig = 512;     // registro donde el sensor guarda su ID (del JSON)
static std::string g_nombreSensor;
static std::vector<Variable> g_tabla;

static void esperarMs(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
static uint32_t segundosDesdeArranque() { return (uint32_t)(esp_timer_get_time() / 1000000); }

static void controlPin(int pin, bool inversa, bool estado)
{
  gpio_set_level((gpio_num_t)pin, inversa ? !estado : estado);
}

static void configurarSalida(int pin)
{
  gpio_reset_pin((gpio_num_t)pin);
  gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);
}

// ---------------------------------------------------------------------------
// Entrada por consola
// ---------------------------------------------------------------------------

// Entero decimal o hex ("0x0200"); Enter vacío o texto inválido = `defecto`.
static long leerEntero(long defecto)
{
  std::string s = leerLinea();
  long v;
  if (s.empty() || !parsearNumero(s.c_str(), &v)) return defecto;
  return v;
}

static double leerDouble(double defecto)
{
  std::string s = leerLinea();
  if (s.empty()) return defecto;
  for (char& c : s) if (c == ',') c = '.';
  char* fin = nullptr;
  double v = strtod(s.c_str(), &fin);
  return (fin == s.c_str()) ? defecto : v;
}

static std::string leerTexto(const std::string& defecto)
{
  std::string s = leerLinea();
  return s.empty() ? defecto : s;
}

static bool leerSiNo()
{
  std::string s = leerLinea();
  return s == "y" || s == "Y" || s == "s" || s == "S";
}

// ---------------------------------------------------------------------------
// Bus Modbus
// ---------------------------------------------------------------------------

static void configurarBus(uint32_t baud)
{
  if (!mbIniciar(baud, TX_PIN, RX_PIN, RE_DE_PIN)) {
    printf("ERROR: no se pudo abrir el UART del bus a %u baudios.\n", (unsigned)baud);
  }
}

static void imprimirError(uint8_t res)
{
  printf("   ERROR 0x%02X %s: %s\n", res, nombreErrorModbus(res), causaErrorModbus(res));
}

static uint8_t leerVariable(const Variable& v, double* valor, uint16_t* crudo)
{
  uint8_t res = mbLeer(g_id, v.fn, v.reg, palabrasDeTipo(v.tipo), crudo);
  if (res == MB_ERR_SUCCESS) {
    *valor = aplicarEscala(decodificar(crudo, v.tipo, v.orden), v.mult, v.off);
  }
  return res;
}

// Decimales que muestra una magnitud: los que aporta el multiplicador (0.01 -> 2).
static int decimalesDe(const Variable& v)
{
  if (v.tipo == T_F32) return 3;
  double m = fabs(v.mult);
  if (m == 0 || m >= 1.0) return (v.off == floor(v.off)) ? 0 : 2;
  int d = (int)ceil(-log10(m) - 1e-6);
  return d < 0 ? 0 : (d > 6 ? 6 : d);
}

// ---------------------------------------------------------------------------
// JSON <-> Variable (cJSON)
// ---------------------------------------------------------------------------

static const char* jsonTexto(const cJSON* o, const char* clave, const char* defecto)
{
  const cJSON* it = cJSON_GetObjectItemCaseSensitive(o, clave);
  return cJSON_IsString(it) ? it->valuestring : defecto;
}

static double jsonNumero(const cJSON* o, const char* clave, double defecto)
{
  const cJSON* it = cJSON_GetObjectItemCaseSensitive(o, clave);
  return cJSON_IsNumber(it) ? it->valuedouble : defecto;
}

// Compatible con el formato viejo: sin "tipo", "size":2 es u32 ABCD y 1 es u16.
static Variable variableDesdeJson(const cJSON* o)
{
  Variable v;
  v.etiqueta = jsonTexto(o, "etiqueta", "sin_nombre");
  v.reg = (uint16_t)jsonNumero(o, "reg", 0);
  v.fn = jsonNumero(o, "fn", 3) == 4 ? 4 : 3;
  v.tipo = jsonNumero(o, "size", 1) == 2 ? T_U32 : T_U16;
  parsearTipo(jsonTexto(o, "tipo", ""), &v.tipo);
  v.orden = ORDEN_ABCD;
  parsearOrden(jsonTexto(o, "orden", ""), &v.orden);
  v.mult = jsonNumero(o, "mult", 1.0);
  v.off = jsonNumero(o, "off", 0.0);
  v.uni = jsonTexto(o, "uni", "");
  return v;
}

static cJSON* variableAJson(const Variable& v)
{
  cJSON* o = cJSON_CreateObject();
  cJSON_AddStringToObject(o, "etiqueta", v.etiqueta.c_str());
  cJSON_AddNumberToObject(o, "reg", v.reg);
  cJSON_AddNumberToObject(o, "size", palabrasDeTipo(v.tipo));
  cJSON_AddNumberToObject(o, "fn", v.fn);
  cJSON_AddStringToObject(o, "tipo", nombreTipo(v.tipo));
  if (palabrasDeTipo(v.tipo) == 2) cJSON_AddStringToObject(o, "orden", nombreOrden(v.orden));
  cJSON_AddNumberToObject(o, "mult", v.mult);
  cJSON_AddNumberToObject(o, "off", v.off);
  cJSON_AddStringToObject(o, "uni", v.uni.c_str());
  return o;
}

// Arreglo de sensores del JSON (el llamador lo libera) o nullptr si no hay uno válido.
static cJSON* cargarSensores()
{
  std::string texto;
  if (!almacenLeer(RUTA_SENSORES, &texto)) return nullptr;
  cJSON* doc = cJSON_Parse(texto.c_str());
  if (doc == nullptr) {
    printf("sensores.json invalido (cerca de: %.20s)\n", cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() : "?");
    return nullptr;
  }
  if (!cJSON_IsArray(doc)) {
    printf("sensores.json no es un arreglo de sensores.\n");
    cJSON_Delete(doc);
    return nullptr;
  }
  return doc;
}

// ---------------------------------------------------------------------------
// Acciones de la sesión
// ---------------------------------------------------------------------------

static uint8_t pedirFuncion()
{
  printf("Funcion: 3 = Holding (F03) | 4 = Input (F04)  [Enter = 3]\n");
  return leerEntero(3) == 4 ? 4 : 3;
}

// Lee un bloque arbitrario y lo muestra en todas las interpretaciones útiles,
// para descubrir qué hay en un sensor desconocido.
static void lecturaRapida()
{
  uint8_t fn = pedirFuncion();
  printf("Direccion inicial (decimal o 0x hex):\n");
  long reg = leerEntero(-1);
  if (reg < 0 || reg > 0xFFFF) { printf("Direccion invalida.\n"); return; }
  printf("Largo: cantidad de registros (1-%u) [Enter = 1]:\n", MB_MAX_REGISTROS);
  long cant = leerEntero(1);
  if (cant < 1 || cant > MB_MAX_REGISTROS) { printf("Largo invalido.\n"); return; }
  if (reg + cant - 1 > 0xFFFF) { printf("El bloque se sale de 0xFFFF.\n"); return; }

  uint16_t buf[MB_MAX_REGISTROS];
  printf("\nF%02u  ID %u  desde %ld (0x%04lX)  largo %ld\n", fn, g_id, reg, reg, cant);
  uint8_t res = mbLeer(g_id, fn, (uint16_t)reg, (uint16_t)cant, buf);
  if (res != MB_ERR_SUCCESS) { imprimirError(res); return; }

  printf("  Reg (dec)  Reg (hex)     u16     s16    hex\n");
  for (long i = 0; i < cant; i++) {
    printf("  %9ld     0x%04lX  %6u  %6d  0x%04X\n",
           reg + i, reg + i, buf[i], (int16_t)buf[i], buf[i]);
  }
  if (cant >= 2) {
    printf("\n  Pares de 32 bits (desde el inicio, de a 2):\n");
    printf("  Regs           u32 ABCD     s32 ABCD     f32 ABCD |     u32 CDAB     f32 CDAB\n");
    for (long i = 0; i + 1 < cant; i += 2) {
      uint32_t ab = unir32(buf[i], buf[i + 1], ORDEN_ABCD);
      uint32_t cd = unir32(buf[i], buf[i + 1], ORDEN_CDAB);
      printf("  %5ld-%-5ld %12lu %12ld %12.4g | %12lu %12.4g\n",
             reg + i, reg + i + 1,
             (unsigned long)ab, (long)(int32_t)ab, (double)bitsAFloat(ab),
             (unsigned long)cd, (double)bitsAFloat(cd));
    }
  }
}

static void agregarVariable()
{
  if (g_tabla.size() >= MAX_VARIABLES_TABLA) {
    printf("La tabla esta llena (%u).\n", (unsigned)MAX_VARIABLES_TABLA);
    return;
  }
  Variable v;
  std::string porDefecto = "reg" + std::to_string(g_tabla.size());
  printf("Etiqueta [Enter = %s]:\n", porDefecto.c_str());
  v.etiqueta = leerTexto(porDefecto);
  v.fn = pedirFuncion();
  printf("Direccion del registro (decimal o 0x hex):\n");
  long reg = leerEntero(-1);
  if (reg < 0 || reg > 0xFFFF) { printf("Direccion invalida.\n"); return; }
  v.reg = (uint16_t)reg;

  printf("Tipo: u16 | s16 | u32 | s32 | f32  [Enter = u16]\n");
  printf("  (u16/s16 = 1 registro, u32/s32/f32 = 2 registros)\n");
  v.tipo = T_U16;
  std::string t = leerLinea();
  if (!t.empty() && !parsearTipo(t.c_str(), &v.tipo)) printf("Tipo no reconocido, uso u16.\n");

  v.orden = ORDEN_ABCD;
  if (palabrasDeTipo(v.tipo) == 2) {
    printf("Orden de palabras: ABCD (alta primero) | CDAB  [Enter = ABCD]\n");
    std::string o = leerLinea();
    if (!o.empty() && !parsearOrden(o.c_str(), &v.orden)) printf("Orden no reconocido, uso ABCD.\n");
  }
  printf("Multiplicador M (valor = crudo * M + O) [Enter = 1]:\n");
  v.mult = leerDouble(1.0);
  printf("Offset O [Enter = 0]:\n");
  v.off = leerDouble(0.0);
  printf("Unidad [Enter = ninguna]:\n");
  v.uni = leerTexto("");

  g_tabla.push_back(v);
  printf("Agregado #%u: %s  F%02u reg %u (0x%04X) %s\n", (unsigned)g_tabla.size() - 1,
         v.etiqueta.c_str(), v.fn, v.reg, v.reg, nombreTipo(v.tipo));
}

static void mostrarTabla()
{
  if (g_tabla.empty()) { printf("La tabla esta vacia (opcion 2 para agregar).\n"); return; }
  printf("\n  #  Etiqueta          Fn   Reg   (hex)  Largo Tipo Orden     Mult      Off  Unidad\n");
  for (size_t i = 0; i < g_tabla.size(); i++) {
    const Variable& v = g_tabla[i];
    printf(" %2u  %-16s F%02u %5u 0x%04X  %5u %-4s %-5s %8g %8g  %s\n",
           (unsigned)i, v.etiqueta.c_str(), v.fn, v.reg, v.reg, palabrasDeTipo(v.tipo),
           nombreTipo(v.tipo), palabrasDeTipo(v.tipo) == 2 ? nombreOrden(v.orden) : "-",
           v.mult, v.off, v.uni.c_str());
  }
}

static void quitarVariable()
{
  mostrarTabla();
  if (g_tabla.empty()) return;
  printf("Numero a quitar (Enter = cancelar, 'todo' = vaciar):\n");
  std::string s = leerLinea();
  if (strcasecmp(s.c_str(), "todo") == 0) { g_tabla.clear(); printf("Tabla vaciada.\n"); return; }
  long i;
  if (!parsearNumero(s.c_str(), &i) || i < 0 || i >= (long)g_tabla.size()) return;
  printf("Quitado: %s\n", g_tabla[i].etiqueta.c_str());
  g_tabla.erase(g_tabla.begin() + i);
}

static void leerTabla(bool compacto)
{
  if (g_tabla.empty()) { printf("La tabla esta vacia.\n"); return; }
  if (!compacto) printf("\n--- LECTURA  ID %u @ %u ---\n", g_id, (unsigned)g_baud);
  std::string linea;
  for (const Variable& v : g_tabla) {
    double valor = 0;
    uint16_t crudo[2] = {0, 0};
    uint8_t res = leerVariable(v, &valor, crudo);
    char b[96];
    if (compacto) {
      if (res == MB_ERR_SUCCESS) {
        snprintf(b, sizeof(b), "%s=%.*f%s  ", v.etiqueta.c_str(), decimalesDe(v), valor, v.uni.c_str());
      } else {
        snprintf(b, sizeof(b), "%s=ERR(%02X)  ", v.etiqueta.c_str(), res);
      }
      linea += b;
    } else if (res == MB_ERR_SUCCESS) {
      if (palabrasDeTipo(v.tipo) == 2) {
        printf(">> %-16s %.*f %s   [crudo 0x%04X 0x%04X]\n", v.etiqueta.c_str(),
               decimalesDe(v), valor, v.uni.c_str(), crudo[0], crudo[1]);
      } else {
        printf(">> %-16s %.*f %s   [crudo %u / 0x%04X]\n", v.etiqueta.c_str(),
               decimalesDe(v), valor, v.uni.c_str(), crudo[0], crudo[0]);
      }
    } else {
      printf(">> %-16s", v.etiqueta.c_str());
      imprimirError(res);
    }
    esperarMs(100);   // silencio entre tramas: varios sensores lo necesitan
  }
  if (compacto) printf("[%u s] %s\n", (unsigned)segundosDesdeArranque(), linea.c_str());
}

static void cicloTabla()
{
  if (g_tabla.empty()) { printf("La tabla esta vacia.\n"); return; }
  printf("Periodo en segundos [Enter = 2]:\n");
  long periodo = leerEntero(2);
  if (periodo < 1) periodo = 1;
  printf("Leyendo en ciclo. Enter (cualquier tecla) para detener.\n");
  vaciarEntrada();
  while (true) {
    leerTabla(true);
    int64_t fin = esp_timer_get_time() + (int64_t)periodo * 1000000;
    while (esp_timer_get_time() < fin) {
      if (hayEntrada()) { vaciarEntrada(); printf("Ciclo detenido.\n"); return; }
      esperarMs(20);
    }
  }
}

static void guardarTabla()
{
  if (g_tabla.empty()) { printf("La tabla esta vacia, nada que guardar.\n"); return; }
  std::string porDefecto = g_nombreSensor.empty() ? "Sensor manual" : g_nombreSensor;
  printf("Nombre del sensor [Enter = %s]:\n", porDefecto.c_str());
  std::string nombre = leerTexto(porDefecto);
  printf("Registro de configuracion de ID [Enter = %d]:\n", g_regIdConfig);
  int regId = (int)leerEntero(g_regIdConfig);

  cJSON* doc = cargarSensores();
  if (doc == nullptr) doc = cJSON_CreateArray();

  cJSON* s = cJSON_CreateObject();
  cJSON_AddStringToObject(s, "nombre", nombre.c_str());
  cJSON_AddNumberToObject(s, "reg_id_config", regId);
  cJSON* vars = cJSON_AddArrayToObject(s, "variables");
  for (const Variable& v : g_tabla) cJSON_AddItemToArray(vars, variableAJson(v));

  // Mismo nombre = se reemplaza; si no, se agrega al final.
  int reemplazar = -1;
  for (int i = 0; i < cJSON_GetArraySize(doc); i++) {
    if (strcasecmp(jsonTexto(cJSON_GetArrayItem(doc, i), "nombre", ""), nombre.c_str()) == 0) {
      reemplazar = i;
      break;
    }
  }
  if (reemplazar >= 0) {
    cJSON_ReplaceItemInArray(doc, reemplazar, s);
    printf("Ya existia un sensor con ese nombre: se reemplaza.\n");
  } else {
    cJSON_AddItemToArray(doc, s);
  }

  char* texto = cJSON_Print(doc);
  cJSON_Delete(doc);
  if (texto == nullptr) { printf("ERROR: sin memoria para armar el JSON, no se guardo.\n"); return; }
  bool ok = almacenEscribir(RUTA_SENSORES, std::string(texto) + "\n");
  cJSON_free(texto);
  if (!ok) { printf("ERROR: no se pudo escribir %s.\n", RUTA_SENSORES); return; }
  g_nombreSensor = nombre;
  g_regIdConfig = regId;
  printf("Guardado '%s' con %u registros en %s.\n", nombre.c_str(), (unsigned)g_tabla.size(), RUTA_SENSORES);
}

static void escribirRegistro()
{
  printf("Direccion del registro a escribir (decimal o 0x hex):\n");
  long reg = leerEntero(-1);
  if (reg < 0 || reg > 0xFFFF) { printf("Direccion invalida.\n"); return; }
  printf("Valor a escribir (0-65535, o 0x hex; negativos como s16):\n");
  long valor = leerEntero(-100000);
  if (valor < -32768 || valor > 65535) { printf("Valor invalido.\n"); return; }

  printf("Escribiendo %ld en el registro %ld (0x%04lX)...\n", valor, reg, reg);
  uint8_t res = mbEscribirSimple(g_id, (uint16_t)reg, (uint16_t)valor);
  if (res == MB_ERR_SUCCESS) printf("Escritura EXITOSA.\n");
  else imprimirError(res);
}

static void cambiarIdSensor()
{
  printf("Registro donde el sensor guarda su ID [Enter = %d (0x%04X)]:\n", g_regIdConfig, g_regIdConfig);
  long reg = leerEntero(g_regIdConfig);
  if (reg < 0 || reg > 0xFFFF) { printf("Direccion invalida.\n"); return; }
  printf("Ingrese la NUEVA ID deseada (1-247):\n");
  long nueva = leerEntero(0);
  if (nueva < 1 || nueva > 247) { printf("ID invalida.\n"); return; }

  printf("Intentando cambiar ID en registro %ld (0x%04lX)...\n", reg, reg);
  uint8_t res = mbEscribirSimple(g_id, (uint16_t)reg, (uint16_t)nueva);
  if (res == MB_ERR_SUCCESS) {
    printf("EXITO: ID cambiada a %ld. La sesion pasa a usar la ID nueva.\n", nueva);
    printf("(Algunos sensores la aplican recien al reiniciarlos.)\n");
    g_id = (uint8_t)nueva;
  } else {
    imprimirError(res);
  }
}

// Pide ID y baudrate; Enter conserva los de la sesión (p. ej. los del escaneo).
static void pedirIdYBaud()
{
  printf("Ingrese ID del esclavo (actual) [Enter = %u]:\n", g_id);
  long id = leerEntero(g_id);
  if (id >= 0 && id <= 247) g_id = (uint8_t)id;

  int actual = 3;
  for (int i = 0; i < NUM_BAUDRATES; i++) if (BAUDRATES[i] == g_baud) actual = i + 1;
  printf("Seleccione Baudrate (1:2400, 2:4800, 3:9600, 4:19200) [Enter = %d]:\n", actual);
  long sel = leerEntero(actual);
  if (sel < 1) sel = 1;
  if (sel > NUM_BAUDRATES) sel = NUM_BAUDRATES;
  g_baud = BAUDRATES[sel - 1];
  configurarBus(g_baud);
}

static void sesion()
{
  while (true) {
    printf("\n--- SESION  ID %u @ %u  |  %s  |  tabla: %u registros ---\n", g_id, (unsigned)g_baud,
           g_nombreSensor.empty() ? "manual" : g_nombreSensor.c_str(), (unsigned)g_tabla.size());
    printf(" 1: Lectura rapida (direccion + largo, crudo)\n");
    printf(" 2: Agregar registro a la tabla\n");
    printf(" 3: Ver tabla\n");
    printf(" 4: Quitar registro de la tabla\n");
    printf(" 5: Leer tabla una vez\n");
    printf(" 6: Leer tabla en ciclo\n");
    printf(" 7: Guardar tabla en sensores.json\n");
    printf(" 8: Escribir registro (F06)\n");
    printf(" 9: Cambiar ID del sensor\n");
    printf("10: Cambiar ID/baudrate de la sesion\n");
    printf(" 0: Volver al menu principal\n");
    switch (leerEntero(-1)) {
      case 1: lecturaRapida(); break;
      case 2: agregarVariable(); break;
      case 3: mostrarTabla(); break;
      case 4: quitarVariable(); break;
      case 5: leerTabla(false); break;
      case 6: cicloTabla(); break;
      case 7: guardarTabla(); break;
      case 8: escribirRegistro(); break;
      case 9: cambiarIdSensor(); break;
      case 10: pedirIdYBaud(); break;
      case 0: return;
      default: printf("Opcion invalida.\n"); break;
    }
  }
}

// Carga un sensor del JSON a la tabla de la sesión. false si no hay o se canceló.
static bool elegirSensorGuardado()
{
  cJSON* doc = cargarSensores();
  if (doc == nullptr) {
    printf("No hay sensores.json valido (subirlo con 'pio run -t uploadfs').\n");
    return false;
  }
  const int n = cJSON_GetArraySize(doc);
  if (n == 0) { printf("sensores.json esta vacio.\n"); cJSON_Delete(doc); return false; }
  printf("\nSeleccione un sensor guardado:\n");
  for (int i = 0; i < n; i++) {
    const cJSON* s = cJSON_GetArrayItem(doc, i);
    printf("%d: %s (%d registros)\n", i, jsonTexto(s, "nombre", "?"),
           cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(s, "variables")));
  }
  long idx = leerEntero(-1);
  if (idx < 0 || idx >= n) { printf("Indice invalido.\n"); cJSON_Delete(doc); return false; }

  const cJSON* s = cJSON_GetArrayItem(doc, (int)idx);
  g_tabla.clear();
  const cJSON* o;
  cJSON_ArrayForEach(o, cJSON_GetObjectItemCaseSensitive(s, "variables")) {
    if (g_tabla.size() >= MAX_VARIABLES_TABLA) break;
    g_tabla.push_back(variableDesdeJson(o));
  }
  g_nombreSensor = jsonTexto(s, "nombre", "");
  g_regIdConfig = (int)jsonNumero(s, "reg_id_config", 512);
  cJSON_Delete(doc);
  printf("Cargado '%s' a la tabla (se puede editar y volver a guardar).\n", g_nombreSensor.c_str());
  mostrarTabla();
  return true;
}

// ---------------------------------------------------------------------------
// Escaneo y diagnóstico
// ---------------------------------------------------------------------------

static void escanearSensor()
{
  printf("ID maxima a barrer (1-247) [Enter = 3]  (~1 s por ID y baudrate sin respuesta):\n");
  long idMax = leerEntero(3);
  if (idMax < 1) idMax = 1;
  if (idMax > 247) idMax = 247;
  printf("\n--- ESCANEO (BROADCASTS + BARRIDO ID 1-%ld)  Enter para abortar ---\n", idMax);
  vaciarEntrada();

  uint16_t r;
  for (int b = 0; b < NUM_BAUDRATES; b++) {
    printf("\n[PROBANDO %u BAUDIOS]\n", (unsigned)BAUDRATES[b]);
    configurarBus(BAUDRATES[b]);

    // Broadcasts: solo informan y siguen.
    const uint8_t broads[] = {0xFE, 0x00};
    for (uint8_t dir : broads) {
      uint8_t res = mbLeer(dir, 3, 0x0000, 1, &r);
      if (res != MB_ERR_RESPONSE_TIMED_OUT) {
        printf("  > Actividad en broadcast 0x%02X (%s)\n", dir, nombreErrorModbus(res));
      }
    }

    for (long id = 1; id <= idMax; id++) {
      if (hayEntrada()) { vaciarEntrada(); printf("Escaneo abortado.\n"); configurarBus(g_baud); return; }
      printf("  Verificando ID %ld... ", id);
      uint8_t res = mbLeer((uint8_t)id, 3, 0x0000, 1, &r);
      if (res == MB_ERR_SUCCESS || mbEsExcepcion(res)) {
        printf("ENCONTRADO!\n\n");
        printf("*********************************\n");
        printf("  SENSOR DETECTADO CON EXITO\n");
        printf("  BAUDRATE: %u\n", (unsigned)BAUDRATES[b]);
        printf("  ID REAL:  %ld\n", id);
        printf("*********************************\n");
        printf("(La sesion queda con esta ID y baudrate.)\n");
        g_id = (uint8_t)id;
        g_baud = BAUDRATES[b];
        return;
      }
      printf("No (%s)\n", nombreErrorModbus(res));
    }
  }
  printf("\nEscaneo finalizado sin detectar una ID entre 1 y %ld.\n", idMax);
  configurarBus(g_baud);
}

// Con el riel encendido y el bus en reposo, el RO del transceptor deja RX en
// marca (HIGH); en LOW no hay transceptor alimentado en ese pin (node_minkai,
// modbusProbeBus). Muestras repetidas: si cambia, hay tráfico en el bus.
static void diagnosticoBus()
{
  configurarBus(g_baud);
  esperarMs(50);
  int altos = 0;
  const int muestras = 50;
  for (int i = 0; i < muestras; i++) {
    altos += mbNivelRx() == 1;
    esperarMs(2);
  }
  printf("\nRX=%d: %d/%d muestras en ALTO\n", RX_PIN, altos, muestras);
  if (altos == muestras) {
    printf("OK: transceptor RS485 presente y alimentado (linea en marca).\n");
  } else if (altos == 0) {
    printf("FALLA: RX en BAJO - no hay transceptor alimentado en ese pin.\n");
    printf("Revisar RS485_PWR_PIN=%d (logica inversa=%s), cableado y pines.\n",
           RS485_PWR_PIN, LOGICA_RS485_INVERSA ? "si" : "no");
  } else {
    printf("AVISO: RX cambia en reposo - hay trafico en el bus u otra fuente de ruido.\n");
  }
}

// Líneas '@' de la interfaz gráfica: muestrean con la ID/baud de la sesión por
// defecto y devuelven el bus a su baudrate.
static void atenderComando(const std::string& linea)
{
  flujoComando(linea, g_id, g_baud);
}

// ---------------------------------------------------------------------------

extern "C" void app_main()
{
  consolaIniciar();
  consolaFijarManejador(atenderComando);
  configurarSalida(RS485_PWR_PIN);
  configurarSalida(SENSOR_PWR_PIN);
  controlPin(RS485_PWR_PIN, LOGICA_RS485_INVERSA, true);
  controlPin(SENSOR_PWR_PIN, LOGICA_SENSOR_INVERSA, true);
  if (!almacenMontar()) printf("Error LittleFS\n");
  configurarBus(g_baud);

  printf("\nSistema Iniciado.\n");
  printf("\n----------------------------------\n");
  printf("¿Desea ver las instrucciones? (Y/N)\n");
  if (leerSiNo()) printf("%s", TEXTO_INSTRUCCIONES);

  while (true) {
    printf("\n--- MENU PRINCIPAL ---\n");
    printf("1: Usar Sensor Conocido (JSON)\n");
    printf("2: Modo Manual (elegir registros a mano)\n");
    printf("3: ESCANEAR Sensor (Auto-deteccion)\n");
    printf("4: Diagnostico del bus RS485\n");

    switch (leerEntero(-1)) {
      case 1:
        pedirIdYBaud();
        if (elegirSensorGuardado()) sesion();
        break;
      case 2:
        pedirIdYBaud();
        if (!g_tabla.empty()) {
          printf("La tabla tiene %u registros de antes. ¿Vaciarla? (Y/N)\n", (unsigned)g_tabla.size());
          if (leerSiNo()) { g_tabla.clear(); g_nombreSensor.clear(); }
        }
        sesion();
        break;
      case 3: escanearSensor(); break;
      case 4: diagnosticoBus(); break;
      default: printf("Opcion invalida.\n"); break;
    }
  }
}

#endif  // TEST_MODE
