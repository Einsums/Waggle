..
    Copyright (c) The Einsums Developers. All rights reserved.
    Licensed under the MIT License. See LICENSE.txt in the project root for license information.

================
Profiling C code
================

The collector's interface is plain C, ``<Waggle/Waggle.h>``; the C++ layer is a thin wrapper over it.
Any language that can call C functions (Fortran through ``bind(C)``, Rust, Julia) can use the same interface.

A zone is a *site*, registered once, and a begin and end around the code:

.. code-block:: c

    #include <Waggle/Waggle.h>
    #include <string.h>

    static uint32_t solve_site;
    static uint32_t key_n;

    void mylib_init(void) {
        uint32_t const domain = waggle_register_domain("mylib", 5);
        solve_site            = waggle_register_site("solve", 5, __FILE__, __LINE__, __func__, domain);
        key_n                 = waggle_intern("n", 1);
    }

    void solve(int n) {
        if (waggle_zone_begin(solve_site, 0)) {
            waggle_annotate_i64(key_n, n);
            /* ... */
            waggle_zone_end();
        }
    }

Rules
=====

* Close a zone only when ``waggle_zone_begin`` returned 1.
  Recording can be switched off and on while a zone is open; a begin that opened nothing must not be followed by an end, or it would close the zone around it.
* Register sites once, at start-up or in a static, not on every call: registration takes a lock, and ``waggle_zone_begin`` with a registered site takes none.
* Strings are passed with their lengths and need not be terminated, except where a function says so (a site's ``file`` and ``func``).
* The second argument of ``waggle_zone_begin`` names the zone at run time (an id from ``waggle_intern``); 0 uses the site's own name.

Domains
=======

``waggle_register_domain`` names your library.
To make the switch for a domain cheap to check before calling in, read its address once with ``waggle_domain_flag`` and test it directly; it is nonzero while both the global switch and the domain's are on.
``waggle_zone_begin`` itself checks only the global switch, so code calling the C interface directly checks its domain's switch itself.

Version
=======

``waggle_abi_version`` reports the interface's version.
The library's soname changes with the major version; minor versions only add functions.

Next
====

:doc:`/reference/api/c/index` lists every C function with its contract.
