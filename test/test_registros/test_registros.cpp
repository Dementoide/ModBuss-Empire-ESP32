/**
 * TEST_REGISTROS.CPP
 * Decodificación de registros, parseo de entrada y tramas RTU del maestro propio.
 *
 * Corre en la placa: pio test -e test
 */
#include <string.h>
#include <unity.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "registros.h"
#include "modbus_error.h"
#include "modbus_rtu.h"
#include "lista_registros.h"

void setUp() {}
void tearDown() {}

// --- Decodificación -------------------------------------------------------

void test_s16_negativo() {
  uint16_t r[] = {0xFFFB};
  TEST_ASSERT_EQUAL_FLOAT(-5.0f, (float)decodificar(r, T_S16, ORDEN_ABCD));
  TEST_ASSERT_EQUAL_FLOAT(65531.0f, (float)decodificar(r, T_U16, ORDEN_ABCD));
}

void test_u32_orden_palabras() {
  uint16_t r[] = {0x0001, 0x0002};
  TEST_ASSERT_EQUAL_UINT32(0x00010002, unir32(r[0], r[1], ORDEN_ABCD));
  TEST_ASSERT_EQUAL_UINT32(0x00020001, unir32(r[0], r[1], ORDEN_CDAB));
  TEST_ASSERT_EQUAL_FLOAT(65538.0f, (float)decodificar(r, T_U32, ORDEN_ABCD));
}

void test_s32_negativo() {
  uint16_t r[] = {0xFFFF, 0xFFFE};
  TEST_ASSERT_EQUAL_FLOAT(-2.0f, (float)decodificar(r, T_S32, ORDEN_ABCD));
}

void test_f32_ieee() {
  uint16_t abcd[] = {0x41C8, 0x0000};  // 25.0f
  uint16_t cdab[] = {0x0000, 0x41C8};
  TEST_ASSERT_EQUAL_FLOAT(25.0f, (float)decodificar(abcd, T_F32, ORDEN_ABCD));
  TEST_ASSERT_EQUAL_FLOAT(25.0f, (float)decodificar(cdab, T_F32, ORDEN_CDAB));
}

void test_escala() {
  uint16_t r[] = {235};
  TEST_ASSERT_FLOAT_WITHIN(1e-4, 23.5f, (float)aplicarEscala(decodificar(r, T_U16, ORDEN_ABCD), 0.1, 0.0));
  TEST_ASSERT_FLOAT_WITHIN(1e-4, 13.5f, (float)aplicarEscala(decodificar(r, T_U16, ORDEN_ABCD), 0.1, -10.0));
}

void test_palabras_de_tipo() {
  TEST_ASSERT_EQUAL_UINT8(1, palabrasDeTipo(T_U16));
  TEST_ASSERT_EQUAL_UINT8(1, palabrasDeTipo(T_S16));
  TEST_ASSERT_EQUAL_UINT8(2, palabrasDeTipo(T_U32));
  TEST_ASSERT_EQUAL_UINT8(2, palabrasDeTipo(T_F32));
}

// --- Entrada --------------------------------------------------------------

void test_parsear_numero() {
  long v;
  TEST_ASSERT_TRUE(parsearNumero("512", &v));    TEST_ASSERT_EQUAL(512, v);
  TEST_ASSERT_TRUE(parsearNumero("0x0200", &v)); TEST_ASSERT_EQUAL(512, v);
  TEST_ASSERT_TRUE(parsearNumero("010", &v));    TEST_ASSERT_EQUAL(10, v);  // no octal
  TEST_ASSERT_TRUE(parsearNumero("-3", &v));     TEST_ASSERT_EQUAL(-3, v);
  TEST_ASSERT_FALSE(parsearNumero("", &v));
  TEST_ASSERT_FALSE(parsearNumero("0x", &v));
  TEST_ASSERT_FALSE(parsearNumero("12a", &v));
}

void test_parsear_tipo_y_orden() {
  TipoDato t;
  OrdenPalabras o;
  TEST_ASSERT_TRUE(parsearTipo("F32", &t));   TEST_ASSERT_EQUAL(T_F32, t);
  TEST_ASSERT_TRUE(parsearTipo("float", &t)); TEST_ASSERT_EQUAL(T_F32, t);
  TEST_ASSERT_TRUE(parsearTipo("s16", &t));   TEST_ASSERT_EQUAL(T_S16, t);
  TEST_ASSERT_FALSE(parsearTipo("u8", &t));
  TEST_ASSERT_TRUE(parsearOrden("cdab", &o)); TEST_ASSERT_EQUAL(ORDEN_CDAB, o);
  TEST_ASSERT_FALSE(parsearOrden("BADC", &o));
}

