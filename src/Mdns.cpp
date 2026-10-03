//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

#include "Mdns.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <string>
#include <type_traits>
#include <vector>

#include "Diagnostics.hpp"

#if defined(__APPLE__)
#    include <arpa/inet.h>
#    include <dns_sd.h>
#elif defined(__linux__)
#    include <dlfcn.h>
#elif defined(_WIN32)
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windns.h>
#    include <windows.h>
#endif

WAGGLE_NAMESPACE_BEGIN

#if defined(__APPLE__)

// Bonjour, through dns_sd.h in the system library.
struct MdnsAdvertisement::Backend {
    DNSServiceRef ref = nullptr;

    explicit Backend(Service const &service) {
        TXTRecordRef txt;
        TXTRecordCreate(&txt, 0, nullptr);
        for (auto const &[key, value] : service.txt) {
            TXTRecordSetValue(&txt, key.c_str(), static_cast<uint8_t>(std::min<size_t>(value.size(), 255)), value.data());
        }
        DNSServiceErrorType const err =
            DNSServiceRegister(&ref, 0, 0, service.name.c_str(), service.type.c_str(), nullptr, nullptr, htons(service.port),
                               TXTRecordGetLength(&txt), TXTRecordGetBytesPtr(&txt), nullptr, nullptr);
        TXTRecordDeallocate(&txt);
        if (err != kDNSServiceErr_NoError) {
            diagnostic(DiagnosticLevel::Warning, fmt::format("Profile server: mDNS registration failed (error {})", static_cast<int>(err)));
            ref = nullptr;
            return;
        }
        diagnostic(DiagnosticLevel::Info,
                   fmt::format("Profile server: registered mDNS service '{}' on port {}", service.name, service.port));
    }

    ~Backend() {
        if (ref != nullptr) {
            DNSServiceRefDeallocate(ref);
        }
    }
};

#elif defined(__linux__)

// Avahi's client library, opened at run time: the daemon and its library are on most desktops and
// few cluster nodes, and a profiler must not make either a requirement. The declarations are the
// few Avahi's ABI (soname 3) fixes; every type passed through is opaque here.
namespace {
namespace avahi {
struct Poll;
struct ThreadedPoll;
struct Client;
struct EntryGroup;
struct StringList;

constexpr int kIfUnspec         = -1;
constexpr int kProtoUnspec      = -1;
constexpr int kClientRunning    = 2; // AVAHI_CLIENT_S_RUNNING
constexpr int kClientCollision  = 3; // AVAHI_CLIENT_S_COLLISION
constexpr int kClientFailure    = 100;
constexpr int kGroupEstablished = 2; // AVAHI_ENTRY_GROUP_ESTABLISHED
constexpr int kGroupCollision   = 3;
constexpr int kGroupFailure     = 4;

using ClientCallback = void (*)(Client *, int state, void *userdata);
using GroupCallback  = void (*)(EntryGroup *, int state, void *userdata);

struct Api {
    void *client_lib = nullptr;
    void *common_lib = nullptr;

    ThreadedPoll *(*threaded_poll_new)()                          = nullptr;
    Poll const *(*threaded_poll_get)(ThreadedPoll *)              = nullptr;
    int (*threaded_poll_start)(ThreadedPoll *)                    = nullptr;
    int (*threaded_poll_stop)(ThreadedPoll *)                     = nullptr;
    void (*threaded_poll_free)(ThreadedPoll *)                    = nullptr;
    char const *(*strerror)(int)                                  = nullptr;
    StringList *(*string_list_new_from_array)(char const **, int) = nullptr;
    void (*string_list_free)(StringList *)                        = nullptr;

    Client *(*client_new)(Poll const *, int flags, ClientCallback, void *userdata, int *error)                  = nullptr;
    void (*client_free)(Client *)                                                                               = nullptr;
    int (*client_errno)(Client *)                                                                               = nullptr;
    EntryGroup *(*entry_group_new)(Client *, GroupCallback, void *userdata)                                     = nullptr;
    int (*entry_group_add_service_strlst)(EntryGroup *, int interface, int protocol, int flags, char const *name, char const *type,
                                          char const *domain, char const *host, uint16_t port, StringList *txt) = nullptr;
    int (*entry_group_commit)(EntryGroup *)                                                                     = nullptr;

