#ifndef INSTRUCCIONES_H
#define INSTRUCCIONES_H

static const char* TEXTO_INSTRUCCIONES =
"====================================================\n"
"           MANUAL DE OPERACION MODBUS EMPIRE        \n"
"====================================================\n"
"1. Verifique si los pines de control tienen logica inversa.\n"
"   Menu 4 (Diagnostico) dice si hay transceptor RS485 vivo.\n"
"2. Direcciones en decimal o hex (0x0200). Si el manual usa\n"
"   notacion 40001/30001, reste: 40001 -> 0 con F03,\n"
"   30001 -> 0 con F04.\n"
"3. Sensor 'Registrado' (JSON): se carga a la tabla con sus\n"
"   registros y conversion (valor = crudo * M + O).\n"
"4. Modo Manual: elija a mano registro, largo y tipo:\n"
"   - Lectura rapida: direccion + largo (1-125), muestra cada\n"
"     registro como u16/s16/hex y los pares como u32/s32/f32\n"
"     en orden ABCD y CDAB para descubrir el formato.\n"
"   - Tabla: agregue registros (F03/F04, u16 s16 u32 s32 f32,\n"
"     orden, M, O, unidad), leala una vez o en ciclo, y\n"
"     guardela en sensores.json como sensor nuevo.\n"
"5. Verifique que el Baudrate coincida con el sensor (el\n"
"   escaneo deja la ID y baudrate encontrados en la sesion).\n"
"6. Enter vacio = valor por defecto [entre corchetes].\n"
"====================================================\n";

#endif
