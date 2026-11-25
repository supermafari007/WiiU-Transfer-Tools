#pragma once

#include <coreinit/userconfig.h>
#include <sys/stat.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace wutlang {

enum class Lang : uint8_t {
    ES = 0,
    EN = 1
};

inline Lang gLang = Lang::ES;

constexpr const char* kLangFilePath = "fs:/vol/external01/wiiu/apps/.wiiu_transfer_stage/lang.txt";

inline bool IsEnglish() {
    return gLang == Lang::EN;
}

inline const char* Tr(const char* es, const char* en) {
    return (gLang == Lang::EN) ? en : es;
}

inline Lang DetectSystemLanguage() {
    UCHandle h = UCOpen();
    if (h < 0) return Lang::ES;

    alignas(0x40) uint32_t sysLang = 5; // 5 = Spanish
    alignas(0x40) UCSysConfig cfg{};
    std::strncpy(cfg.name, "cafe.language", sizeof(cfg.name) - 1);
    cfg.access = 0x777;
    cfg.dataType = UC_DATATYPE_UNSIGNED_INT;
    cfg.error = UC_ERROR_OK;
    cfg.dataSize = sizeof(sysLang);
    cfg.data = &sysLang;

    UCError err = UCReadSysConfig(h, 1, &cfg);
    UCClose(h);

    if (err == UC_ERROR_OK && cfg.error == UC_ERROR_OK) {
        // Wii U cafe.language: 0=JA, 1=EN, 2=FR, 3=DE, 4=IT, 5=ES, ...
        return (sysLang == 5) ? Lang::ES : Lang::EN;
    }
    return Lang::ES;
}

inline void SaveLanguagePref() {
    mkdir("fs:/vol/external01/wiiu", 0777);
    mkdir("fs:/vol/external01/wiiu/apps", 0777);
    mkdir("fs:/vol/external01/wiiu/apps/.wiiu_transfer_stage", 0777);
    FILE* fp = std::fopen(kLangFilePath, "wb");
    if (!fp) return;
    const char* code = (gLang == Lang::EN) ? "en\n" : "es\n";
    std::fwrite(code, 1, std::strlen(code), fp);
    std::fclose(fp);
}

inline void InitLanguage() {
    FILE* fp = std::fopen(kLangFilePath, "rb");
    if (fp) {
        char buf[16]{};
        size_t n = std::fread(buf, 1, sizeof(buf) - 1, fp);
        std::fclose(fp);
        if (n >= 2) {
            if (buf[0] == 'e' && buf[1] == 'n') {
                gLang = Lang::EN;
                return;
            }
            if (buf[0] == 'e' && buf[1] == 's') {
                gLang = Lang::ES;
                return;
            }
        }
    }
    gLang = DetectSystemLanguage();
}

inline void ToggleLanguage() {
    gLang = (gLang == Lang::ES) ? Lang::EN : Lang::ES;
    SaveLanguagePref();
}

} // namespace wutlang
