import json
from pathlib import Path

Import("env")

# ESP-IDF 6 adds -fuse-cxa-atexit for C++ compilation. PlatformIO's external
# library builder can leak it into C compiles; libsodium treats warnings as
# errors, so the harmless "valid for C++ but not C" warning breaks the build.
for key in ("CCFLAGS", "CFLAGS"):
    env[key] = [flag for flag in env.get(key, []) if flag != "-fuse-cxa-atexit"]

env.AppendUnique(CCFLAGS=["-Wno-error"])


def patch_libsodium_manifest():
    project_dir = Path(env.subst("$PROJECT_DIR"))
    libdeps_dir = project_dir / ".pio" / "libdeps"
    if not libdeps_dir.exists():
        return

    required_sources = [
        "+<crypto_hash/sha512/hash_sha512.c>",
        "+<crypto_hash/sha512/cp/hash_sha512_cp.c>",
        "+<crypto_sign/crypto_sign.c>",
        "+<crypto_sign/ed25519/sign_ed25519.c>",
        "+<crypto_sign/ed25519/ref10/keypair.c>",
        "+<crypto_sign/ed25519/ref10/sign.c>",
        "+<crypto_sign/ed25519/ref10/open.c>",
        "+<crypto_sign/ed25519/ref10/obsolete.c>",
    ]

    for manifest in libdeps_dir.glob("*/libsodium/library.json"):
        data = json.loads(manifest.read_text(encoding="utf-8"))
        build = data.setdefault("build", {})
        src_filter = build.setdefault("srcFilter", [])
        changed = False
        for source in required_sources:
            if source not in src_filter:
                src_filter.append(source)
                changed = True
        if changed:
            manifest.write_text(
                json.dumps(data, indent=4) + "\n",
                encoding="utf-8",
            )


patch_libsodium_manifest()
