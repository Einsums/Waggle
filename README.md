# Waggle

A low-overhead tracing profiler for C and C++ libraries.
Every library in a process records into one shared collector, so their zones form a single tree, and a terminal viewer shows it live.

Waggle came out of [Einsums](https://github.com/Einsums/Einsums), whose profiler it was, so that Einsums, Nectar and other libraries can profile one program together.

## Instrumenting a library

```cpp
#include <Waggle/Waggle.hpp>

namespace mylib {
WAGGLE_DEFINE_DOMAIN("mylib")

void solve(int n) {
    WAGGLE_ZONE("solve n={}", n);
    WAGGLE_ANNOTATE("method", "cg");
    // ...
}
} // namespace mylib
```

- `WAGGLE_ZONE` times the rest of its scope.
  A formatted name is cached per call site and value, so a repeated name costs no formatting.
- `WAGGLE_DEFINE_DOMAIN` names the library a zone belongs to.
  It works by name lookup from where the zone is written, so a zone in a header belongs to its library wherever it is compiled.
- `WAGGLE_ANNOTATE` attaches a value to the open zone, and `WAGGLE_MEM_ALLOC` and `WAGGLE_MEM_FREE` record memory against it.
- `WAGGLE_DISABLE`, defined before the header, compiles a library's zones out without changing anything else.

Link `Waggle::waggle` from CMake:

```cmake
find_package(Waggle REQUIRED)
target_link_libraries(mylib PRIVATE Waggle::waggle)
```

### From C

The collector's interface is plain C (`<Waggle/Waggle.h>`), which the C++ layer above wraps.
C code registers a call site once and pairs each begin with an end:

```c
#include <Waggle/Waggle.h>

static uint32_t site;

void solve(int n) {
    if (waggle_zone_begin(site, 0)) {
        waggle_annotate_i64(waggle_intern("n", 1), n);
        /* ... */
        waggle_zone_end();
    }
}

void mylib_init(void) {
    site = waggle_register_site("solve", 5, __FILE__, __LINE__, __func__, waggle_register_domain("mylib", 5));
}
```

Close a zone only when `waggle_zone_begin` returned 1: recording may be switched on or off while a zone is open.
Any language that can call C functions can use the same interface.

### From Python

```python
import waggle

SOLVE = waggle.Zone("solve")      # the site is registered once

def solve(n):
    with SOLVE:
        waggle.annotate("n", n)
        ...

@waggle.profile
def build_fock(density):
    ...

print(waggle.snapshot().find("solve").call_count)
```

`waggle._core`, a compiled module over the C interface, does the recording; build it with `-DWAGGLE_BUILD_PYTHON=ON`, which needs [apiary](https://github.com/Einsums/Apiary) and pybind11.
Python zones belong to the domain `python` unless given another.

## One collector per process

The collector is a shared library behind a C interface (`<Waggle/Waggle.h>`), so libraries built against different minor versions share it.
A second copy loaded into the same process, as a Python extension bundling its own would be, finds the first, switches itself off and says so.
The active collector then records which libraries' zones are missing.

## Settings

Libraries configure the collector, and the first explicit setting wins.
The environment fills in what no library set.
The report and the session file are written when the last library calls `waggle_finalize`, or at exit for a program that never does.
At exit, a program that never opened a zone writes no report.

| Variable | Meaning |
| --- | --- |
| `WAGGLE_DISABLE` | record nothing at run time (the macro of the same name compiles zones out) |
| `WAGGLE_REPORT`, `WAGGLE_REPORT_FILE` | write a text report at the end |
| `WAGGLE_REPORT_APPEND`, `WAGGLE_REPORT_DETAILED` | append to the report file; add min, max and mean |
| `WAGGLE_SERVER`, `WAGGLE_PORT` | serve live data to the viewer (default port 19216) |
| `WAGGLE_SAVE` | save the session as JSON |
| `WAGGLE_WAIT_FOR_VIEWER` | hold the program until a viewer connects |
| `WAGGLE_MAX_DISTINCT_CHILDREN` | names a zone keeps before the rest fold into "(other)" |

## The viewer

```sh
waggle                    # attach to a program run with WAGGLE_SERVER=1
waggle --load run.json    # open a saved session
waggle report run.json    # hotspots or the tree, as text, CSV or JSON
waggle diff a.json b.json # compare two sessions
```

The viewer needs [Textual](https://textual.textualize.io); `report` and `diff` do not.
Libraries add panels and actions through `waggle.plugin`, and `waggle.testing` has a fake server for testing them.

## Building

```sh
conda env create -f environment.yml
conda activate waggle-dev
cmake -S . -B build -GNinja
cmake --build build
ctest --test-dir build
```

Waggle builds and is tested on Linux, macOS and Windows.
On macOS the viewer finds running programs over mDNS; on Linux and Windows it connects by host and port.

## License

MIT; see [LICENSE.txt](LICENSE.txt).
