#include <Arduino.h>
#include <ModbusMaster.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "instrucciones.h"

#define RX_PIN 39
#define TX_PIN 33
#define RE_DE_PIN 32
#define RS485_PWR_PIN 13
#define SENSOR_PWR_PIN 12

const bool LOGICA_RS485_INVERSA = true; 
const bool LOGICA_SENSOR_INVERSA = false; 

ModbusMaster node;
long baudrates[] = {2400, 4800, 9600, 19200};

void preTransmission() { digitalWrite(RE_DE_PIN, HIGH); }
void postTransmission() { digitalWrite(RE_DE_PIN, LOW); }

void controlPin(int pin, bool inversa, bool estado) {
  digitalWrite(pin, inversa ? (estado ? LOW : HIGH) : (estado ? HIGH : LOW));
}

String leerConsola() {
  while (!Serial.available()) { delay(10); }
  String input = Serial.readStringUntil('\n');
  input.trim();
  Serial.println("> Recibido: " + input);
  return input;
}

void escanearSensor() {
  Serial.println("\n--- ESCANEO ESTRATEGICO (BROADCASTS + BARRIDO ID 1-3) ---");
  
  for (int b = 0; b < 4; b++) {
    Serial.printf("\n[PROBANDO %ld BAUDIOS]\n", baudrates[b]);
    
    Serial1.end();
    delay(50);
    Serial1.begin(baudrates[b], SERIAL_8N1, RX_PIN, TX_PIN);

    // --- PARTE 1: BROADCASTS (Solo informan y continúan) ---
    uint8_t broads[] = {0xFE, 0x00};
    for (int i = 0; i < 2; i++) {
      node.begin(broads[i], Serial1);
      node.preTransmission(preTransmission);
      node.postTransmission(postTransmission);
      node.clearResponseBuffer();

      // Intentamos leer registro 0 para ver si alguien "pía"
      uint8_t res = node.readHoldingRegisters(0x0000, 1);
      if (res != 0xE2) { // 0xE2 es Timeout. Cualquier otra cosa es que hay alguien.
        Serial.printf("  > Detectada actividad en Broadcast 0x%02X\n", broads[i]);
      }
    }

    // --- PARTE 2: BARRIDO ESPECIFICO (Detiene el escaneo) ---
    for (int id_test = 1; id_test <= 3; id_test++) {
      Serial.printf("  Verificando ID %d... ", id_test);
      
      node.begin(id_test, Serial1);
      node.clearResponseBuffer();
      
      uint8_t res = node.readHoldingRegisters(0x0000, 1);
      if (res != 0xE2) {
        Serial.printf("¡ENCONTRADO!\n\n");
        Serial.println("*********************************");
        Serial.printf("  SENSOR DETECTADO CON EXITO\n");
        Serial.printf("  BAUDRATE: %ld\n", baudrates[b]);
        Serial.printf("  ID REAL:  %d\n", id_test);
        Serial.println("*********************************");
        return; // Aquí se detiene todo
      }
      Serial.println("No");
    }
  }
  Serial.println("\nEscaneo finalizado sin detectar una ID específica (1, 2 o 3).");
}

void setup() {
  Serial.begin(115200);
  pinMode(RE_DE_PIN, OUTPUT);
  pinMode(RS485_PWR_PIN, OUTPUT);
  pinMode(SENSOR_PWR_PIN, OUTPUT);
  controlPin(RS485_PWR_PIN, LOGICA_RS485_INVERSA, true);
  controlPin(SENSOR_PWR_PIN, LOGICA_SENSOR_INVERSA, true);
  if (!LittleFS.begin(true)) Serial.println("Error LittleFS");
  
  Serial.println("\nSistema Iniciado.");

    Serial.println("\n----------------------------------");
  Serial.println("¿Desea ver las instrucciones? (Y/N)");
  if (leerConsola().equalsIgnoreCase("y")) {
    Serial.println(TEXTO_INSTRUCCIONES);
  }
}

