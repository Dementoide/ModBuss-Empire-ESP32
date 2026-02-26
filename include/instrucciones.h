#ifndef INSTRUCCIONES_H
#define INSTRUCCIONES_H

const char* TEXTO_INSTRUCCIONES = 
"====================================================\n"
"           MANUAL DE OPERACION MODBUS EMPIRE        \n"
"====================================================\n"
"1. Verifique si los pines de control tienen logica inversa\n"
"2. Para escribir nueva ID recuerda convertir la direccion del registro a decimal\n"
"3. Si el sensor es 'Registrado', el sistema aplicara:\n"
"   - Direcciones de registros automaticas.\n"
"   - Conversion de unidades (Formula: x * M + O).\n"
"4. Si es 'Desconocido', debera ingresar manualmente\n"
"   la direccion del registro a leer/escribir.\n"
"5. Verifique que el Baudrate coincida con el sensor.\n"
"====================================================\n";

#endif