#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
tmp="$src/out/tmp"
export TMPDIR="$tmp"
mkdir -p "$tmp"
work=$(mktemp -d "$tmp/reliefmail-tests.XXXXXX")
fixture_pid=
cleanup() {
    if [ -n "$fixture_pid" ]; then
        kill "$fixture_pid" 2>/dev/null || true
        wait "$fixture_pid" 2>/dev/null || true
    fi
    if [ "${RELIEFMAIL_KEEP_TEST_WORK:-0}" = 1 ]; then
        printf 'ReliefMail test work preserved: %s\n' "$work" >&2
    else
        rm -rf "$work"
    fi
}
trap cleanup EXIT
trap 'exit 130' HUP INT TERM

run_protocol() {
    cert_dir="$work/protocol-certs"
    ready_file="$work/protocol-ready.tsv"
    audit_file="$work/protocol-audit.json"
    fixture="$src/tools/tests/reliefmail_protocol_server.py"
    "${PYTHON:-python3}" "$fixture" --prepare-certs "$cert_dir"

    set -- -std=c++17 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
        -fno-exceptions -fno-rtti -nostdinc++ -fsanitize=address,undefined \
        -fno-sanitize-recover=all -g -I"$src/userland/apps/reliefmail" \
        -I/usr/include/mbedtls3 \
        "$src/tests/host/test_reliefmail_protocol.cpp" \
        "$src/userland/apps/reliefmail/model.cpp" \
        "$src/userland/apps/reliefmail/mime.cpp" \
        "$src/userland/apps/reliefmail/html.cpp"
    if [ -f "$src/userland/apps/reliefmail/transport.cpp" ] && \
       [ -f "$src/userland/apps/reliefmail/imap.cpp" ] && \
       [ -f "$src/userland/apps/reliefmail/smtp.cpp" ]; then
        set -- "$@" \
            "$src/userland/apps/reliefmail/transport.cpp" \
            "$src/userland/apps/reliefmail/imap.cpp" \
            "$src/userland/apps/reliefmail/smtp.cpp"
    fi
    "${HOSTCXX:-c++}" "$@" -L/usr/lib/mbedtls3 \
        -Wl,-rpath,/usr/lib/mbedtls3 -lmbedtls -lmbedx509 -lmbedcrypto \
        -o "$work/reliefmail-protocol"

    build_worker

    "${PYTHON:-python3}" "$fixture" --serve --cert-dir "$cert_dir" \
        --ready-file "$ready_file" --audit-file "$audit_file" \
        >"$work/protocol-fixture.stdout" 2>"$work/protocol-fixture.stderr" &
    fixture_pid=$!
    attempt=0
    while [ ! -f "$ready_file" ]; do
        if ! kill -0 "$fixture_pid" 2>/dev/null; then
            cat "$work/protocol-fixture.stdout" "$work/protocol-fixture.stderr" >&2
            return 1
        fi
        attempt=$((attempt + 1))
        if [ "$attempt" -ge 100 ]; then
            cat "$work/protocol-fixture.stdout" "$work/protocol-fixture.stderr" >&2
            return 1
        fi
        sleep 0.05
    done
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
        "$work/reliefmail-worker" --fixture "$ready_file" "$cert_dir/ca.crt"
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
        "$work/reliefmail-protocol" "$ready_file" "$cert_dir/ca.crt" \
        "$cert_dir/untrusted-ca.crt"
    kill "$fixture_pid"
    wait "$fixture_pid"
    fixture_pid=
    "${PYTHON:-python3}" "$fixture" --verify-audit "$audit_file"
}

build_sqlite_core() {
    if [ -f "$work/sqlite_core.o" ]; then return; fi
    generated="$src/out/xorg-vmware-debug/upstream/app-sqlite/generated"
    "${HOSTCC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
        -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION=1 -DSQLITE_OMIT_WAL=1 \
        -DSQLITE_MAX_MMAP_SIZE=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -I"$generated" -c \
        "$src/userland/apps/reliefmail/sqlite_core.c" -o "$work/sqlite_core.o"
}

