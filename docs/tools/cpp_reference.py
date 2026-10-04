# Copyright (c) The Einsums Developers. All rights reserved.
# Licensed under the MIT License. See LICENSE.txt in the project root for license information.

"""Generate the C and C++ API references: parse Waggle's public headers with ``apiary
--emit-cpp-docs-json`` and render one page per entity with apiary's ``apiary_render_cpp_site.py``.

The C header and the C++ headers are rendered as two references, ``api/c`` and ``api/cpp``: a C
function and its C++ wrapper (``waggle_init``, ``waggle::init``) would otherwise get the same label.

apiary's own driver (``apiary_gen_cpp_docs.py``) assumes a library of modules under ``libs/``; Waggle
is one include directory, so this does the two steps itself. The compile flags are those apiary
already parses ``python/src/Core.hpp`` with (from the build's ``build.ninja``), so the build must be
configured with Ninja and ``WAGGLE_BUILD_PYTHON=ON``.

``Metal.h`` is Objective-C++, which this C++ parse cannot read; the device work guide documents it.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

#: The references: (directory, title, label, what the pages were generated from, headers
#: documented, include-relative).
REFERENCES = [
    ("c", "C API", "api_c", "the C header", ["Waggle/Waggle.h"]),
    ("cpp", "C++ API", "api_cpp", "the C++ headers", ["Waggle/Waggle.hpp", "Waggle/Types.hpp", "Waggle/Config.hpp", "Waggle/Clock.hpp"]),
]

#: What apiary's renderer writes for any module: "<title> C++ API" as the index's title, and a
#: note that the pages come from C++ headers. Rewritten for each reference until it takes them.
RENDERED_NOTE = "Generated from the C++ headers by"


def compile_flags(build_dir: Path, source_dir: Path) -> list[str]:
    """The flags after ``--`` in the build's apiary command for ``waggle._core``."""
    ninja = build_dir / "build.ninja"
    if not ninja.is_file():
        raise SystemExit(f"cpp_reference: {ninja} not found; configure the build with -G Ninja")
    m = re.search(r'-DAPIARY_COMMAND=([^"\n]*waggle_register_core[^"\n]*)', ninja.read_text(encoding="utf-8"))
    if m is None:
        raise SystemExit("cpp_reference: no apiary command for waggle._core in build.ninja; configure with WAGGLE_BUILD_PYTHON=ON")
    tokens = m.group(1).split(";")
    if "--" not in tokens:
        raise SystemExit("cpp_reference: the apiary command has no '--' before its compile flags")
    flags = tokens[tokens.index("--") :]
    # Strip a trailing " " left by the ninja line, and make sure the public headers resolve.
    flags = [f.strip() for f in flags if f.strip()]
    return [*flags, f"-I{source_dir / 'include'}"]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--source-dir", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--tool", required=True, help="the apiary binary")
    parser.add_argument("--scripts", type=Path, required=True, help="apiary's scripts directory (APIARY_SCRIPTS_DIR)")
    parser.add_argument("--out-dir", type=Path, required=True, help="where the two references go (<out-dir>/c, <out-dir>/cpp)")
    args = parser.parse_args()

    flags = compile_flags(args.build_dir, args.source_dir)
    for directory, title, label, source, headers in REFERENCES:
        out = args.out_dir / directory
        render(args, flags, headers, out, title, label)
        retitle(out, title, source)
        print(f"cpp_reference: {sum(1 for _ in out.rglob('*.rst'))} pages in {out}")
    return 0


def retitle(out: Path, title: str, source: str) -> None:
    """Give the index its own title, and every page a note naming what it came from."""
    for page in out.rglob("*.rst"):
        text = page.read_text()
        text = text.replace(RENDERED_NOTE, f"Generated from {source} by")
        if page.name == "index.rst":
            rendered = f"{title} C++ API"
            text = text.replace(f"{'=' * len(rendered)}\n{rendered}\n{'=' * len(rendered)}", f"{'=' * len(title)}\n{title}\n{'=' * len(title)}")
        page.write_text(text)


def render(args: argparse.Namespace, flags: list[str], headers: list[str], out: Path, title: str, label: str) -> None:
    """Parse @p headers through an umbrella and render their entities into @p out."""
    with tempfile.TemporaryDirectory() as tmp:
        umbrella = Path(tmp) / "waggle_docs.cpp"
        umbrella.write_text("".join(f"#include <{h}>\n" for h in headers))
        cmd = [args.tool, "--emit-cpp-docs-json", "--module", "waggle"]
        for header in headers:
            cmd += ["--source-include", header]
        cmd += [str(umbrella), *flags]
        run = subprocess.run(cmd, capture_output=True, text=True)
        if run.returncode != 0 or not run.stdout.strip():
            print(run.stderr, file=sys.stderr)
            raise SystemExit(f"cpp_reference: apiary could not parse {headers}")
        doc_json = Path(tmp) / "waggle.json"
        doc_json.write_text(json.dumps(json.loads(run.stdout)))
        out.mkdir(parents=True, exist_ok=True)
        rendered = subprocess.run(
            [sys.executable, str(args.scripts / "apiary_render_cpp_site.py"), "--outdir", str(out),
             "--module-title", title, "--index-label", label, "--label-prefix", label, str(doc_json)],
            capture_output=True, text=True,
        )  # fmt: skip
        if rendered.returncode != 0:
            print(rendered.stdout, rendered.stderr, sep="\n", file=sys.stderr)
            raise SystemExit(f"cpp_reference: rendering {title} failed")


if __name__ == "__main__":
    sys.exit(main())
