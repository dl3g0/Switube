#include "platform_controls.hpp"
// Switube — Native YouTube Music client for Nintendo Switch.

#include <borealis.hpp>

#ifdef __SWITCH__
#include <switch.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "NXConsoleLog.h"
#include <curl/curl.h>
#include "yt_engine.hpp"
#include "download_manager.hpp"
#include "tabs.hpp"
#include "mini_player.hpp"
#include "playback_shortcuts.hpp"
#include "ui_helpers.hpp"
#include "i18n.hpp"
#include "update.hpp"

class MainActivity : public brls::Activity {
 public:
  MainActivity() : alive(std::make_shared<std::atomic_bool>(true)) {}
  ~MainActivity() {
    alive->store(false);
    switube::I18n::getInstance().removeObserver(i18nObserverId);
  }

  CONTENT_FROM_XML_RES("activity/main.xml");

  BRLS_BIND(brls::TabFrame, tabFrame, "tabFrame");

  void onContentAvailable() override {
    registerPlaybackShortcuts(this);
    updateSidebarLabels();

    if (auto* frame = getContentView()) {
      frame->setBackgroundColor(nvgRGB(7, 7, 10));
      if (auto* footer = frame->getView("brls/applet_frame/footer")) {
        footer->setBackgroundColor(nvgRGB(7, 7, 10));
      }
    }
    if (tabFrame) {
      if (auto* sidebar = dynamic_cast<brls::Sidebar*>(tabFrame->getView("brls/tab_frame/sidebar"))) {
        sidebar->setBackgroundColor(nvgRGBA(16, 10, 14, 115));
        sidebar->setCornerRadius(16);
        sidebar->setMargins(14, 0, 6, 18);
        sidebar->setBorderThickness(1.0f);
        sidebar->setBorderColor(nvgRGBA(255, 255, 255, 20));
      }
    }

    auto token = alive;
    i18nObserverId = switube::I18n::getInstance().addObserver([this, token]() {
      brls::sync([this, token]() {
        if (token->load()) {
          updateSidebarLabels();
        }
      });
    });
  }

  void updateSidebarLabels() {
    if (!tabFrame) return;
    auto* sidebar = dynamic_cast<brls::Sidebar*>(tabFrame->getView("brls/tab_frame/sidebar"));
    if (!sidebar) return;
    sidebar->setHeader("Switube");
    if (auto* it = sidebar->getItem(0)) it->setLabel(switube::tr("tab.home"));
    if (auto* it = sidebar->getItem(1)) it->setLabel(switube::tr("tab.search"));
    if (auto* it = sidebar->getItem(2)) it->setLabel(switube::tr("tab.library"));
    if (auto* it = sidebar->getItem(3)) it->setLabel(switube::tr("tab.downloads"));
    if (auto* it = sidebar->getItem(4)) it->setLabel(switube::tr("tab.now_playing"));
    if (auto* it = sidebar->getItem(5)) it->setLabel(switube::tr("tab.settings"));
  }

 private:
  std::shared_ptr<std::atomic_bool> alive;
  size_t i18nObserverId = 0;
};

int main(int argc, char* argv[]) {
  brls::Logger::setLogLevel(brls::LogLevel::LOG_DEBUG);

#ifdef __SWITCH__
  mkdir("/config", 0755);
  mkdir("/config/Switube", 0755);
  nxlog::setLogFile("/config/Switube/app.log");
  nxlog::push("Switube 0.1.0 starting...");
  brls::FontLoader::USER_EMOJI_PATH = "romfs:/fonts/OpenMoji.ttf";
#endif

  curl_global_init(CURL_GLOBAL_ALL);
  switube::update::init(argc, argv);

  // Cargar configuración de idioma antes de iniciar Borealis
  switube::I18n::getInstance().init();
  brls::Platform::APP_LOCALE_DEFAULT = (switube::I18n::getInstance().getLanguage() == switube::Language::Spanish)
      ? brls::LOCALE_ES
      : brls::LOCALE_EN_US;

  nxlog::push("Initializing Borealis...");
  if (!brls::Application::init()) {
    nxlog::push("Failed to initialize Borealis!");
    return EXIT_FAILURE;
  }

  nxlog::push("Creating window...");
  brls::Application::createWindow("Switube");

  // Mantener la pantalla encendida (permite atenuar brillo por inactividad pero nunca apaga la pantalla)
  swplatform::preventSleep(true);
  brls::Application::getPlatform()->setThemeVariant(brls::ThemeVariant::DARK);
  brls::Application::setGlobalQuit(false);

  // Tema unificado Switube (obsidiana + carmesí YouTube Music, sin barras grises ni foco cian)
  auto theme = brls::Application::getTheme();
  theme.addColor("brls/background", nvgRGB(7, 7, 10));
  theme.addColor("brls/accent", nvgRGB(255, 45, 60));
  theme.addColor("brls/highlight/background", nvgRGB(24, 18, 22));
  theme.addColor("brls/highlight/color1", nvgRGB(255, 45, 60));
  theme.addColor("brls/highlight/color2", nvgRGB(255, 118, 130));
  theme.addColor("brls/applet_frame/separator", nvgRGBA(255, 255, 255, 16));
  theme.addColor("brls/sidebar/background", nvgRGBA(0, 0, 0, 0));
  theme.addColor("brls/sidebar/active_item", nvgRGB(255, 45, 60));
  theme.addColor("brls/sidebar/separator", nvgRGBA(0, 0, 0, 0));
  theme.addColor("brls/dialog/backdrop", nvgRGBA(4, 4, 8, 190));

  auto style = brls::Application::getStyle();
  style.addMetric("brls/tab_frame/sidebar_width", 280.0f);
  style.addMetric("brls/sidebar/padding_left", 32.0f);
  style.addMetric("brls/sidebar/padding_right", 16.0f);
  style.addMetric("brls/sidebar/item_height", 58.0f);

  nxlog::push("Registering XML views...");
  brls::Application::registerXMLView("GradientBox", swui::GradientBox::create);
  brls::Application::registerXMLView("HomeTab", HomeTab::create);
  brls::Application::registerXMLView("SearchTab", SearchTab::create);
  brls::Application::registerXMLView("LibraryTab", LibraryTab::create);
  brls::Application::registerXMLView("DownloadsTab", DownloadsTab::create);
  brls::Application::registerXMLView("NowPlayingTab", NowPlayingTab::create);
  brls::Application::registerXMLView("SettingsTab", SettingsTab::create);
  brls::Application::registerXMLView("MiniPlayer", MiniPlayer::create);

  nxlog::push("Initializing YtEngine...");
  switube::YtEngine::getInstance().init();
  nxlog::push("Initializing DownloadManager...");
  switube::DownloadManager::getInstance().init();

  nxlog::push("Pushing MainActivity...");
  auto* activity = new MainActivity();
  brls::Application::pushActivity(activity);

  // Comprobación silenciosa de actualizaciones al iniciar
  switube::update::checkAsync([](switube::update::Release r) {
    if (r.ok && r.installable()) {
      switube::update::promptInstall(r);
    }
  });

  nxlog::push("Entering mainLoop...");
  while (brls::Application::mainLoop()) {
    nxlog::drain();
  }

  nxlog::push("Exiting mainLoop...");
  switube::YtEngine::getInstance().shutdown();
  swui::shutdownRemoteImages();
  swplatform::preventSleep(false);
  switube::update::applyPending();

  nxlog::push("Switube terminated cleanly.");
  return EXIT_SUCCESS;
}