build_store() {
    build_sqlite_core
    "${HOSTCXX:-c++}" -std=c++17 -D_POSIX_C_SOURCE=200809L -DRELIEFMAIL_TESTING \
        -Wall -Wextra -Werror -fno-exceptions -fno-rtti -nostdinc++ \
        -fsanitize=address,undefined -fno-sanitize-recover=all -g \
        -I"$src/userland/apps/reliefmail" -I"$src/out/xorg-vmware-debug/upstream/app-sqlite/generated" \
        -I/usr/include/mbedtls3 "$src/tests/host/test_reliefmail_store.cpp" \
        "$src/userland/apps/reliefmail/store.cpp" "$src/userland/apps/reliefmail/model.cpp" \
        "$src/userland/apps/reliefmail/mime.cpp" "$src/userland/apps/reliefmail/html.cpp" \
        "$src/userland/apps/reliefmail/profile_io.cpp" "$src/userland/apps/reliefmail/vault.cpp" \
        "$work/sqlite_core.o" -L/usr/lib/mbedtls3 -Wl,-rpath,/usr/lib/mbedtls3 -lmbedcrypto \
        -o "$work/reliefmail-store"
}

build_worker() {
    build_sqlite_core
    "${HOSTCXX:-c++}" -std=c++17 -D_POSIX_C_SOURCE=200809L -DRELIEFMAIL_TESTING \
        -Wall -Wextra -Werror -fno-exceptions -fno-rtti -nostdinc++ \
        -fsanitize=address,undefined -fno-sanitize-recover=all -g \
        -I"$src/userland/apps/reliefmail" -I"$src/out/xorg-vmware-debug/upstream/app-sqlite/generated" \
        -I/usr/include/mbedtls3 "$src/tests/host/test_reliefmail_worker.cpp" \
        "$src/userland/apps/reliefmail/worker.cpp" "$src/userland/apps/reliefmail/tasks.cpp" \
        "$src/userland/apps/reliefmail/store.cpp" "$src/userland/apps/reliefmail/model.cpp" \
        "$src/userland/apps/reliefmail/mime.cpp" "$src/userland/apps/reliefmail/html.cpp" \
        "$src/userland/apps/reliefmail/reader_render.cpp" \
        "$src/userland/apps/reliefmail/imap.cpp" "$src/userland/apps/reliefmail/smtp.cpp" \
        "$src/userland/apps/reliefmail/transport.cpp" "$src/userland/apps/reliefmail/profile_io.cpp" \
        "$src/userland/apps/reliefmail/vault.cpp" "$work/sqlite_core.o" \
        -L/usr/lib/mbedtls3 -Wl,-rpath,/usr/lib/mbedtls3 -lmbedtls -lmbedx509 -lmbedcrypto -lpng16 -lz \
        -o "$work/reliefmail-worker"
}

run_store_task5() {
    build_store
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/reliefmail-store" --task5
}

run_worker_local() {
    build_worker
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/reliefmail-worker" --local
}

build_mime() {
    "${HOSTCXX:-c++}" -std=c++17 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
        -fno-exceptions -fno-rtti -nostdinc++ -fsanitize=address,undefined \
        -fno-sanitize-recover=all -g -I"$src/userland/apps/reliefmail" \
        "$src/tests/host/test_reliefmail_mime.cpp" \
        "$src/userland/apps/reliefmail/model.cpp" \
        "$src/userland/apps/reliefmail/mime.cpp" \
        "$src/userland/apps/reliefmail/html.cpp" -o "$work/reliefmail-mime"
}

