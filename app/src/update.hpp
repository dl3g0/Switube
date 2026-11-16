#pragma once

#include <cstdint>
#include <functional>
#include <string>

// Auto-actualizador contra GitHub Releases (Switube).
//
// El .nro no se puede sobrescribir mientras está en ejecución porque hbloader
// lo mantiene abierto y romfs lee recursos de él bajo demanda. Por eso la
// descarga se guarda junto al binario como "<nombre>.nro.new", y applyPending()
// realiza el intercambio al salir, una vez desmontado romfs.
namespace switube::update {

struct Release {
  bool ok = false;       // la comprobación finalizó correctamente
  bool newer = false;    // ok, y la versión es más nueva que APP_VERSION
  std::string version;   // "0.2.0" (sin la 'v' inicial del tag)
  std::string title;     // nombre del release en GitHub
  std::string notes;     // cuerpo markdown del release
  std::string url;       // URL de descarga del asset (.zip o .nro)
  int64_t size = 0;      // tamaño en bytes (0 si no se indicó)
  bool isZip = false;    // true si el asset es un paquete .zip (ej. Switube-Complete-SD.zip)
  std::string error;     // mensaje de error si !ok

  bool installable() const { return newer && !url.empty(); }
};

// Registra la ruta del .nro actual desde argv[0]. Llamar una sola vez en main().
void init(int argc, char** argv);

// Consulta el último release en GitHub en segundo plano y llama al callback en el hilo UI.
void checkAsync(std::function<void(Release)> done);

// Obtiene las notas de versión para un tag específico (y parches anteriores de la misma rama menor).
void fetchNotesAsync(
    const std::string& version,
    std::function<void(bool ok, std::string title, std::string notes, std::string error)> done);

// Muestra una página scrollable con las notas de la versión.
void showNotes(const std::string& title, const std::string& notes);

// Muestra un spinner no cancelable mientras carga el changelog y luego abre ChangelogActivity.
void showChangelog(const std::string& version);
void showChangelog(const Release& r);

// Descarga el .nro del release a "<selfNro>.new".
void downloadAsync(const Release& r,
                   std::function<void(float)> progress,
                   std::function<void(std::string)> done);

// Muestra el diálogo de actualización disponible, descarga con barra de progreso y ofrece reiniciar.
void promptInstall(const Release& r);

// Programa el relanzamiento mediante envSetNextLoad y cierra la aplicación.
bool restartNow();

// Reemplaza el .nro actual con el .nro.new descargado. Llamar ÚNICAMENTE al final de main().
void applyPending();

// True si hay una descarga lista para aplicarse al salir.
bool hasPending();

// Ruta del .nro en ejecución, o "" si no se pudo determinar.
std::string selfPath();

} // namespace switube::update
