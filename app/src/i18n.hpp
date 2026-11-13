#pragma once

#include <string>
#include <functional>
#include <vector>

namespace switube {

enum class Language {
  Spanish = 0,
  English = 1
};

class I18n {
 public:
  static I18n& getInstance();

  void init();
  Language getLanguage() const { return currentLanguage_; }
  void setLanguage(Language lang);

  std::string get(const std::string& key) const;
  std::string format(const std::string& key, const std::string& arg1) const;
  std::string format(const std::string& key, const std::string& arg1, const std::string& arg2) const;

  std::string getHl() const { return currentLanguage_ == Language::Spanish ? "es" : "en"; }
  std::string getGl() const { return currentLanguage_ == Language::Spanish ? "ES" : "US"; }

  size_t addObserver(std::function<void()> observer);
  void removeObserver(size_t id);

 private:
  I18n();
  void loadConfig();
  void saveConfig();

  Language currentLanguage_ = Language::Spanish;
  std::vector<std::pair<size_t, std::function<void()>>> observers_;
  size_t nextObserverId_ = 1;
};

inline std::string tr(const std::string& key) {
  return I18n::getInstance().get(key);
}

inline std::string tr(const std::string& key, const std::string& arg1) {
  return I18n::getInstance().format(key, arg1);
}

inline std::string tr(const std::string& key, const std::string& arg1, const std::string& arg2) {
  return I18n::getInstance().format(key, arg1, arg2);
}

} // namespace switube