run_mime() {
    build_mime
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/reliefmail-mime"
    printf 'ReliefMail MIME/HTML suite passed\n'
    "$work/reliefmail-mime" --emit-eml "$work/interop.eml"
    ${PYTHON:-python3} "$src/tests/host/test_reliefmail_mime_interop.py" "$work/interop.eml"
}

if [ "${1:-all}" = protocol ]; then
    run_protocol
    exit 0
fi
if [ "${1:-all}" = worker ]; then
    run_worker_local
    exit 0
fi
if [ "${1:-all}" = task6-adapters ]; then
    "${HOSTCXX:-c++}" -std=c++17 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
        -fno-exceptions -fno-rtti -nostdinc++ -fsanitize=address,undefined \
        -fno-sanitize-recover=all -g -I"$src/userland/apps/reliefmail" \
        -I/usr/include/mbedtls3 \
        "$src/tests/host/test_reliefmail_task6_adapters.cpp" \
        "$src/userland/apps/reliefmail/ui_adapters.cpp" \
        "$src/userland/apps/reliefmail/model.cpp" \
        "$src/userland/apps/reliefmail/vault.cpp" \
        "$src/userland/apps/reliefmail/profile_io.cpp" \
        "$src/userland/apps/reliefmail/html.cpp" \
        -L/usr/lib/mbedtls3 -Wl,-rpath,/usr/lib/mbedtls3 -lmbedcrypto \
        -o "$work/reliefmail-task6-adapters"
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/reliefmail-task6-adapters"
    printf 'ReliefMail Task6 adapter suite passed\n'
    exit 0
fi
if [ "${1:-all}" = reader-render ]; then
    "${HOSTCXX:-c++}" -std=c++17 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
        -fno-exceptions -fno-rtti -nostdinc++ -fsanitize=address,undefined \
        -fno-sanitize-recover=all -g -I"$src/userland/apps/reliefmail" \
        "$src/tests/host/test_reliefmail_reader_render.cpp" \
        "$src/userland/apps/reliefmail/reader_render.cpp" \
        "$src/userland/apps/reliefmail/model.cpp" \
        "$src/userland/apps/reliefmail/html.cpp" -lpng16 -lz -o "$work/reliefmail-reader-render"
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/reliefmail-reader-render"
    printf 'ReliefMail safe reader renderer suite passed\n'
    exit 0
fi
if [ "${1:-all}" = editor ]; then
    "${HOSTCXX:-c++}" -std=c++17 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
        -fno-exceptions -fno-rtti -nostdinc++ -fsanitize=address,undefined \
        -fno-sanitize-recover=all -g -I"$src/userland/apps/reliefmail" \
        -I"$src/userland/apps/reliefword" \
        "$src/tests/host/test_reliefmail_editor.cpp" \
        "$src/userland/apps/reliefmail/editor.cpp" \
        "$src/userland/apps/reliefmail/compose_state.cpp" \
        "$src/userland/apps/reliefmail/model.cpp" \
        "$src/userland/apps/reliefmail/mime.cpp" \
        "$src/userland/apps/reliefmail/html.cpp" \
        "$src/userland/apps/reliefword/model.cpp" \
        "$src/userland/apps/reliefword/layout.cpp" \
        -o "$work/reliefmail-editor"
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/reliefmail-editor"
    printf 'ReliefMail rich editor suite passed\n'
    exit 0
fi
if [ "${1:-all}" = ui-layout ]; then
    "${HOSTCXX:-c++}" -std=c++17 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
        -fno-exceptions -fno-rtti -nostdinc++ -fsanitize=address,undefined \
        -fno-sanitize-recover=all -g -I"$src/userland/apps/reliefmail" \
        "$src/tests/host/test_reliefmail_ui_layout.cpp" -o "$work/reliefmail-ui-layout"
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/reliefmail-ui-layout"
    printf 'ReliefMail UI layout suite passed\n'
    exit 0
