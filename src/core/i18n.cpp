#include "i18n.h"

#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace {

bool g_turkish = false;
bool g_explicit = false;

// İngilizce özgün metin → Türkçe karşılığı. Yeni bir T("…") eklendiğinde buraya da
// eklenmeli; tests/check_i18n.sh eksikleri bulur.
const std::unordered_map<std::string_view, const char *> &turkishTable()
{
    static const std::unordered_map<std::string_view, const char *> t{
#include "i18n_tr.inc"
    };
    return t;
}

bool envTurkish()
{
    for (const char *var : {"LANGUAGE", "LC_ALL", "LC_MESSAGES", "LANG"}) {
        const char *v = std::getenv(var);
        if (v && *v)
            return std::strncmp(v, "tr", 2) == 0;
    }
    return false;
}

}  // namespace

namespace i18n {

void init(const std::string &setting)
{
    g_explicit = setting == "en" || setting == "tr";
    g_turkish = setting == "tr" || (setting != "en" && envTurkish());
}

bool explicitChoice()
{
    return g_explicit;
}

void applyToEnvironment()
{
    if (g_explicit && g_turkish != envTurkish())
        setenv("LANGUAGE", g_turkish ? "tr" : "en", 1);
}

bool turkish()
{
    return g_turkish;
}

const char *code()
{
    return g_turkish ? "tr" : "en";
}

char decimalPoint()
{
    return g_turkish ? ',' : '.';
}

char thousandsSep()
{
    return g_turkish ? '.' : ',';
}

}  // namespace i18n

const char *T(const char *en)
{
    if (!g_turkish)
        return en;
    auto it = turkishTable().find(en);
    return it != turkishTable().end() ? it->second : en;
}

std::string TF(const char *en, std::initializer_list<std::string_view> args)
{
    const char *f = T(en);
    std::string out;
    for (const char *p = f; *p; ++p) {
        if (*p == '%' && p[1] >= '1' && p[1] <= '9') {
            const size_t i = size_t(p[1] - '1');
            if (i < args.size())
                out += *(args.begin() + i);
            ++p;
        } else {
            out += *p;
        }
    }
    return out;
}
