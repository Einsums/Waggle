//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

#pragma once

#include <Waggle/Config.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

WAGGLE_NAMESPACE_BEGIN

/// A DNS-SD service advertised over multicast DNS for as long as the object lives, so viewers on
/// the network find the server without being told its port.
///
/// Each platform's own responder does the advertising: Bonjour on macOS, the Avahi daemon on
/// Linux, and the system's DNS-SD service on Windows 10 1809 and later. Avahi and the Windows
/// functions are looked up when the advertisement is made, not linked, so a machine without them
/// runs the server unadvertised rather than failing to load the library.
class MdnsAdvertisement {
  public:
    struct Service {
        /// The instance name, unique on the host: "<executable>-<pid>".
        std::string name;
        /// The service type, "_waggle._tcp".
        std::string type;
        uint16_t    port = 0;
        /// TXT record entries, key and value.
        std::vector<std::pair<std::string, std::string>> txt;
    };

    /// Advertises @p service; failure is reported as a diagnostic and leaves nothing advertised.
    explicit MdnsAdvertisement(Service service);
    ~MdnsAdvertisement();

    MdnsAdvertisement(MdnsAdvertisement const &)            = delete;
    MdnsAdvertisement &operator=(MdnsAdvertisement const &) = delete;

    struct Backend;

  private:
    std::unique_ptr<Backend> _backend;
};

WAGGLE_NAMESPACE_END
