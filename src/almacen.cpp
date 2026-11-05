/**
 * ALMACEN.CPP
 * Montaje de LittleFS (joltwallet/littlefs) y lectura/escritura por stdio.
 */
#include "almacen.h"

#include <stdio.h>
#include <esp_littlefs.h>

static constexpr const char* kParticion = "littlefs";

bool almacenMontar()
{
  if (esp_littlefs_mounted(kParticion)) return true;
  esp_vfs_littlefs_conf_t conf = {};
  conf.base_path = ALMACEN_BASE;
  conf.partition_label = kParticion;
  conf.format_if_mount_failed = true;
  return esp_vfs_littlefs_register(&conf) == ESP_OK;
}

bool almacenLeer(const char* ruta, std::string* out)
{
  FILE* f = fopen(ruta, "rb");
  if (f == nullptr) return false;
  out->clear();
  char buf[256];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out->append(buf, n);
  fclose(f);
  return true;
}

bool almacenEscribir(const char* ruta, const std::string& contenido)
{
  FILE* f = fopen(ruta, "wb");
  if (f == nullptr) return false;
  const bool ok = fwrite(contenido.data(), 1, contenido.size(), f) == contenido.size();
  return (fclose(f) == 0) && ok;
}
