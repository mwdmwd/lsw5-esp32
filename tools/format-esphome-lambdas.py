#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Apply the repository's C++ style to ESPHome YAML lambdas without dumping YAML."""

import argparse
import re
import subprocess
from pathlib import Path

import yaml


def lambdas(node, indent=0, seen=None):
    if seen is None:
        seen = set()
    if node is None or id(node) in seen:
        return
    seen.add(id(node))
    if isinstance(node, yaml.ScalarNode):
        if node.tag == "!lambda":
            yield node, indent
    elif isinstance(node, yaml.MappingNode):
        for key, value in node.value:
            indent = key.start_mark.column + 2
            if isinstance(value, yaml.ScalarNode) and (
                key.value == "lambda" or key.value.endswith("_lambda")
            ):
                if id(value) not in seen:
                    seen.add(id(value))
                    yield value, indent
            else:
                yield from lambdas(value, indent, seen)
    elif isinstance(node, yaml.SequenceNode):
        for value in node.value:
            yield from lambdas(value, node.start_mark.column + 2, seen)


def format_code(code, clang_format, filename):
    # ESPHome substitutions are expanded before C++ compilation. Hide their
    # braces from clang-format, including substitutions inside string literals.
    substitutions = {}

    def hide(match):
        token = f"__esphome_format_{len(substitutions)}__"
        while token in code:
            token += "_"
        substitutions[token] = match.group()
        return token

    code = re.sub(r"\$\{[^}\n]+\}", hide, code)
    formatted = subprocess.run(
        [
            clang_format,
            f"--assume-filename={filename.with_suffix('.cpp')}",
            "--style={BasedOnStyle: InheritParentConfig, UseTab: Never}",
        ],
        input=code,
        text=True,
        capture_output=True,
        check=True,
    ).stdout
    for token, substitution in substitutions.items():
        formatted = formatted.replace(token, substitution)
    return formatted.rstrip("\n")


def format_yaml(source, clang_format, filename):
    replacements = []
    scalars = {
        token.end_mark.index: token
        for token in yaml.scan(source)
        if isinstance(token, yaml.ScalarToken)
    }
    for node, indent in lambdas(yaml.compose(source)):
        start, end = node.start_mark.index, node.end_mark.index
        scalar = scalars[end]
        code = format_code(node.value, clang_format, filename)
        if (
            node.style not in ("|", ">")
            and scalar.start_mark.line == scalar.end_mark.line
            and "\n" not in code
        ):
            # Replace only the scalar, retaining tags, anchors and comments.
            # Formatting can introduce YAML punctuation such as ": ".
            inline = yaml.safe_dump(
                code, default_style=node.style, allow_unicode=True, width=float("inf")
            )
            inline = inline.removesuffix("...\n").rstrip("\n")
            replacements.append((scalar.start_mark.index, end, inline))
            continue
        # Preserve header/trailing YAML comments and blank lines around blocks.
        if node.style in ("|", ">"):
            header = source[start : source.index("\n", start)]
            comment = header[header.index("#") :] if "#" in header else ""
            trailing = re.search(r"\n[ \t\n]*\Z", source[start:end])
            ending = trailing.group() if trailing else "\n"
        else:
            line_end = source.find("\n", end)
            line_end = len(source) if line_end < 0 else line_end
            comment = source[end:line_end].strip()
            if comment and not comment.startswith("#"):
                raise ValueError("Lambdas in flow collections must use block mappings")
            end = min(line_end + 1, len(source))
            ending = "\n"
        # Keep explicit tags and anchor definitions before the scalar itself.
        prefix = source[start : scalar.start_mark.index]
        header = prefix + "|-" + (" " + comment if comment else "") + "\n"
        body = "\n".join(" " * indent + line if line else "" for line in code.split("\n"))
        replacements.append((start, end, header + body + ending))
    for start, end, replacement in sorted(replacements, reverse=True):
        source = source[:start] + replacement + source[end:]
    return source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--clang-format", default="clang-format")
    parser.add_argument("files", nargs="*", type=Path)
    args = parser.parse_args()
    changed = False
    for path in args.files:
        source = path.read_text()
        formatted = format_yaml(source, args.clang_format, path.resolve())
        if formatted != source:
            changed = True
            if args.check:
                print(f"{path}: C++ lambda formatting differs; run make format")
            else:
                path.write_text(formatted)
    return int(args.check and changed)


if __name__ == "__main__":
    raise SystemExit(main())
