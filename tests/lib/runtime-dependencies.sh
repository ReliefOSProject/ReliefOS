# Shared host-native test libraries matching the signature-verified target APKs.
# The caller defines src as the repository root; no download occurs here.
runtime_deps=${RELIEFOS_TEST_DEP_ROOT:-$src/out/x86_64/release/host/runtime-development}
for product in usr/include/mbedtls/ssl.h usr/include/sqlite3.h usr/lib/libmbedcrypto.a usr/lib/libsqlite3.a; do
    [ -f "$runtime_deps/$product" ] || {
        printf 'missing native test dependency: %s; run make host-runtime-development (or set RELIEFOS_TEST_DEP_ROOT)\n' "$runtime_deps/$product" >&2
        exit 1
    }
done
