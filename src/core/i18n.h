#pragma once
// Arayüz dili. Kaynaktaki metinler İngilizcedir; T() seçili dilde karşılığını döndürür.
// Dil "system" ise LANGUAGE / LC_ALL / LC_MESSAGES / LANG'a bakılır (tr* → Türkçe).
//
// Qt'nin QObject::tr() üye işleviyle karışmasın diye ad T'dir.

#include <initializer_list>
#include <string>
#include <string_view>

namespace i18n {
void init(const std::string &setting);  // "system", "en" ya da "tr"
bool turkish();
const char *code();  // "en" ya da "tr"
char decimalPoint();
char thousandsSep();
// Dil açıkça seçilmişse ve ortamdakinden farklıysa LANGUAGE'ı ona göre ayarlar; böylece
// araç takımlarının kendi metinleri ve MIME açıklamaları da aynı dilde gelir.
void applyToEnvironment();
bool explicitChoice();
}

// Çevrilecek metni yalnızca işaretler (tablolardaki sabitler için); T() ile çevrilir.
#define N_(s) s

// Çevrilmiş metin; tablo dışındaysa İngilizce özgün metin.
const char *T(const char *en);

// %1 … %9 yer tutucularını sırayla doldurur: TF("%1 files", {fmtInt(n)}).
std::string TF(const char *en, std::initializer_list<std::string_view> args);
