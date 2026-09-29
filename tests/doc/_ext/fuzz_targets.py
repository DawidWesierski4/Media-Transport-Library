# Copyright 2026 Intel Corporation
# SPDX-License-Identifier: BSD-3-Clause

"""Make the list of the fuzz targets from the build file and the harnesses.

The ``fuzz-targets`` directive reads the ``fuzz_targets`` list of
``tests/fuzz/meson.build``. For each target, it reads the first ``/** */``
comment of the harness source and the production ``.c`` file that the harness
includes. It writes a summary table and one section for each target. A new
harness thus gets its section with no change to the documentation.
"""

from __future__ import annotations

import os
import re

from docutils import nodes
from docutils.statemachine import StringList
from sphinx.util.docutils import SphinxDirective
from sphinx.util.nodes import nested_parse_with_titles

# One ['name', 'dir/file.c'] entry of the fuzz_targets list.
TARGET_RE = re.compile(r"\[\s*'(\w+)'\s*,\s*'([^']+\.c)'\s*\]")
COMMENT_RE = re.compile(r"/\*\*(.*?)\*/", re.S)
INCLUDE_RE = re.compile(r'^#include "([^"]+\.c)"', re.M)
# A C name with an underscore, or a function call, for example "struct st_hdr".
IDENT_RE = re.compile(r"\b((?:struct )?[A-Za-z]\w*_\w*(?:\(\))?)")
# The include directories of tests/fuzz/meson.build, relative to the root.
INCLUDE_DIRS = ("lib/src", "include")
# This file is tests/doc/_ext/fuzz_targets.py.
ROOT_DIR = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))


def _paragraphs(source: str) -> list[str]:
    """Return the paragraphs of the first /** */ comment, without @file."""
    match = COMMENT_RE.search(source)
    if not match:
        return []
    lines = []
    for line in match.group(1).splitlines():
        line = re.sub(r"^\s*\*? ?", "", line).rstrip()
        if not line.startswith("@file"):
            lines.append(line)
    text = "\n".join(lines).strip()
    return [" ".join(p.split()) for p in re.split(r"\n\s*\n", text) if p.strip()]


def _literal(text: str) -> str:
    return IDENT_RE.sub(r"``\1``", text)


class FuzzTargets(SphinxDirective):
    has_content = False

    def run(self):
        fuzz_dir = os.path.join(ROOT_DIR, "tests", "fuzz")
        build_file = os.path.join(fuzz_dir, "meson.build")
        if not os.path.isfile(build_file):
            raise self.error(f"fuzz-targets: {build_file} does not exist")
        self.env.note_dependency(build_file)
        with open(build_file, encoding="utf-8") as f:
            targets = TARGET_RE.findall(f.read())
        if not targets:
            raise self.error(f"fuzz-targets: {build_file} has no fuzz_targets entry")

        url = self.config.fuzz_targets_url
        table = [
            ".. list-table::",
            "   :header-rows: 1",
            "   :widths: 30 70",
            "",
            "   * - Target",
            "     - Input",
        ]
        sections = []
        for name, src in targets:
            path = os.path.join(fuzz_dir, src)
            if not os.path.isfile(path):
                raise self.error(f"fuzz-targets: {name} has no source {path}")
            self.env.note_dependency(path)
            with open(path, encoding="utf-8") as f:
                source = f.read()
            paragraphs = [_literal(p) for p in _paragraphs(source)]
            if not paragraphs:
                raise self.error(f"fuzz-targets: {path} has no /** */ file comment")
            table += [f"   * - :ref:`fuzz-{name}`", f"     - {paragraphs[0]}"]

            harness = f"tests/fuzz/{src}"
            fields = [f":Harness: `{harness} <{url}/{harness}>`__"]
            for include in INCLUDE_RE.findall(source):
                for base in (os.path.dirname(harness),) + INCLUDE_DIRS:
                    if os.path.isfile(os.path.join(ROOT_DIR, base, include)):
                        code = os.path.normpath(f"{base}/{include}").replace(
                            os.sep, "/"
                        )
                        fields.append(f":Code under test: `{code} <{url}/{code}>`__")
                        break
            title = f"``{name}``"
            sections += [f".. _fuzz-{name}:", "", title, "-" * len(title), ""]
            for paragraph in paragraphs:
                sections += [paragraph, ""]
            sections += fields + [
                "",
                ".. code-block:: sh",
                "",
                f"   mkdir -p corpus/{name}",
                f"   ./build_fuzz/tests/fuzz/{name} -max_total_time=60 corpus/{name}",
                "",
            ]

        content = StringList()
        for line in table + [""] + sections:
            content.append(line, build_file)
        node = nodes.section()
        node.document = self.state.document
        nested_parse_with_titles(self.state, content, node)
        return node.children


def setup(app):
    app.add_config_value(
        "fuzz_targets_url",
        "https://github.com/OpenVisualCloud/Media-Transport-Library/blob/main",
        "env",
    )
    app.add_directive("fuzz-targets", FuzzTargets)
    return {"parallel_read_safe": True, "parallel_write_safe": True}
