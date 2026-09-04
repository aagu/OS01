#!/usr/bin/env python3
import os
import subprocess
import sys
import tempfile


def run(cmd, cwd):
    return subprocess.run(cmd, cwd=cwd, check=False, text=True,
                          capture_output=True)


def main():
    cc = os.environ.get("CC", "cc")
    ar = os.environ.get("AR", "ar")
    with tempfile.TemporaryDirectory(prefix="os01-runtime-order-") as td:
        main_c = os.path.join(td, "main.c")
        consumer_c = os.path.join(td, "consumer.c")
        runtime_c = os.path.join(td, "runtime.c")
        with open(main_c, "w") as f:
            f.write("extern int consumer_fixture_symbol(void);\n"
                    "int main(void) { return consumer_fixture_symbol() != 42; }\n")
        with open(consumer_c, "w") as f:
            f.write("extern int runtime_fixture_symbol(void);\n"
                    "int consumer_fixture_symbol(void) { return runtime_fixture_symbol(); }\n")
        with open(runtime_c, "w") as f:
            f.write("int runtime_fixture_symbol(void) { return 42; }\n")
        for src, obj in ((main_c, "main.o"), (consumer_c, "consumer.o"),
                         (runtime_c, "runtime.o")):
            result = run([cc, "-c", src, "-o", obj], td)
            if result.returncode:
                sys.stderr.write(result.stdout + result.stderr)
                return 1
        for archive, obj in (("libconsumer.a", "consumer.o"), ("libruntime.a", "runtime.o")):
            result = run([ar, "rcs", archive, obj], td)
            if result.returncode:
                sys.stderr.write(result.stdout + result.stderr)
                return 1
        bad = run([cc, "-o", "bad", "main.o", "libruntime.a", "libconsumer.a"], td)
        good = run([cc, "-o", "good", "main.o", "libconsumer.a", "libruntime.a"], td)
        if bad.returncode == 0 or good.returncode != 0:
            sys.stderr.write("bad link output:\n" + bad.stdout + bad.stderr)
            sys.stderr.write("good link output:\n" + good.stdout + good.stderr)
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
