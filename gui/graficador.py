"""
GRAFICADOR.PY
Interfaz gráfica de ModbusEmpire: muestrea registros Modbus a través de la ESP32
(protocolo '@' de include/flujo.h) y los grafica en vivo.

Solo usa la biblioteca estándar de Python (Tk) y pyserial.
Se abre con el acceso directo del escritorio (gui/crear_acceso_directo.ps1), que
lo lanza con pythonw.exe: sin ventana de consola.
"""

import csv
import json
import math
import queue
import threading
import time
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

import serial
import serial.tools.list_ports

RUTA_SENSORES = Path(__file__).resolve().parent.parent / "data" / "sensores.json"
BAUDS_CONSOLA = 115200
BAUDS_BUS = [1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200]
MAX_REGS = 64            # FLUJO_MAX_REGS del firmware
MAX_REGS_TRAMA = 125     # MB_MAX_REGISTROS
HUECO = 16               # registros faltantes que el firmware lee de más para unir tramas
LATENCIA_TIPICA_MS = 20  # supuesto de respuesta del esclavo, solo para la estimación previa
COLORES = ["#1f77b4", "#d62728", "#2ca02c", "#ff7f0e", "#9467bd", "#8c564b",
           "#e377c2", "#17becf", "#bcbd22", "#7f7f7f"]


# ---------------------------------------------------------------------------
# Lista de registros y tiempos: misma lógica que include/lista_registros.h
# ---------------------------------------------------------------------------

def parsear_lista(texto):
    """'0,4,8-10,0x13' -> [0, 4, 8, 9, 10, 19] (orden pedido, sin repetidos)."""
    regs = []
    for parte in texto.split(","):
        parte = parte.strip()
        if not parte:
            raise ValueError("hay un elemento vacio")
        if "-" in parte[1:]:
            a, b = parte.split("-", 1)
            a, b = int(a.strip(), 0), int(b.strip(), 0)
        else:
            a = b = int(parte, 0)
        if a < 0 or b > 0xFFFF or b < a:
            raise ValueError(f"intervalo invalido: {parte}")
        for r in range(a, b + 1):
            if r not in regs:
                regs.append(r)
                if len(regs) > MAX_REGS:
                    raise ValueError(f"maximo {MAX_REGS} registros")
    return regs


def agrupar_bloques(regs, hueco=HUECO, max_cant=MAX_REGS_TRAMA):
    bloques = []
    for r in sorted(set(regs)):
        if bloques:
            ini, cant = bloques[-1]
            if r - (ini + cant) <= hueco and r - ini + 1 <= max_cant:
                bloques[-1] = (ini, r - ini + 1)
                continue
        bloques.append((r, 1))
    return bloques


def ms_trama(cant, baud):
    return (8 + 5 + 2 * cant) * 10 * 1000 / baud


def ms_silencio(baud):
    return (35000 + baud - 1) // baud + 1


def estimar_ms_muestra(regs, baud, latencia_ms=LATENCIA_TIPICA_MS):
    """(solo cable, con latencia supuesta) en ms por muestra."""
    bloques = agrupar_bloques(regs)
    cable = sum(ms_trama(c, baud) for _, c in bloques) + ms_silencio(baud) * len(bloques)
    return cable, cable + latencia_ms * len(bloques), len(bloques)


# ---------------------------------------------------------------------------
# Enlace serie con la ESP32
# ---------------------------------------------------------------------------

class Enlace:
    """Puerto serie en un hilo lector; las líneas llegan por `cola`."""

    def __init__(self):
        self.puerto = None
        self.cola = queue.Queue()
        self._hilo = None
        self._vivo = False

    def abrir(self, nombre):
        s = serial.Serial()
        s.port = nombre
        s.baudrate = BAUDS_CONSOLA
        s.timeout = 0.1
        # Sin tocar DTR/RTS: en placas con auto-reset, abrir reiniciaría la ESP.
        s.dtr = False
        s.rts = False
        s.open()
        self.puerto = s
        self._vivo = True
        self._hilo = threading.Thread(target=self._leer, daemon=True)
        self._hilo.start()

    def cerrar(self):
        self._vivo = False
        if self._hilo:
            self._hilo.join(timeout=1)
        if self.puerto:
            try:
                self.puerto.close()
            except serial.SerialException:
                pass
        self.puerto = None

    def enviar(self, linea):
        if self.puerto:
            self.puerto.write((linea + "\n").encode("ascii"))

    def _leer(self):
        resto = b""
        while self._vivo:
            try:
                datos = self.puerto.read(4096)
            except serial.SerialException as e:
                self.cola.put(("desconectado", str(e)))
                return
            if not datos:
                continue
            resto += datos
            *lineas, resto = resto.split(b"\n")
            for l in lineas:
                self.cola.put(("linea", l.decode("utf-8", "replace").strip()))


# ---------------------------------------------------------------------------
# Gráfico en un Canvas de Tk
# ---------------------------------------------------------------------------

