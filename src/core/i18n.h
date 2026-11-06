#pragma once
// Interface language. Source strings are English; T() returns the translation in the
// chosen language. Translations live in po/<code>.po and are compiled into the program.
// With the setting "system", LANGUAGE / LC_ALL / LC_MESSAGES / LANG decide.
//
// Named T so it does not clash with Qt's QObject::tr().

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace i18n {
struct Language {
    std::string code;  // "en", "tr", …
    std::string name;  // in the language itself: "English", "Türkçe"
};

void init(const std::string &setting);  // "system" or a language code
bool turkish();
const char *code();                      // active language, "en" when untranslated
std::vector<Language> languages();       // English first, then the compiled translations
bool known(const std::string &code);     // English or a compiled translation
char decimalPoint();
char thousandsSep();
const char *dateFormat();  // strftime format for dates with time
// When the language was chosen explicitly and differs from the environment, sets
// LANGUAGE so toolkit strings and MIME descriptions come in the same language.
void applyToEnvironment();
bool explicitChoice();
}

// Only marks a string for translation (constants in tables); translate with T().
#define N_(s) s

// Translated text; the English original when there is no translation.
const char *T(const char *en);

// Fills %1 … %9 in order: TF("%1 files", {fmtInt(n)}).
std::string TF(const char *en, std::initializer_list<std::string_view> args);