void test_nombres_error() {
  TEST_ASSERT_EQUAL_STRING("RESPONSE_TIMED_OUT", nombreErrorModbus(MB_ERR_RESPONSE_TIMED_OUT));
  TEST_ASSERT_EQUAL_STRING("INVALID_SLAVE_ID", nombreErrorModbus(MB_ERR_INVALID_SLAVE_ID));
  TEST_ASSERT_EQUAL_STRING("UNKNOWN", nombreErrorModbus(0x77));
  TEST_ASSERT_TRUE(mbEsExcepcion(MB_ERR_ILLEGAL_DATA_ADDRESS));
  TEST_ASSERT_FALSE(mbEsExcepcion(MB_ERR_INVALID_CRC));
}

// --- Tramas RTU -----------------------------------------------------------

static size_t conCrc(uint8_t* trama, size_t largo) {
  uint16_t crc = mbCrc16(trama, largo);
  trama[largo] = crc & 0xFF;
  trama[largo + 1] = crc >> 8;
  return largo + 2;
}

// Vector de la especificación: leer 10 registros desde 0 del esclavo 1.
void test_rtu_arma_lectura_referencia() {
  const uint8_t esperado[] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x0A, 0xC5, 0xCD};
  uint8_t trama[8];
  TEST_ASSERT_EQUAL_UINT(8, mbArmarLectura(1, 3, 0, 10, trama));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(esperado, trama, 8);
}

void test_rtu_arma_lectura_input() {
  uint8_t trama[8];
  mbArmarLectura(2, 4, 0x0102, 3, trama);
  TEST_ASSERT_EQUAL_UINT8(0x04, trama[1]);
  TEST_ASSERT_EQUAL_UINT8(0x01, trama[2]);
  TEST_ASSERT_EQUAL_UINT8(0x02, trama[3]);
  TEST_ASSERT_EQUAL_UINT16(mbCrc16(trama, 6), trama[6] | (trama[7] << 8));
}

void test_rtu_parsea_lectura_valida() {
  uint8_t rsp[16] = {0x01, 0x03, 0x04, 0x01, 0x23, 0xFF, 0xFB};
  size_t n = conCrc(rsp, 7);
  uint16_t out[2] = {0, 0};
  TEST_ASSERT_EQUAL_UINT8(MB_ERR_SUCCESS, mbParsearLectura(rsp, n, 1, 3, 2, out));
  TEST_ASSERT_EQUAL_HEX16(0x0123, out[0]);
  TEST_ASSERT_EQUAL_HEX16(0xFFFB, out[1]);
}

void test_rtu_detecta_errores_de_lectura() {
  uint8_t rsp[16] = {0x01, 0x03, 0x04, 0x01, 0x23, 0xFF, 0xFB};
  size_t n = conCrc(rsp, 7);
  uint16_t out[2];
  TEST_ASSERT_EQUAL_UINT8(MB_ERR_INVALID_SLAVE_ID, mbParsearLectura(rsp, n, 2, 3, 2, out));
  TEST_ASSERT_EQUAL_UINT8(MB_ERR_INVALID_FUNCTION, mbParsearLectura(rsp, n, 1, 4, 2, out));
  TEST_ASSERT_EQUAL_UINT8(MB_ERR_RESPONSE_TIMED_OUT, mbParsearLectura(rsp, 2, 1, 3, 2, out));
  rsp[n - 1] ^= 0xFF;
  TEST_ASSERT_EQUAL_UINT8(MB_ERR_INVALID_CRC, mbParsearLectura(rsp, n, 1, 3, 2, out));
}

void test_rtu_parsea_excepcion() {
  uint8_t rsp[8] = {0x01, 0x83, 0x02};
  size_t n = conCrc(rsp, 3);
  uint16_t out[1];
  TEST_ASSERT_EQUAL_UINT8(MB_ERR_ILLEGAL_DATA_ADDRESS, mbParsearLectura(rsp, n, 1, 3, 1, out));
}

void test_rtu_escritura_simple_eco() {
  uint8_t req[8];
  TEST_ASSERT_EQUAL_UINT(8, mbArmarEscrituraSimple(1, 0x0200, 5, req));
  TEST_ASSERT_EQUAL_UINT8(0x06, req[1]);
  TEST_ASSERT_EQUAL_UINT8(MB_ERR_SUCCESS, mbParsearEscrituraSimple(req, 8, req));

  uint8_t otro[8];
  mbArmarEscrituraSimple(1, 0x0200, 6, otro);
  TEST_ASSERT_EQUAL_UINT8(MB_ERR_INVALID_FUNCTION, mbParsearEscrituraSimple(otro, 8, req));

  uint8_t exc[8] = {0x01, 0x86, 0x02};
  size_t n = conCrc(exc, 3);
  TEST_ASSERT_EQUAL_UINT8(MB_ERR_ILLEGAL_DATA_ADDRESS, mbParsearEscrituraSimple(exc, n, req));
}

