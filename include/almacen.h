/**
 * ALMACEN.H
 * Partición LittleFS ("littlefs", montada en /littlefs) con archivos de texto.
 *
 * La imagen sale de data/ con `pio run -t uploadfs`.
 */
#pragma once

#include <string>

#define ALMACEN_BASE "/littlefs"

// Monta la partición; la formatea si no se puede montar. false si falló igual.
bool almacenMontar();

bool almacenLeer(const char* ruta, std::string* out);
bool almacenEscribir(const char* ruta, const std::string& contenido);
