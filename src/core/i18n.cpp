#include "i18n.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace {

struct Pair {
    const char *en, *tr;
};
struct Catalog {
    const char *code, *name;
    char decimal, thousands;
    const char *date;
    const Pair *pairs;
    size_t count;
};

#include "i18n_catalog.inc"  // generated from po/*.po

const Catalog ENGLISH{"en", "English", '.', ',', "%Y-%m-%d %H:%M", nullptr, 0};
const Catalog *g_cat = &ENGLISH;
bool g_explicit = false;
std::unordered_map<std::string_view, const char *> g_table;

const Catalog *findCatalog(std::string_view code)
{
    for (const Catalog &c : CATALOGS)
        if (c.count && code == c.code)
            return &c;
    return nullptr;
}

// Language from the environment: the first variable that is set wins; LANGUAGE may
// list several languages ("tr:en").
const Catalog *envCatalog()
{
    for (const char *var : {"LANGUAGE", "LC_ALL", "LC_MESSAGES", "LANG"}) {
        const char *v = std::getenv(var);
        if (!v || !*v)
            continue;
        std::string_view list(v);
        while (!list.empty()) {
            std::string_view item = list.substr(0, list.find(':'));
            list.remove_prefix(std::min(list.size(), item.size() + 1));
            if (item == "C" || item == "POSIX" || item.rfind("en", 0) == 0)
                return &ENGLISH;
            const std::string_view lang = item.substr(0, item.find_first_of("_.@"));
            if (const Catalog *c = findCatalog(item.substr(0, item.find_first_of(".@"))))
                return c;
            if (const Catalog *c = findCatalog(lang))
                return c;
        }
        return &ENGLISH;
    }
    return &ENGLISH;
}

}  // namespace

namespace i18n {

void init(const std::string &setting)
{
    const Catalog *c = setting == "en" ? &ENGLISH : findCatalog(setting);
    g_explicit = c != nullptr;
    g_cat = c ? c : envCatalog();
    g_table.clear();
    for (size_t i = 0; i < g_cat->count; ++i)
        g_table.emplace(g_cat->pairs[i].en, g_cat->pairs[i].tr);
}

bool explicitChoice()
{
    return g_explicit;
}

void applyToEnvironment()
{
    if (g_explicit && envCatalog() != g_cat)
        setenv("LANGUAGE", g_cat->code, 1);
}

bool turkish()
{
    return std::strcmp(g_cat->code, "tr") == 0;
}

const char *code()
{
    return g_cat->code;
}

std::vector<Language> languages()
{
    std::vector<Language> out{{"en", "English"}};
    for (const Catalog &c : CATALOGS)
        if (c.count)
            out.push_back({c.code, c.name});
    return out;
}

bool known(const std::string &code)
{
    return code == "en" || findCatalog(code);
}

char decimalPoint()
{
    return g_cat->decimal;
}

char thousandsSep()
{
    return g_cat->thousands;
}

const char *dateFormat()
{
    return g_cat->date;
}

}  // namespace i18n

const char *T(const char *en)
{
    if (g_table.empty())
        return en;
    auto it = g_table.find(en);
    return it != g_table.end() ? it->second : en;
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
