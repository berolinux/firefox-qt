# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

import subprocess
import sys


def main(output, input_header, *args):
    """Run Qt moc and write generated code to output.

    args[0] is the path to the moc binary (from GENERATED_FILES flags).
    """
    if not args:
        raise Exception("moc path not provided as GeneratedFile flag")
    moc_path = args[0]
    result = subprocess.run(
        [moc_path, input_header],
        check=True,
        capture_output=True,
        text=True,
    )
    output.write(result.stdout)
