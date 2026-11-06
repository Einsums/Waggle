//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

// waggle._core: apiary generates the bindings for python/src/Core.hpp and the register header this
// includes; this file only names the module. The register header defines APIARY_EXTENSION_MODULE
// for the binding library APIARY_BACKEND chose, so the file is the same under pybind11 and nanobind.

#include <waggle/Modules.hpp>

APIARY_EXTENSION_MODULE(_core, m) {
    m.doc() = "The Waggle profiler for Python code, over its C interface.";
    waggle_register_all(m);
}