class Grafico(tk.Canvas):
    """Series en el tiempo con ventana (span) móvil, paneles separados o eje común."""

    MARGEN_IZQ = 70
    MARGEN_DER = 12
    MARGEN_SUP = 8
    MARGEN_INF = 26

    def __init__(self, master, app):
        super().__init__(master, background="white", highlightthickness=0)
        self.app = app
        self.cursor_x = None
        self._arrastre = None
        self.bind("<Configure>", lambda e: self.app.pedir_redibujo())
        self.bind("<ButtonPress-1>", self._presionar)
        self.bind("<B1-Motion>", self._arrastrar)
        self.bind("<ButtonRelease-1>", lambda e: setattr(self, "_arrastre", None))
        self.bind("<MouseWheel>", self._rueda)
        self.bind("<Motion>", self._mover)
        self.bind("<Leave>", self._salir)

    # --- interacción ---
    def _ancho_util(self):
        return max(10, self.winfo_width() - self.MARGEN_IZQ - self.MARGEN_DER)

    def _presionar(self, e):
        self._arrastre = (e.x, self.app.vista_inicio())

    def _arrastrar(self, e):
        if not self._arrastre:
            return
        x0, inicio = self._arrastre
        dt = (x0 - e.x) / self._ancho_util() * self.app.span
        self.app.mover_vista(inicio + dt)

    def _rueda(self, e):
        factor = 0.8 if e.delta > 0 else 1.25
        frac = min(1.0, max(0.0, (e.x - self.MARGEN_IZQ) / self._ancho_util()))
        self.app.zoom(factor, frac)

    def _mover(self, e):
        self.cursor_x = e.x
        self.app.pedir_redibujo()

    def _salir(self, _e):
        self.cursor_x = None
        self.app.pedir_redibujo()

    # --- dibujo ---
    def dibujar(self):
        self.delete("all")
        w, h = self.winfo_width(), self.winfo_height()
        series = self.app.series_visibles()
        t0 = self.app.vista_inicio()
        t1 = t0 + self.app.span
        if not series or not self.app.tiempos:
            self.create_text(w / 2, h / 2, text="Sin datos: conecte, elija registros e inicie el muestreo",
                             fill="#888")
            return

        separado = self.app.var_paneles.get()
        grupos = [[s] for s in series] if separado else [series]
        alto_panel = (h - self.MARGEN_INF) / len(grupos)
        ancho = self._ancho_util()
        x_de = lambda t: self.MARGEN_IZQ + (t - t0) / self.app.span * ancho

        tiempos = self.app.tiempos
        i0 = max(0, self._indice(tiempos, t0) - 1)
        i1 = min(len(tiempos), self._indice(tiempos, t1) + 1)

        cursor_t = None
        if self.cursor_x is not None and self.MARGEN_IZQ <= self.cursor_x <= self.MARGEN_IZQ + ancho:
            cursor_t = t0 + (self.cursor_x - self.MARGEN_IZQ) / ancho * self.app.span
        idx_cursor = None
        if cursor_t is not None and tiempos:
            j = self._indice(tiempos, cursor_t)
            cands = [k for k in (j - 1, j) if 0 <= k < len(tiempos)]
            idx_cursor = min(cands, key=lambda k: abs(tiempos[k] - cursor_t)) if cands else None

        for g, grupo in enumerate(grupos):
            y_sup = self.MARGEN_SUP + g * alto_panel
            y_inf = (g + 1) * alto_panel - (4 if separado else 0)
            if g == len(grupos) - 1:
                y_inf = h - self.MARGEN_INF
            self._panel(grupo, i0, i1, x_de, y_sup, y_inf, ancho, idx_cursor)

        self._eje_tiempo(t0, t1, x_de, h, ancho)
        if idx_cursor is not None:
            x = x_de(tiempos[idx_cursor])
            self.create_line(x, self.MARGEN_SUP, x, h - self.MARGEN_INF, fill="#999", dash=(3, 3))
            self.create_text(x + 4, h - self.MARGEN_INF - 4, anchor="sw", fill="#555",
                             text=f"t={tiempos[idx_cursor]:.2f} s")

    @staticmethod
    def _indice(lista, valor):
        lo, hi = 0, len(lista)
        while lo < hi:
            mid = (lo + hi) // 2
            if lista[mid] < valor:
                lo = mid + 1
            else:
                hi = mid
        return lo

    def _rango_y(self, grupo, i0, i1):
        if not self.app.var_y_auto.get():
            try:
                return float(self.app.var_y_min.get()), float(self.app.var_y_max.get())
            except ValueError:
                pass
        vals = [v for s in grupo for v in s["valores"][i0:i1] if v is not None]
        if not vals:
            return 0.0, 1.0
        lo, hi = min(vals), max(vals)
        if lo == hi:
            d = abs(lo) * 0.05 or 1.0
            return lo - d, hi + d
        d = (hi - lo) * 0.08
        return lo - d, hi + d

    def _panel(self, grupo, i0, i1, x_de, y_sup, y_inf, ancho, idx_cursor):
        izq, der = self.MARGEN_IZQ, self.MARGEN_IZQ + ancho
        self.create_rectangle(izq, y_sup, der, y_inf, outline="#ccc")
        lo, hi = self._rango_y(grupo, i0, i1)
        y_de = lambda v: y_inf - (v - lo) / (hi - lo) * (y_inf - y_sup)

        for v in _marcas(lo, hi, max(2, int((y_inf - y_sup) / 35))):
            y = y_de(v)
            if y_sup <= y <= y_inf:
                self.create_line(izq, y, der, y, fill="#eee")
                self.create_text(izq - 4, y, text=_formato(v), anchor="e", fill="#555", font=("Segoe UI", 8))

        tiempos = self.app.tiempos
        leyenda_y = y_sup + 4
        for s in grupo:
            puntos = _reducir(tiempos, s["valores"], i0, i1, ancho)
            tramo = []
            for t, v in puntos:
                if v is None:
                    if len(tramo) >= 4:
                        self.create_line(*tramo, fill=s["color"], width=1.5)
                    tramo = []
                    continue
                tramo += [x_de(t), y_de(v)]
            if len(tramo) >= 4:
                self.create_line(*tramo, fill=s["color"], width=1.5)
            elif len(tramo) == 2:
                self.create_oval(tramo[0] - 2, tramo[1] - 2, tramo[0] + 2, tramo[1] + 2, fill=s["color"], outline="")

            valor = None
            if idx_cursor is not None and idx_cursor < len(s["valores"]):
                valor = s["valores"][idx_cursor]
            elif s["valores"]:
                valor = s["valores"][-1]
            texto = f"{s['nombre']}: {_formato(valor) if valor is not None else '-'} {s['unidad']}"
            self.create_text(izq + 6, leyenda_y, anchor="nw", text=texto, fill=s["color"],
                             font=("Segoe UI", 9, "bold"))
            leyenda_y += 15

    def _eje_tiempo(self, t0, t1, x_de, h, ancho):
        y = h - self.MARGEN_INF
        for t in _marcas(t0, t1, max(2, int(ancho / 90))):
            x = x_de(t)
            if self.MARGEN_IZQ <= x <= self.MARGEN_IZQ + ancho:
                self.create_line(x, y, x, y + 4, fill="#888")
                self.create_text(x, y + 6, text=_formato_tiempo(t), anchor="n", fill="#555", font=("Segoe UI", 8))


def _marcas(lo, hi, n):
    """Valores 'redondos' para las marcas de un eje."""
    if hi <= lo:
        return [lo]
    paso = (hi - lo) / n
    mag = 10 ** math.floor(math.log10(paso))
    for m in (1, 2, 2.5, 5, 10):
        if m * mag >= paso:
            paso = m * mag
            break
    inicio = math.ceil(lo / paso) * paso
    return [inicio + k * paso for k in range(int((hi - inicio) / paso) + 1)]


def _formato(v):
    if v is None:
        return "-"
    if abs(v) >= 1000 or v == int(v):
        return f"{v:.0f}" if abs(v - round(v)) < 1e-9 else f"{v:.1f}"
    return f"{v:.3g}" if abs(v) < 1 else f"{v:.2f}".rstrip("0").rstrip(".")


def _formato_tiempo(t):
    if t < 120:
        return f"{t:.0f} s" if t == int(t) else f"{t:.1f} s"
    m, s = divmod(int(t), 60)
    return f"{m}:{s:02d}"


def _reducir(tiempos, valores, i0, i1, ancho):
    """Puntos a dibujar: todos si caben; si no, mínimo y máximo por columna de píxel."""
    n = i1 - i0
    if n <= ancho * 2:
        return [(tiempos[i], valores[i]) for i in range(i0, i1)]
    salida = []
    por_col = n / ancho
    for c in range(int(ancho)):
        a, b = i0 + int(c * por_col), min(i1, i0 + int((c + 1) * por_col))
        tramo = [(tiempos[i], valores[i]) for i in range(a, b) if valores[i] is not None]
        if not tramo:
            salida.append((tiempos[a], None))
            continue
        mn = min(tramo, key=lambda p: p[1])
        mx = max(tramo, key=lambda p: p[1])
        salida += sorted({mn, mx}, key=lambda p: p[0])
    return salida


# ---------------------------------------------------------------------------
# Exportar: qué registros y qué tramo de tiempo
# ---------------------------------------------------------------------------

