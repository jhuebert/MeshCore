#!/usr/bin/env python3
# Adds the pio_env name as OTA_PIO_ENV to every ESP32 build, so the firmware
# can compute its target_id (sha256(pio_env)[0..3]) at runtime — the same
# value the phone app derives from the release asset name (<pio_env>-<tag>...).
# OTA releases rely on it to refuse updates armed for a different board.

Import("env")

# StringifyMacro quotes the value for the toolchain so the macro expands to a
# string literal in C++ — plain embedded quotes do not survive the gcc
# command line on every platform.
env["CPPDEFINES"] = env.get("CPPDEFINES", []) + [("OTA_PIO_ENV", env.StringifyMacro(env["PIOENV"]))]
