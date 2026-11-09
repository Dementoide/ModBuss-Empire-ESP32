"""
GENERAR_ICONO.PY
Dibuja gui/graficador.ico (una curva sobre una grilla) solo con Tk: el .ico lleva
PNGs dentro, que Windows lee desde Vista. Se corre una vez; el .ico va al repo.
"""

import struct
import tkinter as tk
from pathlib import Path

DESTINO = Path(__file__).resolve().parent / "graficador.ico"
TAMANOS = (16, 32, 48, 64, 256)


def dibujar(lado):
    img = tk.PhotoImage(width=lado, height=lado)
    fondo, grilla, curva, curva2 = "#16324f", "#2c5378", "#4fd1c5", "#f6ad55"
    radio = max(2, lado // 6)
    for y in range(lado):
        fila = []
        for x in range(lado):
            # Esquinas redondeadas: fuera del radio queda transparente.
            dx = max(radio - x, x - (lado - 1 - radio), 0)
            dy = max(radio - y, y - (lado - 1 - radio), 0)
            fila.append(fondo if dx * dx + dy * dy <= radio * radio else None)
        for x, c in enumerate(fila):
            if c:
                img.put(c, (x, y))
    paso = max(4, lado // 4)
    for k in range(paso, lado, paso):
        for t in range(radio, lado - radio):
            img.put(grilla, (k, t))
            img.put(grilla, (t, k))

    def linea(puntos, color, grosor):
        for (x0, y0), (x1, y1) in zip(puntos, puntos[1:]):
            n = max(abs(x1 - x0), abs(y1 - y0), 1) * 2
            for i in range(n + 1):
                x = x0 + (x1 - x0) * i / n
                y = y0 + (y1 - y0) * i / n
                for gx in range(grosor):
                    for gy in range(grosor):
                        px, py = int(x) + gx - grosor // 2, int(y) + gy - grosor // 2
                        if 0 <= px < lado and 0 <= py < lado:
                            img.put(color, (px, py))

    m = lado / 64
    linea([(int(8 * m), int(46 * m)), (int(20 * m), int(30 * m)), (int(30 * m), int(38 * m)),
           (int(42 * m), int(16 * m)), (int(56 * m), int(24 * m))], curva, max(1, lado // 16))
    linea([(int(8 * m), int(54 * m)), (int(24 * m), int(48 * m)), (int(38 * m), int(52 * m)),
           (int(56 * m), int(40 * m))], curva2, max(1, lado // 24))
    return img


def main():
    raiz = tk.Tk()
    raiz.withdraw()
    pngs = []
    tmp = DESTINO.with_suffix(".tmp.png")
    for lado in TAMANOS:
        dibujar(lado).write(str(tmp), format="png")
        pngs.append((lado, tmp.read_bytes()))
    tmp.unlink()
    raiz.destroy()

    cabecera = struct.pack("<HHH", 0, 1, len(pngs))
    directorio = b""
    datos = b""
    desplazamiento = 6 + 16 * len(pngs)
    for lado, png in pngs:
        lado_byte = 0 if lado >= 256 else lado
        directorio += struct.pack("<BBBBHHII", lado_byte, lado_byte, 0, 0, 1, 32, len(png),
                                  desplazamiento + len(datos))
        datos += png
    DESTINO.write_bytes(cabecera + directorio + datos)
    print(f"{DESTINO} ({DESTINO.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