void loop() {
   // 1. Menú de opciones
  Serial.println("\n--- MENU PRINCIPAL ---");
  Serial.println("1: Usar Sensor Conocido (JSON)");
  Serial.println("2: Modo Manual (Desconocido)");
  Serial.println("3: ESCANEAR Sensor (Auto-deteccion)");
  
  int opcion = leerConsola().toInt();

  if (opcion == 3) {
    escanearSensor();
    return; // Reinicia el loop tras escanear
  }

  // Configuración de ID y Baudrate para opciones 1 y 2
  Serial.println("Ingrese ID del esclavo (actual):");
  uint8_t id = leerConsola().toInt();
  
  Serial.println("Seleccione Baudrate (1:2400, 2:4800, 3:9600, 4:19200):");
  int selB = leerConsola().toInt();
  long baud = baudrates[constrain(selB-1, 0, 3)];

  Serial1.begin(baud, SERIAL_8N1, RX_PIN, TX_PIN);
  node.begin(id, Serial1);
  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);

  if (opcion == 1) {
    File file = LittleFS.open("/sensores.json", "r");
    DynamicJsonDocument doc(4096);
    deserializeJson(doc, file);
    file.close();

    JsonArray sensores = doc.as<JsonArray>();
    Serial.println("\nSeleccione un sensor guardado:");
    for (int i = 0; i < sensores.size(); i++) {
      Serial.printf("%d: %s\n", i, sensores[i]["nombre"].as<const char*>());
    }
    int s_idx = leerConsola().toInt();
    JsonObject s_data = sensores[s_idx];

    Serial.println("ACCION:\n1: Leer Datos\n2: Cambiar ID del Sensor");
    int accion = leerConsola().toInt();

    if (accion == 1) {
      JsonArray vars = s_data["variables"].as<JsonArray>();
      Serial.println("\n--- LECTURA DE DATOS ---");
      for (JsonObject v : vars) {
        node.clearResponseBuffer();
        uint8_t res = node.readHoldingRegisters(v["reg"], v["size"]|1);
        if (res == node.ku8MBSuccess) {
          float raw = (v["size"]|1) == 2 ? ((uint32_t)node.getResponseBuffer(0)<<16|node.getResponseBuffer(1)) : node.getResponseBuffer(0);
          float resultado = (raw * v["mult"].as<float>()) + v["off"].as<float>();
          Serial.printf(">> %s: %.2f %s\n", v["etiqueta"].as<const char*>(), resultado, v["uni"].as<const char*>());
        } else {
          Serial.printf(">> %s: ERROR [0x%02X]\n", v["etiqueta"].as<const char*>(), res);
        }
        delay(200);
      }
    } else if (accion == 2) {
      Serial.println("Ingrese la NUEVA ID deseada (1-247):");
      int nueva_id = leerConsola().toInt();
      
      // Intentamos escribir en 0x0200 (Dirección del esclavo según manual)
      node.clearResponseBuffer();
      uint8_t res = node.writeSingleRegister(0x0200, nueva_id);
      
      if (res == node.ku8MBSuccess) {
        Serial.printf("EXITO: ID cambiada a %d. El sensor ya no respondera en la ID antigua.\n", nueva_id);
      } else {
        // Si falla con 0x0200, intentamos con 0x0002 por si acaso es una versión distinta
        Serial.println("Fallo en 0x0200, reintentando en registro 2...");
        res = node.writeSingleRegister(0x0002, nueva_id);
        
        if(res == node.ku8MBSuccess) Serial.println("Exito en registro 2.");
        else Serial.printf("ERROR CRITICO: 0x%02X\n", res);
      }
    }
  } else if (opcion == 2) {
    // Modo manual... (abreviado por brevedad, mantiene tu lógica anterior)
    Serial.println("1: Leer (F03) | 2: Escribir (F06)");
    int modo = leerConsola().toInt();
    // Dentro del bloque de Modo Manual
if (modo == 1) { // Lógica de Lectura
      Serial.println("Direccion del registro (Decimal):");
      int reg = leerConsola().toInt();
      Serial.println("Cantidad de registros a leer:");
      int cant = leerConsola().toInt();
      
      uint8_t res = node.readHoldingRegisters(reg, cant);
      if (res == node.ku8MBSuccess) {
        for (int i = 0; i < cant; i++) {
          Serial.printf("Reg %d: %u (0x%04X)\n", reg + i, node.getResponseBuffer(i), node.getResponseBuffer(i));
        }
      } else {
        Serial.printf("Error Modbus: 0x%02X\n", res);
      }
    } 
    else if (modo == 2) { // Lógica de Escritura
      Serial.println("Direccion del registro a escribir (Decimal):");
      int regEscritura = leerConsola().toInt();
      
      Serial.println("Valor a escribir (0-65535):");
      int valorEscritura = leerConsola().toInt();

      Serial.printf("Escribiendo %d en el registro %d...\n", valorEscritura, regEscritura);
      
      node.clearResponseBuffer();
      uint8_t res = node.writeSingleRegister(regEscritura, valorEscritura);

      if (res == node.ku8MBSuccess) {
        Serial.println("¡Escritura EXITOSA!");
      } else {
        Serial.printf("ERROR al escribir: 0x%02X\n", res);
        if (res == 0x01) Serial.println("-> El sensor no soporta esta funcion.");
        if (res == 0x02) Serial.println("-> El registro no existe.");
      }
    }
    }
  
  delay(1000);
}