    /// Opens the libraries and finds every function; false, with nothing held, if any is missing.
    bool load() {
        client_lib = dlopen("libavahi-client.so.3", RTLD_NOW | RTLD_LOCAL);
        common_lib = dlopen("libavahi-common.so.3", RTLD_NOW | RTLD_LOCAL);
        bool ok    = client_lib != nullptr && common_lib != nullptr;
        auto find  = [&](void *lib, auto &fn, char const *name) {
            if (ok) {
                fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
                ok = fn != nullptr;
            }
        };
        find(common_lib, threaded_poll_new, "avahi_threaded_poll_new");
        find(common_lib, threaded_poll_get, "avahi_threaded_poll_get");
        find(common_lib, threaded_poll_start, "avahi_threaded_poll_start");
        find(common_lib, threaded_poll_stop, "avahi_threaded_poll_stop");
        find(common_lib, threaded_poll_free, "avahi_threaded_poll_free");
        find(common_lib, strerror, "avahi_strerror");
        find(common_lib, string_list_new_from_array, "avahi_string_list_new_from_array");
        find(common_lib, string_list_free, "avahi_string_list_free");
        find(client_lib, client_new, "avahi_client_new");
        find(client_lib, client_free, "avahi_client_free");
        find(client_lib, client_errno, "avahi_client_errno");
        find(client_lib, entry_group_new, "avahi_entry_group_new");
        find(client_lib, entry_group_add_service_strlst, "avahi_entry_group_add_service_strlst");
        find(client_lib, entry_group_commit, "avahi_entry_group_commit");
        if (!ok) {
            unload();
        }
        return ok;
    }

    void unload() {
        if (client_lib != nullptr) {
            dlclose(client_lib);
        }
        if (common_lib != nullptr) {
            dlclose(common_lib);
        }
        client_lib = common_lib = nullptr;
    }
};
} // namespace avahi
} // namespace

struct MdnsAdvertisement::Backend {
    avahi::Api           api;
    Service              service;
    avahi::ThreadedPoll *poll   = nullptr;
    avahi::Client       *client = nullptr;
    bool                 added  = false; // touched only by Avahi's callbacks, which run one at a time

    explicit Backend(Service s) : service(std::move(s)) {
        if (!api.load()) {
            diagnostic(DiagnosticLevel::Debug, "Profile server: not advertised over mDNS (Avahi's client library is not installed)");
            return;
        }
        poll = api.threaded_poll_new();
        if (poll == nullptr) {
            diagnostic(DiagnosticLevel::Warning, "Profile server: mDNS registration failed (Avahi could not start its event loop)");
            return;
        }
        int error = 0;
        // The first state can arrive inside client_new, before the client is returned: the
        // callback works from the client it is handed.
        client = api.client_new(api.threaded_poll_get(poll), 0, &Backend::on_client, this, &error);
        if (client == nullptr) {
            diagnostic(DiagnosticLevel::Info, fmt::format("Profile server: not advertised over mDNS ({})", api.strerror(error)));
            return;
        }
        if (api.threaded_poll_start(poll) < 0) {
            diagnostic(DiagnosticLevel::Warning, "Profile server: mDNS registration failed (Avahi's event loop did not start)");
        }
    }

    ~Backend() {
        if (poll != nullptr) {
            api.threaded_poll_stop(poll); // joins the loop's thread; no callback runs after it
        }
        if (client != nullptr) {
            api.client_free(client); // frees the entry group, which withdraws the service
        }
        if (poll != nullptr) {
            api.threaded_poll_free(poll);
        }
        api.unload();
    }

