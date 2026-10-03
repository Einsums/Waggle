//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

#include "Symbols.hpp"

#include <fmt/format.h>

#include <string>
#include <string_view>

#include "Sources.hpp"

#ifdef _WIN32
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#else
#    include <cstdlib>
#    include <cxxabi.h>
#    include <dlfcn.h>
#endif

#ifdef __linux__
#    include <algorithm>
#    include <cstring>
#    include <elf.h>
#    include <fcntl.h>
#    include <link.h>
#    include <map>
#    include <memory>
#    include <mutex>
#    include <sys/mman.h>
#    include <sys/stat.h>
#    include <unistd.h>
#    include <vector>
#endif

WAGGLE_NAMESPACE_BEGIN

namespace {

auto module_and_offset(std::string_view module, void const *address, void const *base) -> std::string {
    module = module.substr(module.find_last_of("\\/") + 1);
    return fmt::format("{}+{:#x}", module.empty() ? "?" : module, static_cast<char const *>(address) - static_cast<char const *>(base));
}

#ifndef _WIN32
auto readable(char const *mangled) -> std::string {
    int               status    = 0;
    char             *demangled = abi::__cxa_demangle(mangled, nullptr, nullptr, &status);
    std::string const name      = status == 0 && demangled != nullptr ? demangled : mangled;
    std::free(demangled); // NOLINT(cppcoreguidelines-no-malloc)
    return short_function_name(name);
}
#endif

#ifdef __linux__
/// One module's functions, by address relative to where it is loaded.
class ElfFunctions {
  public:
    explicit ElfFunctions(char const *path) {
        int const fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return;
        }
        struct stat st{};
        void       *map = MAP_FAILED;
        if (fstat(fd, &st) == 0 && st.st_size > static_cast<off_t>(sizeof(ElfW(Ehdr)))) {
            map = mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
        }
        close(fd);
        if (map == MAP_FAILED) {
            return;
        }
        read(static_cast<unsigned char const *>(map), static_cast<size_t>(st.st_size));
        munmap(map, static_cast<size_t>(st.st_size));
    }

    /// The function at @p offset, mangled; empty if none covers it.
    [[nodiscard]] auto at(uintptr_t offset) const -> std::string const * {
        auto it =
            std::upper_bound(_functions.begin(), _functions.end(), offset, [](uintptr_t o, Function const &f) { return o < f.start; });
        if (it == _functions.begin()) {
            return nullptr;
        }
        --it;
        return offset < it->start + it->size ? &it->name : nullptr;
    }

    /// Whether symbol values are addresses (a fixed-position executable) rather than offsets.
    [[nodiscard]] auto absolute() const -> bool { return _absolute; }

  private:
    struct Function {
        uintptr_t   start;
        uintptr_t   size;
        std::string name;
    };
    std::vector<Function> _functions;
    bool                  _absolute{false};

    void read(unsigned char const *data, size_t size) {
        auto const *ehdr = reinterpret_cast<ElfW(Ehdr) const *>(data);
        if (std::memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0 || ehdr->e_shoff == 0 ||
            ehdr->e_shoff + ehdr->e_shnum * sizeof(ElfW(Shdr)) > size) {
            return;
        }
        _absolute         = ehdr->e_type == ET_EXEC;
        auto const *shdrs = reinterpret_cast<ElfW(Shdr) const *>(data + ehdr->e_shoff);
        // The full symbol table when the file has one; the dynamic one when it was stripped.
        for (auto const wanted : {SHT_SYMTAB, SHT_DYNSYM}) {
            for (size_t i = 0; i < ehdr->e_shnum; ++i) {
                ElfW(Shdr) const &sh = shdrs[i];
                if (sh.sh_type != wanted || sh.sh_link >= ehdr->e_shnum || sh.sh_entsize != sizeof(ElfW(Sym))) {
                    continue;
                }
                ElfW(Shdr) const &strings = shdrs[sh.sh_link];
                if (sh.sh_offset + sh.sh_size > size || strings.sh_offset + strings.sh_size > size) {
                    continue;
                }
                auto const *syms = reinterpret_cast<ElfW(Sym) const *>(data + sh.sh_offset);
                auto const *text = reinterpret_cast<char const *>(data + strings.sh_offset);
                // ELF64_ST_TYPE is ELF32_ST_TYPE too: both take the low four bits.
                for (size_t k = 0; k < sh.sh_size / sizeof(ElfW(Sym)); ++k) {
                    ElfW(Sym) const &sym = syms[k];
                    if (ELF64_ST_TYPE(sym.st_info) == STT_FUNC && sym.st_value != 0 && sym.st_size != 0 && sym.st_name < strings.sh_size) {
                        _functions.push_back({.start = sym.st_value, .size = sym.st_size, .name = text + sym.st_name});
                    }
                }
            }
            if (!_functions.empty()) {
                break;
            }
        }
        std::sort(_functions.begin(), _functions.end(), [](Function const &a, Function const &b) { return a.start < b.start; });
    }
};

auto elf_functions(char const *path) -> ElfFunctions const & {
    // Never destroyed: a runtime's callbacks can still name a function while the process exits,
    // after function-local statics are gone. Modules are never forgotten either, so a reference
    // returned here stays good without the lock.
    static auto *const     mutex   = new std::mutex;
    static auto *const     modules = new std::map<std::string, std::unique_ptr<ElfFunctions const>>;
    std::scoped_lock const lock(*mutex);
    auto                  &entry = (*modules)[path];
    if (entry == nullptr) {
        entry = std::make_unique<ElfFunctions const>(path);
    }
    return *entry;
}
#endif

} // namespace

auto function_at(void const *address) -> std::string {
#ifdef _WIN32
    HMODULE module = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(address), &module) != 0) {
        char path[MAX_PATH]; // NOLINT(modernize-avoid-c-arrays)
        if (GetModuleFileNameA(module, path, MAX_PATH) > 0) {
            return module_and_offset(path, address, module);
        }
    }
    return fmt::format("{}", address);
#else
    Dl_info info{};
    if (dladdr(address, &info) == 0) {
        return fmt::format("{}", address);
    }
#    ifdef __linux__
    if (info.dli_fname != nullptr && info.dli_fbase != nullptr) {
        ElfFunctions const &functions = elf_functions(info.dli_fname);
        auto const          where     = reinterpret_cast<uintptr_t>(address);
        auto const          offset    = functions.absolute() ? where : where - reinterpret_cast<uintptr_t>(info.dli_fbase);
        if (std::string const *name = functions.at(offset)) {
            return readable(name->c_str());
        }
    }
#    endif
    if (info.dli_sname != nullptr) {
        return readable(info.dli_sname);
    }
    return info.dli_fname != nullptr ? module_and_offset(info.dli_fname, address, info.dli_fbase) : fmt::format("{}", address);
#endif
}

WAGGLE_NAMESPACE_END
