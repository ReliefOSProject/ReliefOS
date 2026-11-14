#!/usr/bin/env python3
"""Extract per-page gettext templates, update PO files, or check all translations."""
import argparse
from collections import Counter
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile

from babel.messages.catalog import Catalog
from babel.messages.pofile import PoFileError, read_po, write_po

LANGUAGES = ("en", "ja")
MARKER = ".reliefos-gettext-output"
HAN = re.compile("[\\u3400-\\u4dbf\\u4e00-\\u9fff\\uf900-\\ufaff\\U00020000-\\U000323af]")


def read_catalog(path, language=None):
    with path.open("rb") as stream:
        return read_po(stream, locale=language, abort_invalid=True)


def messages(catalog):
    return {(message.id, message.context): message for message in catalog if message.id}


def extract(source, output, sphinx):
    """Replace only an owned POT output; never compile catalogs in the source."""
    if output.is_symlink():
        raise RuntimeError(f"gettext output is a symlink: {output}")
    output = output.resolve()
    if output == source or output in source.parents or source in output.parents:
        raise RuntimeError("gettext output must be outside the documentation source")
    if output.exists() and not (output.is_dir() and (output / MARKER).is_file()):
        raise RuntimeError(f"refusing to replace unowned gettext output: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="docs-gettext-", dir=output.parent) as temporary:
        work = Path(temporary)
        (work / "locale").mkdir()
        command = [sphinx, *shlex.split(os.environ.get("SPHINXOPTS", "")),
                   "-b", "gettext", "-n", "-W", "--keep-going",
                   "-D", "language=zh_CN", "-D", "gettext_compact=0",
                   "-D", "gettext_location=0",
                   "-D", f"locale_dirs={work / 'locale'}",
                   "-d", str(work / "doctrees"), str(source), str(work / "pot")]
        subprocess.run(command, check=True)
        if not list((work / "pot").rglob("*.pot")):
            raise RuntimeError("Sphinx produced no gettext templates")
        (work / "pot" / MARKER).touch()
        if output.exists():
            shutil.rmtree(output)
        (work / "pot").rename(output)
    return output


def update_catalogs(source, templates):
    for language in LANGUAGES:
        for template_path in sorted(templates.rglob("*.pot")):
            relative = template_path.relative_to(templates).with_suffix(".po")
            target = source / "locale" / language / "LC_MESSAGES" / relative
            template = read_catalog(template_path)
            if target.exists():
                catalog = read_catalog(target, language)
            else:
                catalog = Catalog(locale=language, domain=relative.with_suffix("").as_posix(),
                                  project=template.project, version=template.version,
                                  copyright_holder=template.copyright_holder, fuzzy=False)
            catalog.update(template)
            target.parent.mkdir(parents=True, exist_ok=True)
            # Preserve existing translations, translator notes and obsolete entries.
            with tempfile.NamedTemporaryFile(dir=target.parent, delete=False) as stream:
                temporary = Path(stream.name)
                try:
                    write_po(stream, catalog, include_previous=True, no_location=True)
                    stream.close()
                    temporary.replace(target)
                finally:
                    temporary.unlink(missing_ok=True)
            print(f"updated {target}")


def identical_whitelist(source):
    path = source / "locale/identical.json"

    def unique_keys(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate language key: {key}")
            result[key] = value
        return result

    try:
        policy = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique_keys)
        if not isinstance(policy, dict) or set(policy) != set(LANGUAGES):
            raise ValueError("expected exactly the language keys en and ja")
        for language, entries in policy.items():
            if (not isinstance(entries, list)
                    or any(not isinstance(entry, str) or not entry.strip() for entry in entries)):
                raise ValueError(f"{language}: expected a list of nonempty strings")
            if len(entries) != len(set(entries)):
                raise ValueError(f"{language}: duplicate entries")
    except (OSError, ValueError) as error:
        raise RuntimeError(f"{path}: invalid identical whitelist: {error}") from error
    return {language: set(entries) for language, entries in policy.items()}


def check_catalogs(source, templates, allow_identical=()):
    """Compare message identities, not line numbers or extraction timestamps."""
    errors = []
    total = 0
    template_paths = sorted(templates.rglob("*.pot"))
    expected_files = {path.relative_to(templates).with_suffix(".po") for path in template_paths}
    policy = identical_whitelist(source)
    for language in LANGUAGES:
        allowed = policy[language] | set(allow_identical)
        directory = source / "locale" / language / "LC_MESSAGES"
        for extra in sorted(set(directory.rglob("*.po")) -
                            {directory / path for path in expected_files}):
            errors.append(f"{extra}: obsolete catalog (no current source page)")
        for template_path in template_paths:
            relative = template_path.relative_to(templates).with_suffix(".po")
            target = directory / relative
            expected = messages(read_catalog(template_path))
            total += len(expected)
            if not target.is_file():
                errors.append(f"{target}: missing PO ({len(expected)} messages)")
                continue
            catalog = read_catalog(target, language)
            actual = messages(catalog)
            counts = Counter()
            if catalog.fuzzy:
                counts["fuzzy header"] += 1
            for key, original in expected.items():
                message = actual.get(key)
                if message is None:
                    counts["missing message"] += 1
                    continue
                if message.fuzzy:
                    counts["fuzzy"] += 1
                strings = message.string if isinstance(message.string, (tuple, list)) else (message.string,)
                if (not strings or any(not value or not value.strip() for value in strings)
                        or (original.pluralizable and len(strings) != catalog.num_plurals)):
                    counts["untranslated"] += 1
                originals = original.id if original.pluralizable else (original.id,)
                if any(value and text not in allowed and HAN.search(text)
                       and value.strip() == text.strip()
                       for value in strings for text in originals):
                    counts["identical Chinese source"] += 1
            counts["obsolete message"] += len(actual.keys() - expected.keys())
            for message, problems in catalog.check():
                if message.id:
                    counts["invalid translation"] += len(problems)
            counts = +counts
            if counts:
                detail = ", ".join(f"{count} {name}" for name, count in sorted(counts.items()))
                errors.append(f"{target}: {detail}")
    if errors:
        raise RuntimeError("\n".join(errors))
    print(f"translations: {len(template_paths)} pages × {len(LANGUAGES)} languages, "
          f"{total} messages checked; complete and current")


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=root / "out/docs-gettext",
                        help="owned POT output directory, outside Documentation")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--update", action="store_true", help="initialize/merge en and ja PO files")
    mode.add_argument("--check", action="store_true", help="fail on incomplete, fuzzy or stale PO files")
    parser.add_argument("--allow-identical", action="append", default=[], metavar="MSGID",
                        help="temporary exact exception for both languages (repeatable)")
    args = parser.parse_args()
    try:
        source = root / "Documentation"
        sphinx = shutil.which(os.environ.get("SPHINXBUILD", "sphinx-build"))
        if sphinx is None:
            raise RuntimeError("Sphinx is required; install Documentation/requirements.txt")
        templates = extract(source, args.output, sphinx)
        if args.update:
            update_catalogs(source, templates)
        elif args.check:
            check_catalogs(source, templates, args.allow_identical)
        else:
            print(f"gettext templates: {templates}")
    except (RuntimeError, OSError, ValueError, PoFileError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"translations: {error}\n")


if __name__ == "__main__":
    main()
