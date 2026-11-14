# 🚀 Switube v0.1.0 — Lanzamiento Inicial

**Switube** es un cliente nativo de **YouTube Music** y motor de reproducción en segundo plano para **Nintendo Switch**, diseñado para funcionar sin necesidad de cuenta Premium ni emparejamiento con dispositivos externos.

Esta versión inicial **v0.1.0** incluye tanto la aplicación principal a pantalla completa (**`Switube.nro`**) como el paquete de reproducción en segundo plano para **Ultrahand / Tesla** (**`Switube.ovl` + `exefs.nsp`**).

---

## ✨ Novedades de la Versión 0.1.0

### 🖥️ Aplicación Principal (`Switube.nro`)
- **Interfaz Nativa en Borealis:** Navegación fluida con mando (D-Pad y Sticks) y soporte completo para pantalla táctil.
- **Inicio Dinámico Multi-Carrusel:** Carga automática de secciones personalizadas y globales de YouTube Music (*Selección rápida*, *Clásicos inolvidables*, *Esenciales de todos los tiempos*, *Nuevos lanzamientos*, *Canciones del momento para ti*, *Playlists de la comunidad*, *Álbumes y sencillos nuevos*, *Tendencias* y *Videos musicales nuevos*).
- **Búsqueda Avanzada con Filtros:** Búsqueda en tiempo real con chips de filtro por categoría (**Todos**, **Canciones**, **Videos**, **Álbumes**, **Artistas** y **Listas de reproducción**) y paginación continua (*Cargar más*).
- **Perfiles de Artista, Álbumes, Sencillos y Playlists:**
  - Vista de artista con foto de cabecera, suscriptores, canciones populares, discografía (álbumes/sencillos) y artistas relacionados.
  - Vista completa para álbumes, sencillos (*Singles / EPs*) y listas de reproducción con reproducción de cola completa.
- **Modo Offline y Gestor de Descargas (`.opus`):**
  - Descarga canciones y videos directamente desde cualquier lista (botón **`X`** al enfocar una pista), desde la barra lateral o desde la vista *En Reproducción*.
  - Almacenamiento local en `sdmc:/config/Switube/downloads/` con reproducción instantánea sin conexión a Internet.
- **Reproductor Completo y Mini-Player:**
  - Controles de transporte completos (*Play/Pausa*, *Anterior*, *Siguiente*, *Aleatorio / Shuffle*, *Repetir Todo / Una*), barra de progreso interactiva (±10 s) y control de volumen.
  - Audio nativo en alta calidad **Opus / AAC a 48 kHz** mediante `audout`.
- **Soporte Multilenguaje:** Detección automática del idioma de la consola (**Español** / **Inglés**) y cambio en caliente desde la pestaña *Ajustes*.

---

### 🎮 Overlay y Motor en Segundo Plano (`Switube.ovl` + `exefs.nsp`)
- **Música Mientras Juegas:** Escucha tus canciones descargadas o transmite directamente desde YouTube Music sin salir de tus juegos.
- **Sysmodule Ultraligero (`0x4200000000535755`):**
  - Huella de memoria reducida (~10 MB) con compilación minimalista de **FFmpeg** (demuxers `webm/matroska`, `mp4/mov`, `ogg` y decodificadores `opus` y `aac`) para total compatibilidad con los juegos de Nintendo Switch.
  - Adquisición diferida de `audout`: el servicio no reserva recursos de audio hasta que inicia la reproducción.
- **Overlay Interactivo para Ultrahand / Tesla:**
  - **Vista *En Reproducción*:** Navegación por zonas con D-Pad, Sticks o pantalla táctil sobre la barra de tiempo (±10 s), los 5 botones de transporte (*Repetir*, *Anterior*, *Play/Pausa*, *Siguiente*, *Aleatorio*) y la barra de volumen (pasos de 5%).
  - **Descargas Offline:** Reproduce al instante cualquier canción descargada previamente desde la aplicación principal.
  - **Búsqueda en Línea:** Teclado integrado en el overlay para buscar y reproducir pistas de YouTube Music en cualquier momento.

---

## 📦 Archivos de la Release e Instalación

Descarga **`Switube-Complete-SD.zip`** y extrae su contenido directamente en la **raíz de tu tarjeta microSD**:

| Ruta en la microSD | Descripción |
| :--- | :--- |
| `/switch/Switube/Switube.nro` | Aplicación principal a pantalla completa |
| `/switch/.overlays/Switube.ovl` | Overlay para Ultrahand / Tesla |
| `/atmosphere/contents/4200000000535755/exefs.nsp` | Sysmodule de audio en segundo plano |
| `/atmosphere/contents/4200000000535755/toolbox.json` | Configuración para Hekate Toolbox / Kosmos |

> **Recomendación:** Inicia la aplicación principal (`Switube.nro`) usando **Title Override** (manteniendo presionado **`R`** al abrir cualquier juego hasta entrar al *Homebrew Menu*) para disponer de toda la memoria RAM del sistema.

---

## 🙌 Créditos

- **[Switify](https://github.com/DomazinUS/Switify)** (*HiroshiYamauchi*) — Por el diseño de la interfaz de usuario e infraestructura visual en Borealis y el overlay.
- **BitChord** — Por la inspiración y referencia técnica en la integración con YouTube Music (InnerTube) en Nintendo Switch.
- **[Borealis](https://github.com/natinusala/borealis)** & **[libultrahand](https://github.com/ppkantorski/libultrahand) / libtesla** — Frameworks de interfaz gráfica y overlay para Nintendo Switch.
