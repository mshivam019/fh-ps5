# Desktop helper checks

Run from the repository root on Linux with Python 3.11+, clang and pyftpdlib:

```sh
python3 -m unittest discover -s ps5/tools/tests -v
```

Shared tests cover preparation, path safety, checksum validation and a local FTP
server. Xbox 360 tests use synthetic executables and compiled tiny ELF fixtures;
they never include game code or contact a console. Retail executable hashes are
replaced only in memory inside those tests. Real-data packaging must be checked
separately. Native Windows/macOS file-picker execution is not covered by Linux tests.

Native setup tests cover tool archives, response-file quoting and repeatable
Python/Git source patching. Private integration receipts in build/native-setup
record Windows tools run under Wine and generated-function equivalence. Neither
those receipts nor generated game code belongs in the setup ZIP.
