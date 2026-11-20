"""Builds the benchmark with link-time and profile-guided optimization of the library and runs it.

Three steps, the same as for your own program: an instrumented build (FASTHTMLPARSER_PGO=GENERATE), a training run
of the benchmark's fasthtmlparser rows, and a build with the profile (FASTHTMLPARSER_PGO=USE). Under MSVC run it
from a developer prompt (the instrumented program needs pgort140.dll on the PATH).

    python tools/pgo.py [build directory, default bench/build-pgo] [-- more cmake arguments]

The profile comes from the same pages the benchmark then times, so the gain it shows is the best case; on other pages
it is smaller.
"""

import glob
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROWS = " fasthtmlparser"  # the --filter of the benchmark: this library's rows, not the other libraries'


def run(*cmd, **kw):
    print("+", " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True, **kw)


def main():
    args = sys.argv[1:]
    extra = []
    if "--" in args:
        i = args.index("--")
        args, extra = args[:i], args[i + 1 :]
    build = os.path.abspath(args[0] if args else os.path.join(ROOT, "bench", "build-pgo"))
    bench = os.path.join(build, "fasthtmlparser_bench" + (".exe" if os.name == "nt" else ""))

    def configure(mode):
        run("cmake", "-S", os.path.join(ROOT, "bench"), "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
            "-DFASTHTMLPARSER_LTO=ON", "-DFASTHTMLPARSER_PGO=" + mode, *extra)
        run("cmake", "--build", build)

    # a profile of an earlier run would be merged too
    shutil.rmtree(os.path.join(build, "pgo"), ignore_errors=True)
    for f in glob.glob(os.path.join(build, "*.pgc")):
        os.remove(f)

    configure("GENERATE")
    print("training run...", flush=True)
    run(bench, "--filter", ROWS, stdout=subprocess.DEVNULL)
    configure("USE")
    run(bench, "--filter", ROWS)


if __name__ == "__main__":
    main()
