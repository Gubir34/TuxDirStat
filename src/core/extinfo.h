#pragma once
// Dosya türü açıklamaları. Arayüzler önce sistemin MIME veritabanına bakar; orada
// olmayan türler (oyun motorları, geliştirme araçları, önbellekler…) için bu tablo,
// o da yoksa genel bir açıklama kullanılır. Böylece hiçbir tür açıklamasız kalmaz.

#include <cstdint>
#include <string>

// Yerleşik tablodaki açıklama; bilinmiyorsa boş.
std::string builtinExtDescription(const std::string &ext);

// Her uzantı için bir açıklama üretir: "XYZ dosyası", sayısal uzantılar için sürüm/parça.
std::string genericExtDescription(const std::string &ext);

// Sistem açıklaması (boş olabilir) verildiğinde gösterilecek son açıklama.
// ext: ExtTable kimliği (0 = uzantısız).
std::string describeExt(uint32_t ext, const std::string &systemDescription = {});
