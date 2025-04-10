#!/usr/bin/env python3
"""Applies fixes, generated by buildscripts/clang-tidy.py, across the codebase."""

import argparse
import hashlib
import json
import os
import re
import sys
from collections import defaultdict

import yaml


def is_writeable(file) -> bool:
    try:
        with open(file, "a") as f:  # noqa: F841
            pass
        return True
    except OSError:
        return False


def can_replacements_be_applied(replacements) -> bool:
    """Checks if the files containing replacements are unchanged since clang-tidy was run and are writeable.
    For any replacement to be valid, all impacted files must be unchanged."""
    for replacement in replacements:
        if (
            os.path.exists(replacement["FilePath"])
            and is_writeable(replacement["FilePath"])
            and replacement["FileContentsMD5"]
        ):
            with open(replacement["FilePath"], "rb") as fin:
                file_bytes = fin.read()
                current_md5 = hashlib.md5(file_bytes).hexdigest()
                if current_md5 != replacement["FileContentsMD5"]:
                    return False
        else:
            return False
    return True


def get_replacements_to_apply(fixes_file) -> dict:
    """Gets a per file listing of the valid replacements to apply."""
    replacements_to_apply = defaultdict(list)

    with open(fixes_file) as fin:
        fixes_data = json.load(fin)
        for clang_tidy_check in fixes_data:
            for main_source_file in fixes_data[clang_tidy_check]:
                for violation_instance in fixes_data[clang_tidy_check][main_source_file]:
                    replacements = fixes_data[clang_tidy_check][main_source_file][
                        violation_instance
                    ]["replacements"]
                    if can_replacements_be_applied(replacements):
                        for replacement in replacements:
                            replacements_to_apply[replacement["FilePath"]].append(replacement)
                    else:
                        print(
                            f"""WARNING: not applying replacements for {clang_tidy_check} in {main_source_file} at offset {violation_instance}, at least one file that is part of the automatic replacement has changed since clang-tidy was run, or is not writeable."""
                        )

    return replacements_to_apply


def _combine_errors(dir: str) -> str:
    failed_files = 0
    all_fixes = {}
    files_to_parse = []
    for root, _, files in os.walk(dir):
        for name in files:
            if name.endswith("clang-tidy.yaml"):
                files_to_parse.append(os.path.join(root, name))

    # loop files_to_parse and count the number of failed_files
    for item in files_to_parse:
        if item is None:
            continue
        failed_files += 1

        # Read the yaml fixes for the file to combine them with the other suggested fixes
        with open(item) as input_yml:
            fixes = yaml.safe_load(input_yml)
        if not fixes:
            continue
        for fix in fixes["Diagnostics"]:
            fix_msg = None
            if "Notes" in fix:
                fix_msg = fix["Notes"][0]
                if len(fix["Notes"]) > 1:
                    print(f'Warning: this script may be missing values in [{fix["Notes"]}]')
            else:
                fix_msg = fix["DiagnosticMessage"]
            fix_data = (
                all_fixes.setdefault(fix["DiagnosticName"], {})
                .setdefault(
                    re.sub(
                        "^.*/src/mongo/",
                        "src/mongo/",
                        fix_msg.get("FilePath", "FilePath Not Found"),
                        1,
                    ),
                    {},
                )
                .setdefault(
                    str(fix_msg.get("FileOffset", "FileOffset Not Found")),
                    {
                        "replacements": fix_msg.get("Replacements", "Replacements not found"),
                        "message": fix_msg.get("Message", "Message not found"),
                        "count": 0,
                        "source_files": [],
                    },
                )
            )

            for replacement in fix_data["replacements"]:
                if replacement.get("FilePath"):
                    file_path = re.sub(
                        "^.*/src/mongo/", "src/mongo/", replacement.get("FilePath"), 1
                    )
                    replacement["FilePath"] = file_path
                    if os.path.exists(file_path):
                        with open(file_path, "rb") as contents:
                            replacement["FileContentsMD5"] = hashlib.md5(
                                contents.read()
                            ).hexdigest()

            fix_data["count"] += 1
            fix_data["source_files"].append(
                re.sub("^.*/src/mongo/", "src/mongo", fixes["MainSourceFile"], 1)
            )

    fixes_file = os.path.join(dir, "clang_tidy_fixes.json")
    with open(fixes_file, "w") as files_file:
        json.dump(all_fixes, files_file, indent=4, sort_keys=True)

    return fixes_file


def main(argv=sys.argv[1:]):
    parser = argparse.ArgumentParser()
    parser.add_argument(
        dest="fixes",
        help="Path to fixes file or directory of fixes files.",
        nargs="?",
        default="bazel-bin",
    )
    args = parser.parse_args(argv)

    if os.path.isdir(args.fixes):
        fixes_file = _combine_errors(args.fixes)
    else:
        fixes_file = args.fixes

    replacements_to_apply = get_replacements_to_apply(fixes_file)

    for file in replacements_to_apply:
        with open(file, "rb") as fin:
            file_bytes = fin.read()

        # perform the swap replacement of the binary data
        file_bytes = bytearray(file_bytes)
        replacements_to_apply[file].sort(key=lambda r: r["Offset"])
        adjustments = 0

        for replacement in replacements_to_apply[file]:
            file_bytes[
                replacement["Offset"] + adjustments : replacement["Offset"]
                + adjustments
                + replacement["Length"]
            ] = replacement["ReplacementText"].encode()

            if replacement["Length"] != len(replacement["ReplacementText"]):
                adjustments += len(replacement["ReplacementText"]) - replacement["Length"]

        with open(file, "wb") as fout:
            fout.write(bytes(file_bytes))


if __name__ == "__main__":
    main()