    static void on_client(avahi::Client *c, int state, void *userdata) {
        auto *self = static_cast<Backend *>(userdata);
        if (state == avahi::kClientRunning && !self->added) {
            self->added = true;
            self->add(c);
        } else if (state == avahi::kClientFailure || state == avahi::kClientCollision) {
            diagnostic(DiagnosticLevel::Warning,
                       fmt::format("Profile server: mDNS advertisement lost ({})", self->api.strerror(self->api.client_errno(c))));
        }
    }

    static void on_group(avahi::EntryGroup *, int state, void *userdata) {
        auto const *self = static_cast<Backend const *>(userdata);
        if (state == avahi::kGroupEstablished) {
            diagnostic(DiagnosticLevel::Info,
                       fmt::format("Profile server: registered mDNS service '{}' on port {}", self->service.name, self->service.port));
        } else if (state == avahi::kGroupCollision || state == avahi::kGroupFailure) {
            diagnostic(DiagnosticLevel::Warning, fmt::format("Profile server: mDNS registration of '{}' failed", self->service.name));
        }
    }

    void add(avahi::Client *c) {
        avahi::EntryGroup *group = api.entry_group_new(c, &Backend::on_group, this);
        if (group == nullptr) {
            diagnostic(DiagnosticLevel::Warning,
                       fmt::format("Profile server: mDNS registration failed ({})", api.strerror(api.client_errno(c))));
            return;
        }
        std::vector<std::string> entries;
        entries.reserve(service.txt.size());
        for (auto const &[key, value] : service.txt) {
            entries.push_back(key + "=" + value);
        }
        std::vector<char const *> pointers;
        pointers.reserve(entries.size());
        for (auto const &e : entries) {
            pointers.push_back(e.c_str());
        }
        avahi::StringList *txt = api.string_list_new_from_array(pointers.data(), static_cast<int>(pointers.size()));
        int const          err = api.entry_group_add_service_strlst(group, avahi::kIfUnspec, avahi::kProtoUnspec, 0, service.name.c_str(),
                                                                    service.type.c_str(), nullptr, nullptr, service.port, txt);
        api.string_list_free(txt);
        if (err < 0 || api.entry_group_commit(group) < 0) {
            diagnostic(DiagnosticLevel::Warning,
                       fmt::format("Profile server: mDNS registration failed ({})", api.strerror(err < 0 ? err : api.client_errno(c))));
        }
        // The group belongs to the client, which frees it.
    }
};

#elif defined(_WIN32)

// The system's DNS-SD service (Windows 10 1809 and later). The functions are found in dnsapi.dll
// when the server starts: imported, they would stop waggle.dll loading on an older Windows.
namespace {
std::wstring widen(std::string const &text) {
    if (text.empty()) {
        return {};
    }
    int const    size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}
} // namespace

struct MdnsAdvertisement::Backend {
    using ConstructFn  = PDNS_SERVICE_INSTANCE(WINAPI *)(PCWSTR, PCWSTR, PIP4_ADDRESS, PIP6_ADDRESS, WORD, WORD, WORD, DWORD, PCWSTR *,
                                                         PCWSTR *);
    using RegisterFn   = DWORD(WINAPI *)(PDNS_SERVICE_REGISTER_REQUEST, PDNS_SERVICE_CANCEL);
    using DeregisterFn = DWORD(WINAPI *)(PDNS_SERVICE_REGISTER_REQUEST, PDNS_SERVICE_CANCEL);
    using FreeFn       = VOID(WINAPI *)(PDNS_SERVICE_INSTANCE);

    HMODULE      dll           = nullptr;
    ConstructFn  construct     = nullptr;
    RegisterFn   register_fn   = nullptr;
    DeregisterFn deregister    = nullptr;
    FreeFn       free_instance = nullptr;

    std::string                  name;
    uint16_t                     port     = 0;
    PDNS_SERVICE_INSTANCE        instance = nullptr;
    DNS_SERVICE_REGISTER_REQUEST request{};
    bool                         registered = false;
    /// Signaled by each completion: the deregistration's is awaited before the request goes away.
    HANDLE done = nullptr;