fi
if [ "${1:-all}" = actions ]; then
    "${HOSTCXX:-c++}" -std=c++17 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
        -fno-exceptions -fno-rtti -nostdinc++ -fsanitize=address,undefined \
        -fno-sanitize-recover=all -g -I"$src/userland/apps/reliefmail" \
        "$src/tests/host/test_reliefmail_actions.cpp" -o "$work/reliefmail-actions"
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/reliefmail-actions"
    exit 0
fi
if [ "${1:-all}" = store-task5 ]; then
    run_store_task5
    exit 0
fi
if [ "${1:-all}" = store-task8a ]; then
    build_store
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/reliefmail-store" --task8a
    exit 0
fi
if [ "${1:-all}" = vault-task5 ]; then
    "${HOSTCXX:-c++}" -std=c++17 -D_POSIX_C_SOURCE=200809L -DRELIEFMAIL_TESTING \
        -Wall -Wextra -Werror -fno-exceptions -fno-rtti -nostdinc++ \
        -fsanitize=address,undefined -fno-sanitize-recover=all -g \
        -I"$src/userland/apps/reliefmail" -I/usr/include/mbedtls3 \
        "$src/tests/host/test_reliefmail_vault.cpp" \
        "$src/userland/apps/reliefmail/vault.cpp" \
        "$src/userland/apps/reliefmail/profile_io.cpp" \
        -L/usr/lib/mbedtls3 -Wl,-rpath,/usr/lib/mbedtls3 -lmbedcrypto \
        -o "$work/reliefmail-vault"
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
        "$work/reliefmail-vault" --task5-password-change
    exit 0
fi
if [ "${1:-all}" = mime ]; then
    run_mime
    exit 0
fi
if [ "${1:-all}" = date-cpp ]; then
    build_mime
    ulimit -c 0
    if locale -a 2>/dev/null | grep -iq '^zh_CN\.utf8$'; then
        LC_ALL=zh_CN.utf8 "$work/reliefmail-mime" --date-only
    else
        "$work/reliefmail-mime" --date-only
    fi
    exit $?
fi
if [ "${1:-all}" = date-python ]; then
    build_mime
    "$work/reliefmail-mime" --emit-eml "$work/interop.eml"
    ${PYTHON:-python3} "$src/tests/host/test_reliefmail_mime_interop.py" "$work/interop.eml"
    exit $?
fi
if [ "${1:-all}" = date-header-limit ]; then
    build_mime
    ulimit -c 0
    "$work/reliefmail-mime" --date-header-limit
    exit $?
fi
${HOSTCXX:-c++} -std=c++17 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
    -fno-exceptions -fno-rtti -nostdinc++ -fsanitize=address,undefined \
    -fno-sanitize-recover=all -g -I"$src/userland/apps/reliefmail" \
    "$src/tests/host/test_reliefmail_model.cpp" \
    "$src/userland/apps/reliefmail/model.cpp" -o "$work/reliefmail-model"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/reliefmail-model"
printf 'ReliefMail model suite passed\n'
run_mime
build_store
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/reliefmail-store"
printf 'ReliefMail store suite passed\n'
${HOSTCXX:-c++} -std=c++17 -D_POSIX_C_SOURCE=200809L -DRELIEFMAIL_TESTING \
    -Wall -Wextra -Werror -fno-exceptions -fno-rtti -nostdinc++ \
    -fsanitize=address,undefined -fno-sanitize-recover=all -g \
    -I"$src/userland/apps/reliefmail" -I/usr/include/mbedtls3 \
    "$src/tests/host/test_reliefmail_vault.cpp" \
    "$src/userland/apps/reliefmail/vault.cpp" \
    "$src/userland/apps/reliefmail/profile_io.cpp" \
    -L/usr/lib/mbedtls3 -Wl,-rpath,/usr/lib/mbedtls3 -lmbedcrypto \
    -o "$work/reliefmail-vault"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/reliefmail-vault"
printf 'ReliefMail vault suite passed\n'
run_protocol
