//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

// waggle._core: apiary generates the bindings for python/src/Core.hpp and the register header this
// includes; this file only names the module.

#include <waggle/Modules.hpp>

#include <pybind11/pybind11.h>

PYBIND11_MODULE(_core, m) {
    m.doc() = "The Waggle profiler for Python code, over its C interface.";
    waggle_register_all(m);
}