class DialogoExportar(tk.Toplevel):
    """Elige registros (todos, los graficados o a mano), columnas y el tramo de
    tiempo con una barra de recorte de dos manijas, y escribe un CSV.

    Trabaja sobre una copia de los datos al abrirse: el muestreo puede seguir."""

    ALTO_BARRA = 110
    MARGEN = 12
    MANIJA = 6

    def __init__(self, app):
        super().__init__(app)
        self.app = app
        self.title("Exportar datos")
        self.transient(app)
        self.minsize(640, 0)

        n = len(app.tiempos)
        self.tiempos = app.tiempos[:n]
        self.horas = app.horas[:n]
        self.regs = [dict(r, crudos=r["crudos"][:n], valores=r["valores"][:n]) for r in app.regs]
        self.t_fin_total = self.tiempos[-1] if self.tiempos else 0.0

        self.var_quienes = tk.StringVar(value="graficados")
        self.var_valor = tk.BooleanVar(value=True)
        self.var_crudo = tk.BooleanVar(value=True)
        self.var_hora = tk.BooleanVar(value=True)
        self.var_coma = tk.BooleanVar(value=True)
        self.var_desde = tk.StringVar()
        self.var_hasta = tk.StringVar()
        self.var_resumen = tk.StringVar()
        self.desde = 0.0
        self.hasta = self.t_fin_total
        self._arrastre = None

        self._armar()
        self._fijar_rango(0.0, self.t_fin_total)
        self.grab_set()
        self.focus_set()

    # --- interfaz ---
    def _armar(self):
        cuerpo = ttk.Frame(self, padding=10)
        cuerpo.pack(fill="both", expand=True)

        quienes = ttk.LabelFrame(cuerpo, text="Registros", padding=6)
        quienes.grid(row=0, column=0, sticky="nsew")
        for texto, valor in (("Todos los de la tabla", "todos"), ("Los mostrados en el gráfico", "graficados"),
                             ("Elegir a mano (Ctrl/Shift + clic):", "manual")):
            ttk.Radiobutton(quienes, text=texto, value=valor, variable=self.var_quienes,
                            command=self._cambio_quienes).pack(anchor="w")
        marco = ttk.Frame(quienes)
        marco.pack(fill="both", expand=True, pady=(2, 0))
        self.lista = tk.Listbox(marco, selectmode="extended", height=8, exportselection=False)
        for r in self.regs:
            self.lista.insert("end", f"{r['reg']:>5}  {r['nombre']}  {r['unidad']}")
        self.lista.pack(side="left", fill="both", expand=True)
        sb = ttk.Scrollbar(marco, orient="vertical", command=self.lista.yview)
        sb.pack(side="right", fill="y")
        self.lista.configure(yscrollcommand=sb.set)
        self.lista.bind("<<ListboxSelect>>", lambda e: self._redibujar())

        cols = ttk.LabelFrame(cuerpo, text="Columnas", padding=6)
        cols.grid(row=0, column=1, sticky="nsew", padx=(10, 0))
        ttk.Checkbutton(cols, text="Valor convertido (crudo × mult)", variable=self.var_valor).pack(anchor="w")
        ttk.Checkbutton(cols, text="Valor crudo del registro", variable=self.var_crudo).pack(anchor="w")
        ttk.Checkbutton(cols, text="Hora del PC", variable=self.var_hora).pack(anchor="w")
        ttk.Checkbutton(cols, text="Decimales con coma (Excel en español)", variable=self.var_coma).pack(anchor="w")
        ttk.Label(cols, text="Siempre va t_s (segundos desde\nla primera muestra).", foreground="#888").pack(
            anchor="w", pady=(6, 0))
        cuerpo.columnconfigure(0, weight=1)

        tramo = ttk.LabelFrame(cuerpo, text="Tramo de tiempo  (arrastre las manijas o la zona elegida)",
                               padding=6)
        tramo.grid(row=1, column=0, columnspan=2, sticky="ew", pady=(10, 0))
        self.barra = tk.Canvas(tramo, height=self.ALTO_BARRA, background="#fafafa", highlightthickness=1,
                               highlightbackground="#ccc")
        self.barra.pack(fill="x", expand=True)
        self.barra.bind("<Configure>", lambda e: self._redibujar())
        self.barra.bind("<ButtonPress-1>", self._presionar)
        self.barra.bind("<B1-Motion>", self._arrastrar)
        self.barra.bind("<ButtonRelease-1>", lambda e: setattr(self, "_arrastre", None))
        self.barra.bind("<Motion>", self._cursor)

        campos = ttk.Frame(tramo)
        campos.pack(fill="x", pady=(6, 0))
        ttk.Label(campos, text="Desde (s)").pack(side="left")
        e1 = ttk.Entry(campos, textvariable=self.var_desde, width=9)
        e1.pack(side="left", padx=4)
        ttk.Label(campos, text="Hasta (s)").pack(side="left", padx=(8, 0))
        e2 = ttk.Entry(campos, textvariable=self.var_hasta, width=9)
        e2.pack(side="left", padx=4)
        for e in (e1, e2):
            e.bind("<Return>", lambda _e: self._desde_campos())
            e.bind("<FocusOut>", lambda _e: self._desde_campos())
        ttk.Button(campos, text="Todo", command=lambda: self._fijar_rango(0.0, self.t_fin_total)).pack(
            side="left", padx=(12, 2))
        ttk.Button(campos, text="Lo que muestra el gráfico", command=self._rango_de_la_vista).pack(side="left")
        ttk.Label(campos, textvariable=self.var_resumen, foreground="#1f5f9f").pack(side="right")

        botones = ttk.Frame(cuerpo)
        botones.grid(row=2, column=0, columnspan=2, sticky="e", pady=(10, 0))
        ttk.Button(botones, text="Cancelar", command=self.destroy).pack(side="right")
        ttk.Button(botones, text="Exportar…", command=self._exportar).pack(side="right", padx=6)
        self._cambio_quienes()

    def _cambio_quienes(self):
        manual = self.var_quienes.get() == "manual"
        self.lista.configure(state="normal" if manual else "disabled")
        self._redibujar()

    def _elegidos(self):
        modo = self.var_quienes.get()
        if modo == "todos":
            return list(self.regs)
        if modo == "graficados":
            return [r for r in self.regs if r["graficar"]]
        return [self.regs[i] for i in self.lista.curselection()]

    # --- rango ---
    def _fijar_rango(self, desde, hasta):
        desde = max(0.0, min(desde, self.t_fin_total))
        hasta = max(0.0, min(hasta, self.t_fin_total))
        if hasta < desde:
            desde, hasta = hasta, desde
        self.desde, self.hasta = desde, hasta
        self.var_desde.set(f"{desde:.2f}")
        self.var_hasta.set(f"{hasta:.2f}")
        self._redibujar()

    def _desde_campos(self):
        try:
            self._fijar_rango(float(self.var_desde.get().replace(",", ".")),
                              float(self.var_hasta.get().replace(",", ".")))
        except ValueError:
            self._fijar_rango(self.desde, self.hasta)

    def _rango_de_la_vista(self):
        ini = self.app.vista_inicio()
        self._fijar_rango(ini, ini + self.app.span)

    def _indices(self):
        return [i for i, t in enumerate(self.tiempos) if self.desde <= t <= self.hasta]

    # --- barra de recorte ---
    def _ancho(self):
        return max(10, self.barra.winfo_width() - 2 * self.MARGEN)

    def _x_de(self, t):
        return self.MARGEN + (t / self.t_fin_total * self._ancho() if self.t_fin_total > 0 else 0)

    def _t_de(self, x):
        return (x - self.MARGEN) / self._ancho() * self.t_fin_total

    def _presionar(self, e):
        xd, xh = self._x_de(self.desde), self._x_de(self.hasta)
        if abs(e.x - xd) <= self.MANIJA + 2:
            self._arrastre = ("desde", None)
        elif abs(e.x - xh) <= self.MANIJA + 2:
            self._arrastre = ("hasta", None)
        elif xd < e.x < xh:
            self._arrastre = ("mover", (e.x, self.desde, self.hasta))
        else:
            # Clic fuera de la zona: la manija más cercana salta ahí.
            t = self._t_de(e.x)
            if abs(e.x - xd) < abs(e.x - xh):
                self._fijar_rango(t, self.hasta)
                self._arrastre = ("desde", None)
            else:
                self._fijar_rango(self.desde, t)
                self._arrastre = ("hasta", None)

    def _arrastrar(self, e):
        if not self._arrastre:
            return
        modo, dato = self._arrastre
        t = self._t_de(e.x)
        if modo == "desde":
            self._fijar_rango(min(t, self.hasta), self.hasta)
        elif modo == "hasta":
            self._fijar_rango(self.desde, max(t, self.desde))
        else:
            x0, d0, h0 = dato
            dt = self._t_de(e.x) - self._t_de(x0)
            dt = max(-d0, min(dt, self.t_fin_total - h0))
            self._fijar_rango(d0 + dt, h0 + dt)

    def _cursor(self, e):
        xd, xh = self._x_de(self.desde), self._x_de(self.hasta)
        if abs(e.x - xd) <= self.MANIJA + 2 or abs(e.x - xh) <= self.MANIJA + 2:
            self.barra.configure(cursor="sb_h_double_arrow")
        elif xd < e.x < xh:
            self.barra.configure(cursor="fleur")
        else:
            self.barra.configure(cursor="")

    def _redibujar(self):
        c = self.barra
        c.delete("all")
        w, h = c.winfo_width(), self.ALTO_BARRA
        if w < 30 or not self.tiempos:
            return
        arriba, abajo = 8, h - 20
        ancho = self._ancho()
        # Miniatura de las series elegidas, cada una normalizada a su propio rango.
        for r in self._elegidos()[:8]:
            vals = r["valores"]
            reales = [v for v in vals if v is not None]
            if not reales:
                continue
            lo, hi = min(reales), max(reales)
            rango = (hi - lo) or 1.0
            tramo = []
            for t, v in _reducir(self.tiempos, vals, 0, len(vals), ancho):
                if v is None:
                    if len(tramo) >= 4:
                        c.create_line(*tramo, fill=r["color"])
                    tramo = []
                    continue
                tramo += [self._x_de(t), abajo - (v - lo) / rango * (abajo - arriba)]
            if len(tramo) >= 4:
                c.create_line(*tramo, fill=r["color"])
        # Fuera del tramo: sombreado. Dentro: borde y manijas.
        xd, xh = self._x_de(self.desde), self._x_de(self.hasta)
        c.create_rectangle(self.MARGEN, arriba - 4, xd, abajo + 4, fill="#000", stipple="gray25", outline="")
        c.create_rectangle(xh, arriba - 4, self.MARGEN + ancho, abajo + 4, fill="#000", stipple="gray25",
                           outline="")
        c.create_rectangle(xd, arriba - 4, xh, abajo + 4, outline="#1f5f9f", width=2)
        for x in (xd, xh):
            c.create_rectangle(x - self.MANIJA / 2, arriba - 6, x + self.MANIJA / 2, abajo + 6,
                               fill="#1f5f9f", outline="")
        for t in _marcas(0, self.t_fin_total, max(2, int(ancho / 90))):
            x = self._x_de(t)
            c.create_line(x, abajo + 6, x, abajo + 9, fill="#888")
            c.create_text(x, abajo + 10, text=_formato_tiempo(t), anchor="n", fill="#555", font=("Segoe UI", 8))
        self.var_resumen.set(f"{len(self._indices())} muestras · {self.hasta - self.desde:.1f} s")

    # --- escribir ---
    def _exportar(self):
        elegidos = self._elegidos()
        if not elegidos:
            messagebox.showwarning("Exportar", "No hay registros elegidos.", parent=self)
            return
        if not (self.var_valor.get() or self.var_crudo.get()):
            messagebox.showwarning("Exportar", "Elija valor convertido, crudo o ambos.", parent=self)
            return
        indices = self._indices()
        if not indices:
            messagebox.showwarning("Exportar", "El tramo elegido no tiene muestras.", parent=self)
            return
        ruta = filedialog.asksaveasfilename(parent=self, defaultextension=".csv", filetypes=[("CSV", "*.csv")],
                                            initialfile=time.strftime("muestras_%Y%m%d_%H%M%S.csv"))
        if not ruta:
            return
        self.escribir_csv(ruta, elegidos, indices)
        self.app.var_estado.set(f"Exportado: {ruta} ({len(indices)} muestras, {len(elegidos)} registros)")
        self.destroy()

    def escribir_csv(self, ruta, elegidos, indices):
        cab = ["t_s"] + (["hora_pc"] if self.var_hora.get() else [])
        if self.var_valor.get():
            cab += [f"{r['nombre']} (reg {r['reg']}) [{r['unidad']}]" for r in elegidos]
        if self.var_crudo.get():
            cab += [f"crudo reg {r['reg']}" for r in elegidos]
        dec = (lambda x: x.replace(".", ",")) if self.var_coma.get() else (lambda x: x)
        # utf-8-sig: Excel reconoce la codificación y muestra bien "°C" y "µg".
        with open(ruta, "w", newline="", encoding="utf-8-sig") as f:
            w = csv.writer(f, delimiter=";")
            w.writerow(cab)
            for i in indices:
                fila = [dec(f"{self.tiempos[i]:.3f}")]
                if self.var_hora.get():
                    fila.append(time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(self.horas[i]))
                                + f".{int(self.horas[i] * 1000) % 1000:03d}")
                if self.var_valor.get():
                    fila += ["" if r["valores"][i] is None else dec(_formato(r["valores"][i])) for r in elegidos]
                if self.var_crudo.get():
                    fila += ["" if r["crudos"][i] is None else r["crudos"][i] for r in elegidos]
                w.writerow(fila)