    explicit Backend(Service const &service) : name(service.name), port(service.port) {
        dll = LoadLibraryW(L"dnsapi.dll");
        if (dll != nullptr) {
            construct     = reinterpret_cast<ConstructFn>(GetProcAddress(dll, "DnsServiceConstructInstance"));
            register_fn   = reinterpret_cast<RegisterFn>(GetProcAddress(dll, "DnsServiceRegister"));
            deregister    = reinterpret_cast<DeregisterFn>(GetProcAddress(dll, "DnsServiceDeRegister"));
            free_instance = reinterpret_cast<FreeFn>(GetProcAddress(dll, "DnsServiceFreeInstance"));
        }
        if (construct == nullptr || register_fn == nullptr || deregister == nullptr || free_instance == nullptr) {
            diagnostic(DiagnosticLevel::Debug, "Profile server: not advertised over mDNS (this Windows has no DNS-SD service)");
            return;
        }

        wchar_t host[256];
        DWORD   host_size = 256;
        if (GetComputerNameExW(ComputerNameDnsHostname, host, &host_size) == 0) {
            diagnostic(DiagnosticLevel::Warning, "Profile server: mDNS registration failed (no host name)");
            return;
        }
        std::wstring const        instance_name = widen(service.name + "." + service.type + ".local");
        std::wstring const        host_name     = std::wstring(host, host_size) + L".local";
        std::vector<std::wstring> keys, values;
        for (auto const &[key, value] : service.txt) {
            keys.push_back(widen(key));
            values.push_back(widen(value));
        }
        std::vector<PCWSTR> key_ptrs, value_ptrs;
        for (size_t i = 0; i < keys.size(); ++i) {
            key_ptrs.push_back(keys[i].c_str());
            value_ptrs.push_back(values[i].c_str());
        }
        // No addresses: the system answers for its own host name.
        instance = construct(instance_name.c_str(), host_name.c_str(), nullptr, nullptr, port, 0, 0, static_cast<DWORD>(keys.size()),
                             key_ptrs.data(), value_ptrs.data());
        done     = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (instance == nullptr || done == nullptr) {
            diagnostic(DiagnosticLevel::Warning, "Profile server: mDNS registration failed (could not describe the service)");
            return;
        }
        request.Version                     = DNS_QUERY_REQUEST_VERSION1;
        request.InterfaceIndex              = 0;
        request.pServiceInstance            = instance;
        request.pRegisterCompletionCallback = &Backend::on_complete;
        request.pQueryContext               = this;
        request.unicastEnabled              = FALSE;
        DWORD const status                  = register_fn(&request, nullptr);
        if (status != DNS_REQUEST_PENDING) {
            diagnostic(DiagnosticLevel::Warning, fmt::format("Profile server: mDNS registration failed (error {})", status));
            return;
        }
        registered = true;
    }

    ~Backend() {
        if (registered && deregister(&request, nullptr) == DNS_REQUEST_PENDING) {
            WaitForSingleObject(done, 2000);
        }
        if (instance != nullptr) {
            free_instance(instance);
        }
        if (done != nullptr) {
            CloseHandle(done);
        }
        // dnsapi.dll stays loaded: a completion the wait gave up on may still be running in it.
    }

    static VOID WINAPI on_complete(DWORD status, PVOID context, PDNS_SERVICE_INSTANCE result) {
        auto *self = static_cast<Backend *>(context);
        if (result != nullptr) {
            self->free_instance(result);
        }
        if (status == ERROR_SUCCESS) {
            diagnostic(DiagnosticLevel::Info, fmt::format("Profile server: mDNS service '{}' on port {} updated", self->name, self->port));
        } else {
            diagnostic(DiagnosticLevel::Warning, fmt::format("Profile server: mDNS registration failed (error {})", status));
        }
        SetEvent(self->done);
    }
};

#else

struct MdnsAdvertisement::Backend {
    explicit Backend(Service const &) {}
};

#endif

MdnsAdvertisement::MdnsAdvertisement(Service service) : _backend(std::make_unique<Backend>(std::move(service))) {
}

MdnsAdvertisement::~MdnsAdvertisement() = default;

WAGGLE_NAMESPACE_END
