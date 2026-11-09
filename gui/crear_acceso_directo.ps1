# Crea en el escritorio el acceso directo "ModbusEmpire Graficador", que abre la
# interfaz con pythonw.exe (sin ventana de consola) y el icono gui\graficador.ico.
# Se corre una vez:  powershell -ExecutionPolicy Bypass -File <ruta>\crear_acceso_directo.ps1

$gui = Split-Path -Parent $MyInvocation.MyCommand.Path
$pythonw = 'C:\Python314\pythonw.exe'
if (-not (Test-Path $pythonw)) {
    $cmd = Get-Command pythonw.exe -ErrorAction SilentlyContinue
    if ($null -eq $cmd) { throw 'No se encontro pythonw.exe (instale Python 3 con Tk y pyserial).' }
    $pythonw = $cmd.Source
}

$escritorio = [Environment]::GetFolderPath('Desktop')
$destino = Join-Path $escritorio 'ModbusEmpire Graficador.lnk'
$shell = New-Object -ComObject WScript.Shell
$acceso = $shell.CreateShortcut($destino)
$acceso.TargetPath = $pythonw
$acceso.Arguments = '"' + (Join-Path $gui 'graficador.py') + '"'
$acceso.WorkingDirectory = $gui
$acceso.Description = 'Graficador de registros Modbus via ESP32 (ModbusEmpire)'
$icono = Join-Path $gui 'graficador.ico'
if (Test-Path $icono) { $acceso.IconLocation = "$icono,0" }
$acceso.Save()
Write-Output "Acceso directo creado: $destino"
