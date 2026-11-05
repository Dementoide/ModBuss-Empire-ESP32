# Componente main de ModbusEmpire. Un .cpp nuevo bajo src/ entra al build solo
# si se agrega a SRCS.
idf_component_register(
  SRCS main.cpp
       modbus_rtu.cpp
       consola.cpp
       almacen.cpp
  INCLUDE_DIRS . ${CMAKE_SOURCE_DIR}/include
  REQUIRES esp_driver_uart esp_driver_gpio esp_timer vfs littlefs
)
