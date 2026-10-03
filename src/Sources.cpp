//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

#include "Sources.hpp"

WAGGLE_NAMESPACE_BEGIN

auto source_requested(Settings const &settings, std::string_view name) -> bool {
    std::string_view rest = settings.sources;
    while (!rest.empty()) {
        auto const       comma = rest.find(',');
        std::string_view item  = rest.substr(0, comma);
        rest                   = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
        while (!item.empty() && item.front() == ' ') {
            item.remove_prefix(1);
        }
        while (!item.empty() && item.back() == ' ') {
            item.remove_suffix(1);
        }
        if (item == name) {
            return true;
        }
    }
    return false;
}

auto source_statuses() -> std::vector<SourceStatus> {
    return {ompt::status()};
}

WAGGLE_NAMESPACE_END