# ---------------------------------------------------------------------------
# Ventana principal
# ---------------------------------------------------------------------------

class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("ModbusEmpire - Graficador de registros")
        icono = Path(__file__).resolve().parent / "graficador.ico"
        if icono.exists():
            self.iconbitmap(default=str(icono))
        self.geometry("1200x760")
        self.minsize(900, 560)

        self.enlace = Enlace()
        self.conectado = False
        self.firmware_ok = False
        self.muestreando = False
        self.regs = []          # dicts: reg, nombre, tipo, mult, unidad, graficar, color, crudos, valores
        self.tiempos = []       # segundos desde la primera muestra
        self.horas = []         # hora del PC al recibir cada muestra (epoch s)
        self.t_primera_ms = None
        self.errores = 0
        self.durs = []
        self.span = 60.0
        self._inicio_manual = 0.0
        self._redibujo_pendiente = False
        self.sensores = self._cargar_sensores()
        self.meta_sensor = {}   # reg -> nombre/tipo/mult/unidad del sensor elegido en el JSON

        self.var_puerto = tk.StringVar()
        self.var_sensor = tk.StringVar(value="(manual)")
        self.var_id = tk.StringVar(value="1")
        self.var_baud = tk.StringVar(value="9600")
        self.var_fn = tk.StringVar(value="3")
        self.var_regs = tk.StringVar(value="0-19")
        self.var_ms = tk.StringVar(value="1000")
        self.var_hz = tk.StringVar(value="1")
        self.var_span = tk.StringVar(value="60")
        self.var_vivo = tk.BooleanVar(value=True)
        self.var_paneles = tk.BooleanVar(value=True)
        self.var_crudo = tk.BooleanVar(value=False)
        self.var_y_auto = tk.BooleanVar(value=True)
        self.var_y_min = tk.StringVar(value="0")
        self.var_y_max = tk.StringVar(value="100")
        self.var_estado = tk.StringVar(value="Desconectado")
        self.var_maximo = tk.StringVar(value="")

        self._armar_ui()
        self._refrescar_puertos()
        self.aplicar_lista()
        self.protocol("WM_DELETE_WINDOW", self._cerrar)
        self.report_callback_exception = self._error_inesperado
        self.after(50, self._atender_cola)

    # --- construcción de la interfaz ---
    def _armar_ui(self):
        arriba = ttk.Frame(self, padding=(8, 6))
        arriba.pack(fill="x")
        ttk.Label(arriba, text="Puerto").pack(side="left")
        self.combo_puerto = ttk.Combobox(arriba, textvariable=self.var_puerto, width=28, state="readonly")
        self.combo_puerto.pack(side="left", padx=4)
        ttk.Button(arriba, text="↻", width=3, command=self._refrescar_puertos).pack(side="left")
        self.btn_conectar = ttk.Button(arriba, text="Conectar", command=self._conectar_o_desconectar)
        self.btn_conectar.pack(side="left", padx=6)
        ttk.Label(arriba, textvariable=self.var_estado, foreground="#555").pack(side="left", padx=10)

        cuerpo = ttk.PanedWindow(self, orient="horizontal")
        cuerpo.pack(fill="both", expand=True)
        izq = ttk.Frame(cuerpo, padding=8)
        der = ttk.Frame(cuerpo, padding=(0, 8, 8, 8))
        cuerpo.add(izq, weight=0)
        cuerpo.add(der, weight=1)

        # Configuración del muestreo
        cfg = ttk.LabelFrame(izq, text="Muestreo", padding=6)
        cfg.pack(fill="x")
        fila = 0

        def campo(etiqueta, widget):
            nonlocal fila
            ttk.Label(cfg, text=etiqueta).grid(row=fila, column=0, sticky="w", pady=2)
            widget.grid(row=fila, column=1, sticky="ew", pady=2)
            fila += 1
            return widget

        nombres = ["(manual)"] + [s.get("nombre", "?") for s in self.sensores]
        cb = campo("Sensor (JSON)", ttk.Combobox(cfg, textvariable=self.var_sensor, values=nombres,
                                                  state="readonly", width=24))
        cb.bind("<<ComboboxSelected>>", lambda e: self._cargar_sensor())
        campo("ID esclavo", ttk.Entry(cfg, textvariable=self.var_id, width=8))
        campo("Baudrate", ttk.Combobox(cfg, textvariable=self.var_baud, values=[str(b) for b in BAUDS_BUS],
                                       width=10))
        campo("Función", ttk.Combobox(cfg, textvariable=self.var_fn, values=["3", "4"], state="readonly", width=4))
        e_regs = campo("Registros", ttk.Entry(cfg, textvariable=self.var_regs, width=26))
        e_regs.bind("<Return>", lambda e: self.aplicar_lista())
        ttk.Label(cfg, text="lista o intervalos: 0,4,8-10,0x13", foreground="#888").grid(
            row=fila, column=1, sticky="w")
        fila += 1
        e_ms = campo("Periodo (ms)", ttk.Entry(cfg, textvariable=self.var_ms, width=8))
        e_ms.bind("<KeyRelease>", lambda e: self._periodo_editado())
        e_hz = campo("Frecuencia (Hz)", ttk.Entry(cfg, textvariable=self.var_hz, width=8))
        e_hz.bind("<KeyRelease>", lambda e: self._frecuencia_editada())
        ttk.Label(cfg, text="0 ms = lo más rápido posible", foreground="#888").grid(
            row=fila, column=1, sticky="w")
        fila += 1
        cfg.columnconfigure(1, weight=1)

        botones = ttk.Frame(cfg)
        botones.grid(row=fila, column=0, columnspan=2, sticky="ew", pady=(6, 0))
        ttk.Button(botones, text="Aplicar lista", command=self.aplicar_lista).pack(side="left")
        self.btn_iniciar = ttk.Button(botones, text="▶ Iniciar", command=self._iniciar_o_detener)
        self.btn_iniciar.pack(side="right")
        fila += 1
        ttk.Label(cfg, textvariable=self.var_maximo, foreground="#1f5f9f", wraplength=290,
                  justify="left").grid(row=fila, column=0, columnspan=2, sticky="w", pady=(6, 0))

        # Tabla de registros
        tabla = ttk.LabelFrame(izq, text="Registros  (clic en ☑ = graficar · doble clic = editar)", padding=4)
        tabla.pack(fill="both", expand=True, pady=(8, 0))
        cols = ("g", "reg", "nombre", "tipo", "mult", "unidad", "crudo", "valor")
        self.tree = ttk.Treeview(tabla, columns=cols, show="headings", height=14, selectmode="browse")
        for c, t, w in (("g", "☑", 30), ("reg", "Reg", 50), ("nombre", "Nombre", 80), ("tipo", "Tipo", 42),
                        ("mult", "Mult", 52), ("unidad", "Unidad", 58), ("crudo", "Crudo", 56),
                        ("valor", "Valor", 66)):
            self.tree.heading(c, text=t)
            self.tree.column(c, width=w, anchor="center" if c in ("g", "tipo") else "w", stretch=c == "nombre")
        self.tree.pack(side="left", fill="both", expand=True)
        sb = ttk.Scrollbar(tabla, orient="vertical", command=self.tree.yview)
        sb.pack(side="right", fill="y")
        self.tree.configure(yscrollcommand=sb.set)
        self.tree.bind("<Button-1>", self._clic_tabla)
        self.tree.bind("<Double-1>", self._doble_clic_tabla)
        acciones = ttk.Frame(izq)
        acciones.pack(fill="x", pady=(4, 0))
        ttk.Button(acciones, text="Todos", command=lambda: self._marcar_todos(True)).pack(side="left")
        ttk.Button(acciones, text="Ninguno", command=lambda: self._marcar_todos(False)).pack(side="left", padx=4)
        ttk.Button(acciones, text="Exportar datos…", command=self._exportar_csv).pack(side="right")
        ttk.Button(acciones, text="Limpiar datos", command=self._limpiar_datos).pack(side="right", padx=4)

        # Controles del gráfico
        ctrl = ttk.Frame(der)
        ctrl.pack(fill="x")
        ttk.Checkbutton(ctrl, text="En vivo", variable=self.var_vivo, command=self.pedir_redibujo).pack(side="left")
        ttk.Label(ctrl, text="  Span (s)").pack(side="left")
        e_span = ttk.Entry(ctrl, textvariable=self.var_span, width=7)
        e_span.pack(side="left", padx=2)
        e_span.bind("<Return>", lambda e: self._fijar_span_desde_campo())
        for etiqueta, seg in (("10 s", 10), ("1 min", 60), ("5 min", 300), ("30 min", 1800)):
            ttk.Button(ctrl, text=etiqueta, width=6, command=lambda s=seg: self.fijar_span(s)).pack(side="left", padx=1)
        ttk.Button(ctrl, text="Todo", width=5, command=self._ver_todo).pack(side="left", padx=1)
        ttk.Button(ctrl, text="−", width=3, command=lambda: self.zoom(1.5, 1.0)).pack(side="left", padx=(8, 1))
        ttk.Button(ctrl, text="+", width=3, command=lambda: self.zoom(1 / 1.5, 1.0)).pack(side="left", padx=1)
        ttk.Checkbutton(ctrl, text="Paneles separados", variable=self.var_paneles,
                        command=self.pedir_redibujo).pack(side="left", padx=(12, 0))
        ttk.Checkbutton(ctrl, text="Ver crudo", variable=self.var_crudo,
                        command=self.pedir_redibujo).pack(side="left", padx=(12, 0))
        ttk.Checkbutton(ctrl, text="Y auto", variable=self.var_y_auto, command=self.pedir_redibujo).pack(
            side="left", padx=(12, 2))
        ttk.Entry(ctrl, textvariable=self.var_y_min, width=7).pack(side="left")
        ttk.Label(ctrl, text="a").pack(side="left", padx=2)
        ttk.Entry(ctrl, textvariable=self.var_y_max, width=7).pack(side="left")

        self.grafico = Grafico(der, self)
        self.grafico.pack(fill="both", expand=True, pady=(4, 0))
        self.barra = ttk.Scrollbar(der, orient="horizontal", command=self._scroll)
        self.barra.pack(fill="x")
        ttk.Label(der, text="Arrastrar = mover en el tiempo · rueda = zoom del span · el cursor muestra los valores",
                  foreground="#888").pack(anchor="w")

    # --- sensores del JSON ---
    def _cargar_sensores(self):
        try:
            datos = json.loads(RUTA_SENSORES.read_text(encoding="utf-8"))
            return datos if isinstance(datos, list) else []
        except (OSError, ValueError):
            return []

    def _cargar_sensor(self):
        nombre = self.var_sensor.get()
        sensor = next((s for s in self.sensores if s.get("nombre") == nombre), None)
        if sensor is None:
            # Manual: los registros quedan crudos (sin nombre ni escala del sensor).
            self.meta_sensor = {}
            self.aplicar_lista(olvidar=True)
            return
        variables = sensor.get("variables", [])
        regs = []
        meta = {}
        for v in variables:
            tipo = v.get("tipo") or ("u32" if v.get("size", 1) == 2 else "u16")
            if tipo not in ("u16", "s16"):
                continue   # 32 bits: el graficador trabaja por registro de 16 bits
            r = int(v.get("reg", 0))
            regs.append(r)
            meta[r] = dict(nombre=v.get("etiqueta", f"r{r}"), tipo=tipo, mult=float(v.get("mult", 1)),
                           unidad=v.get("uni", ""))
            if int(v.get("fn", 3)) in (3, 4):
                self.var_fn.set(str(int(v.get("fn", 3))))
        if not regs:
            messagebox.showinfo("Sensor", "Este sensor no tiene variables de 16 bits para graficar.")
            return
        self.meta_sensor = meta
        self.var_regs.set(",".join(str(r) for r in regs))
        self.aplicar_lista(olvidar=True)

    # --- tabla ---
    def aplicar_lista(self, olvidar=False):
        """Arma la tabla con la lista escrita. Un registro del sensor elegido toma
        su nombre y escala; uno que no es del sensor queda crudo (mult 1), así se
        puede mirar cualquier registro aunque haya un sensor cargado."""
        if self.muestreando:
            messagebox.showwarning("Registros", "Detenga el muestreo antes de cambiar la lista.")
            return
        try:
            regs = parsear_lista(self.var_regs.get())
        except ValueError as e:
            messagebox.showerror("Registros", f"Lista invalida: {e}")
            return
        anteriores = {} if olvidar else {r["reg"]: r for r in self.regs}
        self.regs = []
        for i, r in enumerate(regs):
            base = anteriores.get(r) or self.meta_sensor.get(r) or {}
            self.regs.append(dict(reg=r, nombre=base.get("nombre", f"r{r}"), tipo=base.get("tipo", "u16"),
                                  mult=base.get("mult", 1.0), unidad=base.get("unidad", ""),
                                  graficar=base.get("graficar", True), color=COLORES[i % len(COLORES)],
                                  crudos=[], valores=[]))
        self._limpiar_datos()
        self._rellenar_tabla()
        self._actualizar_maximo()

    def _rellenar_tabla(self):
        self.tree.delete(*self.tree.get_children())
        for i, r in enumerate(self.regs):
            self.tree.insert("", "end", iid=str(i), values=self._fila(r), tags=(f"c{i}",))
            self.tree.tag_configure(f"c{i}", foreground=r["color"])

    def _fila(self, r):
        valor = r["valores"][-1] if r["valores"] else None
        crudo = r["crudos"][-1] if r["crudos"] else None
        return ("☑" if r["graficar"] else "☐", f"{r['reg']} (0x{r['reg']:04X})", r["nombre"], r["tipo"],
                _formato(r["mult"]), r["unidad"], "" if crudo is None else str(crudo),
                _formato(valor) if valor is not None else "")

    def _clic_tabla(self, e):
        if self.tree.identify_region(e.x, e.y) != "cell" or self.tree.identify_column(e.x) != "#1":
            return
        fila = self.tree.identify_row(e.y)
        if fila:
            r = self.regs[int(fila)]
            r["graficar"] = not r["graficar"]
            self.tree.item(fila, values=self._fila(r))
            self.pedir_redibujo()
            return "break"

    def _doble_clic_tabla(self, e):
        fila = self.tree.identify_row(e.y)
        col = self.tree.identify_column(e.x)
        claves = {"#3": "nombre", "#4": "tipo", "#5": "mult", "#6": "unidad"}
        if not fila or col not in claves:
            return
        r = self.regs[int(fila)]
        clave = claves[col]
        if clave == "tipo":
            r["tipo"] = "s16" if r["tipo"] == "u16" else "u16"
            self._recalcular(r)
            self.tree.item(fila, values=self._fila(r))
            self.pedir_redibujo()
            return
        x, y, w, h = self.tree.bbox(fila, col)
        editor = ttk.Entry(self.tree)
        editor.insert(0, str(r[clave]) if clave != "mult" else _formato(r["mult"]))
        editor.place(x=x, y=y, width=max(w, 80), height=h)
        editor.focus_set()
        editor.select_range(0, "end")

        def guardar(_e=None):
            texto = editor.get().strip()
            editor.destroy()
            if clave == "mult":
                try:
                    r["mult"] = float(texto.replace(",", "."))
                except ValueError:
                    return
                self._recalcular(r)
            else:
                r[clave] = texto
            self.tree.item(fila, values=self._fila(r))
            self.pedir_redibujo()

        editor.bind("<Return>", guardar)
        editor.bind("<FocusOut>", guardar)
        editor.bind("<Escape>", lambda _e: editor.destroy())

    def _marcar_todos(self, valor):
        for i, r in enumerate(self.regs):
            r["graficar"] = valor
            self.tree.item(str(i), values=self._fila(r))
        self.pedir_redibujo()

    @staticmethod
    def _escalar(r, crudo):
        if crudo is None:
            return None
        if r["tipo"] == "s16" and crudo >= 0x8000:
            crudo -= 0x10000
        return crudo * r["mult"]

    def _recalcular(self, r):
        r["valores"] = [self._escalar(r, c) for c in r["crudos"]]

    def series_visibles(self):
        visibles = [r for r in self.regs if r["graficar"]]
        if not self.var_crudo.get():
            return visibles
        # Crudo: el valor del registro tal cual llega (u16 o s16), sin multiplicador.
        return [dict(r, valores=[None if c is None else (c - 0x10000 if r["tipo"] == "s16" and c >= 0x8000 else c)
                                 for c in r["crudos"]],
                     unidad="crudo") for r in visibles]

    # --- periodo y frecuencia: se edita uno y el otro se recalcula ---
    def _periodo_editado(self):
        try:
            ms = float(self.var_ms.get().replace(",", "."))
        except ValueError:
            return
        self.var_hz.set("máx" if ms <= 0 else _formato(round(1000.0 / ms, 3)))
        self._actualizar_maximo()

    def _frecuencia_editada(self):
        texto = self.var_hz.get().replace(",", ".").strip().lower()
        if texto in ("máx", "max"):
            self.var_ms.set("0")
        else:
            try:
                hz = float(texto)
            except ValueError:
                return
            if hz <= 0:
                return
            self.var_ms.set(str(int(round(1000.0 / hz))))
        self._actualizar_maximo()

    # --- estimación de la frecuencia máxima ---
    def _actualizar_maximo(self):
        try:
            baud = int(self.var_baud.get())
            regs = [r["reg"] for r in self.regs]
            cable, con_lat, bloques = estimar_ms_muestra(regs, baud)
        except (ValueError, ZeroDivisionError):
            self.var_maximo.set("")
            return
        texto = (f"{len(regs)} registros en {bloques} trama(s) a {baud} baud.\n"
                 f"Solo cable: {cable:.0f} ms/muestra → máx {1000 / cable:.1f} Hz.\n"
                 f"Con ~{LATENCIA_TIPICA_MS} ms de respuesta del sensor por trama: "
                 f"{con_lat:.0f} ms → ≈{1000 / con_lat:.1f} Hz.")
        if self.durs:
            med = sorted(self.durs[-50:])[len(self.durs[-50:]) // 2]
            # El firmware deja el silencio RTU entre una muestra y la siguiente.
            ciclo = med + ms_silencio(baud)
            texto += f"\nMedido: {med} ms/muestra + {ms_silencio(baud)} ms de silencio → máx real ≈{1000 / ciclo:.1f} Hz."
        try:
            periodo = float(self.var_ms.get().replace(",", "."))
            if periodo < con_lat:
                texto += "\nEl periodo pedido es menor: se muestreará lo más rápido posible."
        except ValueError:
            pass
        actualiza = self._intervalo_de_actualizacion()
        if actualiza is None:
            texto += "\nSi el sensor actualiza más lento, verá escalones (valores repetidos)."
        else:
            texto += (f"\nEl sensor cambia sus valores cada ~{_formato(actualiza)} s: muestrear a más de "
                      f"{_formato(round(1 / actualiza, 2))} Hz solo repite valores.")
        self.var_maximo.set(texto)

    def _intervalo_de_actualizacion(self):
        """Cada cuánto renueva el sensor sus valores (s), visto en los datos: el
        menor tiempo entre dos cambios de un mismo registro, en el registro que
        más cambia. None mientras no haya cambios suficientes."""
        if len(self.tiempos) < 20:
            return None
        mejor = None
        for r in self.regs:
            c = r["crudos"]
            cambios = [self.tiempos[i] for i in range(1, len(c))
                       if c[i] is not None and c[i - 1] is not None and c[i] != c[i - 1]]
            if len(cambios) < 4:
                continue
            difs = sorted(b - a for a, b in zip(cambios, cambios[1:]))
            # Percentil 10: un cambio suelto muy seguido (ruido de reloj) no manda.
            d = difs[len(difs) // 10]
            if d > 0 and (mejor is None or d < mejor):
                mejor = d
        return None if mejor is None else round(mejor, 1)

    # --- conexión ---
    def _refrescar_puertos(self):
        puertos = [f"{p.device} - {p.description}" for p in serial.tools.list_ports.comports()]
        self.combo_puerto["values"] = puertos
        if puertos and not self.var_puerto.get():
            self.var_puerto.set(puertos[0])

    def _conectar_o_desconectar(self):
        if self.conectado:
            self._desconectar()
            return
        nombre = self.var_puerto.get().split(" - ")[0]
        if not nombre:
            messagebox.showerror("Puerto", "Elija un puerto.")
            return
        try:
            self.enlace.abrir(nombre)
        except serial.SerialException as e:
            messagebox.showerror("Puerto", f"No se pudo abrir {nombre}:\n{e}\n\n"
                                           "¿Hay otro monitor serie abierto en ese puerto?")
            return
        self.conectado = True
        self.firmware_ok = False
        self.btn_conectar.configure(text="Desconectar")
        self.var_estado.set(f"Conectado a {nombre}, esperando a la ESP...")
        # @stop corta un muestreo que haya quedado andando; la ESP vacía su
        # entrada al parar, así que @hola va un rato después.
        self.enlace.enviar("@stop")
        self.after(300, lambda: self.conectado and self.enlace.enviar("@hola"))
        self.after(1800, self._verificar_hola)

    def _verificar_hola(self):
        if self.conectado and not self.firmware_ok:
            self.var_estado.set("La ESP no respondió @hola: ¿firmware sin modo gráfico, o placa sin arrancar?")

    def _desconectar(self):
        if self.muestreando:
            self._detener()
        self.enlace.cerrar()
        self.conectado = False
        self.btn_conectar.configure(text="Conectar")
        self.var_estado.set("Desconectado")

    # --- muestreo ---
    def _iniciar_o_detener(self):
        if self.muestreando:
            self._detener()
        else:
            self._iniciar()

    def _iniciar(self):
        if not self.conectado:
            messagebox.showwarning("Muestreo", "Conecte primero la ESP.")
            return
        try:
            regs = parsear_lista(self.var_regs.get())
            id_ = int(self.var_id.get(), 0)
            baud = int(self.var_baud.get())
            periodo = int(round(float(self.var_ms.get().replace(",", "."))))
        except ValueError as e:
            messagebox.showerror("Muestreo", f"Parametro invalido: {e}")
            return
        if regs != [r["reg"] for r in self.regs]:
            self.aplicar_lista()
        self._limpiar_datos()
        self.muestreando = True
        self.btn_iniciar.configure(text="■ Detener")
        lista = ",".join(str(r) for r in regs)
        self.enlace.enviar(f"@leer id={id_} baud={baud} fn={self.var_fn.get()} regs={lista} ms={max(0, periodo)}")
        self.var_estado.set("Muestreando...")

    def _detener(self):
        self.enlace.enviar("@stop")
        self.muestreando = False
        self.btn_iniciar.configure(text="▶ Iniciar")

    def _limpiar_datos(self):
        self.tiempos = []
        self.horas = []
        self.t_primera_ms = None
        self.errores = 0
        self.durs = []
        for r in self.regs:
            r["crudos"] = []
            r["valores"] = []
        self.pedir_redibujo()

    # --- mensajes de la ESP ---
    def _atender_cola(self):
        hubo_datos = False
        try:
            while True:
                tipo, dato = self.enlace.cola.get_nowait()
                if tipo == "desconectado":
                    self._desconectar()
                    messagebox.showerror("Puerto", f"Se perdió la conexión: {dato}")
                    break
                hubo_datos |= self._procesar(dato)
        except queue.Empty:
            pass
        if hubo_datos:
            for i, r in enumerate(self.regs):
                self.tree.item(str(i), values=self._fila(r))
            self._actualizar_maximo()
            self.pedir_redibujo()
        self.after(50, self._atender_cola)

    def _procesar(self, linea):
        if not linea.startswith("@"):
            return False
        partes = linea.split()
        cmd = partes[0]
        if cmd == "@ok":
            self.firmware_ok = True
            self.var_estado.set(f"Conectado: {' '.join(partes[1:])}")
        elif cmd == "@d" and self.muestreando:
            try:
                t_ms, dur = int(partes[1]), int(partes[2])
                crudos = [int(x) for x in partes[3:]]
            except (ValueError, IndexError):
                return False
            if len(crudos) != len(self.regs):
                return False
            if self.t_primera_ms is None:
                self.t_primera_ms = t_ms
            self.tiempos.append((t_ms - self.t_primera_ms) / 1000.0)
            self.horas.append(time.time())
            self.durs.append(dur)
            for r, c in zip(self.regs, crudos):
                r["crudos"].append(c)
                r["valores"].append(self._escalar(r, c))
            self.var_estado.set(f"Muestreando: {len(self.tiempos)} muestras, {self.errores} errores")
            return True
        elif cmd == "@e" and self.muestreando:
            self.errores += 1
            try:
                t_ms = int(partes[1])
            except (ValueError, IndexError):
                return False
            if self.t_primera_ms is None:
                self.t_primera_ms = t_ms
            self.tiempos.append((t_ms - self.t_primera_ms) / 1000.0)
            self.horas.append(time.time())
            for r in self.regs:
                r["crudos"].append(None)
                r["valores"].append(None)
            nombre = partes[3] if len(partes) > 3 else ""
            self.var_estado.set(f"Muestreando: {len(self.tiempos)} muestras, {self.errores} errores (último: {nombre})")
            return True
        elif cmd in ("@error", "@aviso"):
            texto = " ".join(partes[1:])
            self.var_estado.set(f"ESP: {texto}")
            if cmd == "@error":
                self.muestreando = False
                self.btn_iniciar.configure(text="▶ Iniciar")
        elif cmd == "@fin":
            self.muestreando = False
            self.btn_iniciar.configure(text="▶ Iniciar")
        return False

    # --- vista del gráfico ---
    def vista_inicio(self):
        if self.var_vivo.get() and self.tiempos:
            return max(0.0, self.tiempos[-1] - self.span) if self.tiempos[-1] > self.span else 0.0
        return self._inicio_manual

    def mover_vista(self, inicio):
        fin = self.tiempos[-1] if self.tiempos else 0.0
        self._inicio_manual = min(max(0.0, inicio), max(0.0, fin - self.span * 0.1))
        self.var_vivo.set(False)
        self.pedir_redibujo()

    def fijar_span(self, segundos):
        centro_fin = self.vista_inicio() + self.span
        self.span = max(0.5, float(segundos))
        self.var_span.set(_formato(self.span))
        if not self.var_vivo.get():
            self._inicio_manual = max(0.0, centro_fin - self.span)
        self.pedir_redibujo()

    def _fijar_span_desde_campo(self):
        try:
            self.fijar_span(float(self.var_span.get().replace(",", ".")))
        except ValueError:
            self.var_span.set(_formato(self.span))

    def zoom(self, factor, frac):
        inicio = self.vista_inicio()
        t_ancla = inicio + frac * self.span
        self.span = max(0.5, self.span * factor)
        self.var_span.set(_formato(self.span))
        if not self.var_vivo.get() or frac < 1.0:
            self._inicio_manual = max(0.0, t_ancla - frac * self.span)
            if frac < 1.0:
                self.var_vivo.set(False)
        self.pedir_redibujo()

    def _ver_todo(self):
        if self.tiempos:
            self.span = max(0.5, self.tiempos[-1] * 1.02)
            self._inicio_manual = 0.0
            self.var_span.set(_formato(self.span))
        self.pedir_redibujo()

    def _scroll(self, *args):
        total = max(self.tiempos[-1] if self.tiempos else 0.0, self.span)
        if args[0] == "moveto":
            self.mover_vista(float(args[1]) * total)
        elif args[0] == "scroll":
            paso = self.span * (0.9 if args[2] == "pages" else 0.1)
            self.mover_vista(self.vista_inicio() + int(args[1]) * paso)

    def pedir_redibujo(self):
        if not self._redibujo_pendiente:
            self._redibujo_pendiente = True
            self.after(60, self._redibujar)

    def _redibujar(self):
        self._redibujo_pendiente = False
        self.grafico.dibujar()
        total = max(self.tiempos[-1] if self.tiempos else 0.0, self.span)
        ini = self.vista_inicio()
        self.barra.set(ini / total, min(1.0, (ini + self.span) / total))

    def _error_inesperado(self, tipo, valor, traza):
        import traceback
        detalle = "".join(traceback.format_exception(tipo, valor, traza))
        messagebox.showerror("Error inesperado", f"{valor}\n\n{detalle[-1500:]}")

    # --- exportar y cerrar ---
    def _exportar_csv(self):
        if not self.tiempos:
            messagebox.showinfo("Exportar", "No hay datos.")
            return
        DialogoExportar(self)

    def _cerrar(self):
        try:
            if self.conectado:
                self.enlace.enviar("@stop")
                time.sleep(0.1)
                self.enlace.cerrar()
        finally:
            self.destroy()


if __name__ == "__main__":
    App().mainloop()
