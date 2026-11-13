# Building Switube

Este repositorio compila los dos componentes principales de **Switube** para Nintendo Switch desde un único árbol de código: la aplicación independiente `Switube.nro` y el paquete de segundo plano (`exefs.nsp` + `Switube.ovl`).

## Requisitos

- Windows PowerShell
- devkitPro instalado en `C:\devkitPro`
- Paquetes de devkitPro: `switch-dev`, `switch-cmake`, `switch-curl`, `switch-ffmpeg`, `switch-mbedtls`, `switch-zlib`, `switch-bzip2`, `switch-dav1d`

## 1. Aplicación Independiente (`Switube.nro`)

Desde la raíz del repositorio:

```powershell
.\build-app.ps1
```

Salidas generadas:
- `build_app\Switube.nro`
- `build_app\Switube.elf`

## 2. Sysmodule y Overlay (`exefs.nsp` + `Switube.ovl`)

Desde la raíz del repositorio:

```powershell
.\build-phase2.ps1
```

Salidas generadas:
- `phase2\build_engine\switube-background.nsp`
- `phase2\dist_engine\atmosphere\contents\4200000000535755\exefs.nsp`
- `phase2\dist_engine\switch\.overlays\Switube.ovl`

## 3. Empaquetado Completo para microSD

Para generar `Switube-Complete-SD.zip` con la estructura lista para extraer en la raíz de la microSD:

```powershell
.\package_sd.ps1
```