// --- Lista de registros y bloques ----------------------------------------

void test_lista_registros_mixta() {
  std::vector<uint16_t> r;
  TEST_ASSERT_TRUE(parsearListaRegistros("0,4,8-10,0x13", &r, 64));
  const uint16_t esperado[] = {0, 4, 8, 9, 10, 19};
  TEST_ASSERT_EQUAL_UINT(6, r.size());
  TEST_ASSERT_EQUAL_UINT16_ARRAY(esperado, r.data(), 6);
}

void test_lista_registros_orden_y_repetidos() {
  std::vector<uint16_t> r;
  TEST_ASSERT_TRUE(parsearListaRegistros("19, 4 ,4,2-3", &r, 64));
  const uint16_t esperado[] = {19, 4, 2, 3};
  TEST_ASSERT_EQUAL_UINT(4, r.size());
  TEST_ASSERT_EQUAL_UINT16_ARRAY(esperado, r.data(), 4);
}

void test_lista_registros_invalida() {
  std::vector<uint16_t> r;
  TEST_ASSERT_FALSE(parsearListaRegistros("", &r, 64));
  TEST_ASSERT_FALSE(parsearListaRegistros("5-2", &r, 64));
  TEST_ASSERT_FALSE(parsearListaRegistros("1,,2", &r, 64));
  TEST_ASSERT_FALSE(parsearListaRegistros("70000", &r, 64));
  TEST_ASSERT_FALSE(parsearListaRegistros("0-64", &r, 64));   // 65 registros
  TEST_ASSERT_FALSE(parsearListaRegistros("3a", &r, 64));
}

void test_agrupar_bloques_con_hueco() {
  std::vector<uint16_t> r = {19, 0, 4, 8, 9, 10};
  std::vector<BloqueLectura> b;
  agruparBloques(r, 16, 125, &b);
  TEST_ASSERT_EQUAL_UINT(1, b.size());
  TEST_ASSERT_EQUAL_UINT16(0, b[0].inicio);
  TEST_ASSERT_EQUAL_UINT16(20, b[0].cant);
  agruparBloques(r, 0, 125, &b);
  TEST_ASSERT_EQUAL_UINT(4, b.size());   // 0 | 4 | 8-10 | 19
  TEST_ASSERT_EQUAL_UINT16(8, b[2].inicio);
  TEST_ASSERT_EQUAL_UINT16(3, b[2].cant);
}

void test_agrupar_bloques_respeta_tope() {
  std::vector<uint16_t> r = {0, 200};
  std::vector<BloqueLectura> b;
  agruparBloques(r, 300, 125, &b);
  TEST_ASSERT_EQUAL_UINT(2, b.size());
}

void test_tiempos_de_trama() {
  // 20 registros a 9600: (13 + 40) bytes * 10 bits / 9600 = 55,2 ms.
  TEST_ASSERT_FLOAT_WITHIN(0.1, 55.2, msTramaLectura(20, 9600));
  TEST_ASSERT_EQUAL_UINT32(5, msSilencioEntreTramas(9600));
  TEST_ASSERT_EQUAL_UINT32(16, msSilencioEntreTramas(2400));
}

extern "C" void app_main() {
  vTaskDelay(pdMS_TO_TICKS(2000));
  UNITY_BEGIN();
  RUN_TEST(test_s16_negativo);
  RUN_TEST(test_u32_orden_palabras);
  RUN_TEST(test_s32_negativo);
  RUN_TEST(test_f32_ieee);
  RUN_TEST(test_escala);
  RUN_TEST(test_palabras_de_tipo);
  RUN_TEST(test_parsear_numero);
  RUN_TEST(test_parsear_tipo_y_orden);
  RUN_TEST(test_nombres_error);
  RUN_TEST(test_rtu_arma_lectura_referencia);
  RUN_TEST(test_rtu_arma_lectura_input);
  RUN_TEST(test_rtu_parsea_lectura_valida);
  RUN_TEST(test_rtu_detecta_errores_de_lectura);
  RUN_TEST(test_rtu_parsea_excepcion);
  RUN_TEST(test_rtu_escritura_simple_eco);
  RUN_TEST(test_lista_registros_mixta);
  RUN_TEST(test_lista_registros_orden_y_repetidos);
  RUN_TEST(test_lista_registros_invalida);
  RUN_TEST(test_agrupar_bloques_con_hueco);
  RUN_TEST(test_agrupar_bloques_respeta_tope);
  RUN_TEST(test_tiempos_de_trama);
  UNITY_END();
}
