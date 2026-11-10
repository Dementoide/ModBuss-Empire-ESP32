# Informe de verificación de sensores Modbus

Un solo informe que se edita cada vez que se prueba un sensor: qué se compró, si
coincide con su datasheet, qué registros tiene de verdad y cómo se calcula cada
valor.

| Sensor | Marca | Coincide con el datasheet | Estado |
|---|---|---|---|
| [RK330-01](#rk330-01--temperatura-y-humedad) | Rika | Sí, en lo comprado (T y H) | Verificado 2026-11-10 |
| [RK300-08](#rk300-08--multigás) | Rika | **No**: mapa distinto en los 3 documentos | Verificado 2026-11-09, con reclamo pendiente |
| RK300-02 | Rika | — | Pendiente |
| RK200-02 | Rika | — | Pendiente |

## Cómo se prueba y cómo leer las tablas

- **Banco:** ESP32 con el firmware de este repositorio. Lectura cruda con
  `@leer` (`include/flujo.h`) y la interfaz `gui/graficador.py`.
- **Direcciones:** base 0, en decimal y hex: `4 (0x0004)`. Es lo que se escribe
  en el campo de registro, no la notación 40001.
- **Tipos:**
  - **u16:** entero sin signo de 16 bits (0..65535).
  - **s16:** entero con signo en complemento a dos. Si el crudo es ≥ 32768, el
    valor es `crudo − 65536`. Por ejemplo, `0xFE70` = 65136 → −400.
- **Conversión:** `valor = crudo_con_signo × multiplicador + offset`. Es la
  misma fórmula que usa `data/sensores.json` (campos `mult` y `off`).
- **Cómo se sabe cada registro:**
  - **Confirmado con estímulo:** se provocó un cambio (aliento, fósforo,
    humidificador) y el registro respondió como corresponde.
  - **Datasheet + plausible:** el documento lo dice y el valor medido tiene
    sentido, pero no se provocó un cambio.
  - **Deducido:** el documento no lo dice; se dedujo de los valores.
  - **Sin identificar:** responde, pero no se sabe qué es.

---

## RK330-01 — Temperatura y humedad

**Compra:** factura R26101402, ítem 1: RK330-01A, 2 unidades. Se compró solo
**temperatura y humedad**, con escudo de radiación RK95-01. Rango −40–60 ℃ ±0,5 ℃
y 0–100 %RH ±3 %RH.

**Datasheet:** `Datasheets/RK330-01_T,H.pdf` ("Communication Protocol
(MODBUS-RTU)", 2 páginas).

**Unidad probada:** 1 de las 2, el 2026-11-10.

### Comunicación

| Parámetro | Valor | ¿Coincide con el datasheet? |
|---|---|---|
| ID | 1 | Sí |
| Serie | 9600 baud, 8N1 | Sí |
| Lectura | Función 03 (holding) | Sí. La función 04 no responde (no la indica). |
| Escritura | Función 06 | Según el datasheet; no probada. |
| Máximo por lectura | Hasta el registro 32 (0–32 en una trama) | El datasheet no lo indica. Leer 0–33, el 34 solo o bloques más arriba no da respuesta (timeout, no excepción). |
| Actualización | No medida | — |

### Registros

| Reg | Variable | Tipo | Conversión | Unidad | Ejemplo medido | Cómo se sabe |
|---|---|---|---|---|---|---|
| **0 (0x0000)** | **Temperatura** | **s16** | **÷ 10** | ℃ | 283 → **28,3 ℃** | Datasheet + plausible |
| **1 (0x0001)** | **Humedad relativa** | **u16** | **÷ 10** | %RH | 392 → **39,2 %RH** | Datasheet + plausible |
| 2 (0x0002) | Presión | u16 | ÷ 10 | hPa | 0 | Datasheet. Vale 0 porque no se compró. |
| 3 (0x0003) | Punto de rocío | s16 | ÷ 10 | ℃ | 130 → 13,0 ℃ | **Deducido**: el calculado con T y H (Magnus) da 13,1 / 12,9 / 13,0 contra 13,0 / 12,8 / 12,9 medido |
| 12 (0x000C) | Dirección del esclavo (lectura) | u16 | — | — | 1 | Deducido |
| 13 (0x000D) | Baudrate | u16 en hex "BCD" | — | — | `0x9600` = 9600 | Deducido. Usa la misma codificación del datasheet: `0x1200` = 1200, `0x4800` = 4800 |
| 16 / 17 | Rango de temperatura mín./máx. | s16 | ÷ 10 | ℃ | −400 / 600 → −40,0 / 60,0 | Deducido; coincide con la ficha (−40–60 ℃) |
| 18 / 19 | Rango de humedad mín./máx. | u16 | ÷ 10 | %RH | 0 / 1000 → 0 / 100,0 | Deducido |
| 20 / 21 | Rango de presión mín./máx. | u16 | ÷ 10 | hPa | 3000 / 11000 → 300,0 / 1100,0 | Deducido |
| 31 / 32 | ? | u16 | — | — | 128 / 129 | Sin identificar (constantes) |
| 4–11, 14, 15, 22–30 | — | — | — | — | 0 | — |

**Cómo calcular.** Por ejemplo, para una temperatura bajo cero:

- crudo de temperatura `0xFF9C` = 65436 → es s16 y ≥ 32768 → 65436 − 65536 = −100 → −100 ÷ 10 = **−10,0 ℃**
- crudo de humedad `0x0188` = 392 → u16 → 392 ÷ 10 = **39,2 %RH**

### Diferencias y erratas del datasheet

- **Temperatura negativa:** el ejemplo negativo divide por 100 (`−251/100 = −2.51 ℃`) y el
  positivo por 10. Lo correcto es **÷ 10**. Coincide con la resolución de 0,1 ℃ y con los
  registros de rango (−400 = −40,0 ℃).
- **Errata del ejemplo positivo:** escribe `(0121)H=(298)D`, pero 0x0121 = 289; el resultado,
  28,9 ℃, está bien.
- **Presión:** la tabla dice kPa y el ejemplo convierte a mbar (10024 → 1002,4). Por los
  registros de rango (300–1100), la unidad es **hPa (= mbar)**. No aplica a esta unidad
  porque no trae presión.
- **Cambio de ID:** el datasheet escribe la ID nueva con función 06 en el **registro 0**, y su
  respuesta no es el eco estándar de la 06. La opción "Cambiar ID" de ModbusEmpire espera el
  eco, así que marcaría error aunque el cambio haya funcionado. El cambio toma efecto después
  de reiniciar el sensor. **No probado.**
- **Códigos de baudrate:** la tabla de códigos no incluye 9600, aunque el sensor viene a 9600
  y el registro 13 lo informa como `0x9600`.

**Conclusión:** coincide con lo comprado. Temperatura (reg 0, s16 ÷ 10) y humedad (reg 1,
u16 ÷ 10) están donde dice el datasheet. Además trae el punto de rocío en el registro 3, que el
datasheet no menciona.

---

## RK300-08 — Multigás

**Compra:** factura R26101402, ítem 4: RK300-08, 1 unidad, con escudo de radiación. Se
compró con **CO₂** (0–5000 ppm), **SO₂** (0–10 ppm, resolución 0,001), **O₂**
(0–30 %VOL, resolución 0,1) y **PM1.0/2.5/10** (0–1000 µg/m³).

**Datasheets:** ninguno corresponde a la unidad.

- `Datasheets/RK300-08_CO2,SO2,O2,PM1,PM2.5,PM10.pdf`: lo mandó el proveedor.
- `Datasheets/RK300-08_proveedor-2_CO2,SO2,O2,PM1,PM2.5,PM10.pdf`: lo mandó el proveedor;
  es el mismo mapa con registros de configuración 40–42.
- `Datasheets/RK300-08_manual-v5.0-oficial_….pdf`: sitio de Rika, mismo mapa.
- `Datasheets/RK300-08_spec-v5.2-2025_….pdf`: ficha nueva, modular, sin tabla de registros.

Los tres mapas ponen PM2.5 en 0, CO₂ en 6, O₂ en 7, SO₂ en 8 y PM1.0 en 18. La unidad no
responde así. Solo coincide la dirección del esclavo en el registro 15.

**Unidad probada:** el 2026-11-09, con pruebas de aliento, fósforos y humidificador
ultrasónico.

### Comunicación

| Parámetro | Valor | ¿Coincide con el datasheet? |
|---|---|---|
| ID | 1 | Sí |
| Serie | 9600 baud, 8N1 | Sí |
| Lectura | Función 03 | Sí. La función 04 no responde. |
| Máximo por lectura | 64 registros sin problema (0–63 y 64–124 leídos) | El datasheet no lo indica |
| Actualización | **1 vez por segundo** en todas las variables (cambios cada 1000 ms exactos) | El datasheet no lo indica |
| Tiempo por muestra | 76 ms para 20 registros (55 ms de cable + ~21 ms de respuesta) | — |

### Registros

| Reg | Variable | Tipo | Conversión | Unidad | Reposo | Cómo se sabe |
|---|---|---|---|---|---|---|
| **4 (0x0004)** | **CO₂** | u16 | × 1 | ppm | 406–466 | **Confirmado con estímulo**: aliento → 3256; fósforos → 2315 |
| **8 (0x0008)** | **PM2.5** | u16 | × 1 | µg/m³ | 3 | **Confirmado con estímulo**: humo → 1003 |
| **9 (0x0009)** | **PM10** | u16 | × 1 | µg/m³ | 3 | **Confirmado con estímulo**: humo → 1913 |
| **10 (0x000A)** | **PM1.0** | u16 | × 1 | µg/m³ | 1 | **Confirmado con estímulo**: humo → 586 |
| **19 (0x0013)** | **O₂** | u16 | × 0,1 | %VOL | 208 → 20,8 % | **Confirmado con estímulo**: aliento → 19,4 % |
| 0 (0x0000) | ¿SO₂? | u16 | × 0,01 (supuesto) | ppm | 2 → 0,02 | **Sin identificar**: subió a 40 una vez con aliento; no reacciona a fósforos ni a humidificador |
| 15 (0x000F) | Dirección del esclavo | u16 | — | — | 1 | Datasheet (escritura con 06) + plausible |
| 16 (0x0010) | Baudrate (literal) | u16 | — | baud | 9600 | Deducido. El datasheet dice CH₄ |
| 30 | ? | u16 | — | — | 769 (`0x0301`) | Sin identificar (constante; ¿versión 3.1?) |
| 32 / 33 | ? | u16 | — | — | 16384 / 14916, luego 8960 / 4868 | Sin identificar: cambiaron entre dos lecturas separadas por minutos y quedaron fijos durante 7 min (¿calibración o contador interno?) |
| 14, 17, 18, 20–29 | — | — | — | — | 65535 (`0xFFFF`) | Probablemente "módulo no instalado"; el datasheet no lo explica |
| 1–3, 5–7, 11–13, 31, 34–124 | — | — | — | — | 0 siempre | — |

**Cómo calcular:**

- CO₂: crudo 412 → **412 ppm**
- O₂: crudo 208 → 208 × 0,1 = **20,8 %VOL**
- PM2.5: crudo 3 → **3 µg/m³**

### Lo que no coincide con la compra (para el reclamo)

1. **SO₂:** está en la factura, pero ningún registro responde a SO₂. Tres fósforos encendidos
   juntos al sensor subieron CO₂ y PM, y nada más. El registro 0 es el único candidato y no
   reaccionó.
2. **Humedad:** el proveedor dice que la unidad la tiene, pero ningún registro del 0 al 124
   se comporta como humedad. Una humedad tendría que subir con el aliento y bajar con la llama;
   ningún registro lo hace. Con el humidificador ultrasónico solo subieron los PM.
3. **Mapa de registros:** los tres documentos entregados describen un mapa genérico de 29
   variables que no corresponde a esta unidad. Hay que pedir el mapa de esta versión, dónde va
   el SO₂ y qué significa `0xFFFF`.

### Observaciones de uso

- **PM2.5 se satura cerca de 1000 µg/m³**, que es el tope del rango de compra. Sobre eso el
  valor no es confiable.
- **El agua cuenta como PM:** con el humidificador ultrasónico, PM10 llegó a 1914 µg/m³ por
  las gotitas. En terreno, la niebla o el rocío darán PM altos falsos.
- **Muestrear a más de 1 Hz solo repite valores.**
- **Reinicio espontáneo:** se vio una vez. Tras el reinicio los valores parten de 0 y el CO₂ y
  los PM suben en escalones; el O₂ da un primer valor absurdo, de unos 55000. En los 10 minutos
  de registro posteriores no se repitió (1208 muestras, 0 errores).

**Evidencia:**

- `informe/capturas/rk300-08_reposo_10min.csv`: 10 min, registros 0–19, cada 500 ms. Al
  final, tres fósforos.
- `informe/capturas/rk300-08_humedad_0-34.csv`: 7 min, registros 0–34, con aliento,
  fósforo y humidificador.
  - **Reloj:** el tiempo de la prueba (el que vio el operador) está corrido unos 2 min respecto
    al del registro.
  - **Humidificador:** en el tiempo de la prueba, de 2:20 a 4:20; en el registro, de ~4:30 a
    6:35.

**Conclusión:** de lo comprado, la unidad mide CO₂, O₂ y PM1.0/2.5/10, en registros distintos
a los del datasheet. **El SO₂ no aparece.** Queda en `sensores.json` con el mapa medido, y el
SO₂ en el registro 0 sin confirmar.
