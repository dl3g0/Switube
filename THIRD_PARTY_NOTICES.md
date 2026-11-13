# Third-Party Notices & Credits

**Switube** es un proyecto independiente y no oficial para Nintendo Switch. YouTube Music y Nintendo son marcas registradas de sus respectivos propietarios.

## Créditos Especiales

- **Switify** (<https://github.com/DomazinUS/Switify>) — Créditos por el diseño de la interfaz de usuario (UI) en Borealis y la arquitectura visual del overlay.
- **BitChord** — Créditos por la referencia e inspiración para la integración de búsqueda, filtros y exploración de YouTube Music en Nintendo Switch.

## Componentes y Librerías de Terceros

- **Borealis** — Apache-2.0. Utilizado para la interfaz gráfica principal con soporte de mando y pantalla táctil (`vendor/borealis/`). Upstream: <https://github.com/natinusala/borealis>
- **libultrahand / libtesla** — GPL-2.0-only y licencias subordinadas. Utilizado para el overlay de Ultrahand / Tesla (`phase2/overlay/lib/libultrahand/`). Upstream: <https://github.com/ppkantorski/libultrahand>
- **nlohmann/json** — MIT License. Utilizado para el parseo de respuestas JSON de InnerTube (`vendor/bell/external/nlohmann_json/`).
- **FFmpeg (`libavformat`, `libavcodec`, `libswresample`, `libavutil`)**, **libcurl**, **mbedTLS** y **libnx** — Provistos a través de `devkitPro` portlibs bajo sus respectivas licencias.
- El servidor ligero CMIF en `phase2/sysmodule/source/ipc_server.c` y `.h` está basado en el trabajo utilizado por `sys-clk` y `sys-tune` y conserva su aviso de licencia Beer-Ware.
