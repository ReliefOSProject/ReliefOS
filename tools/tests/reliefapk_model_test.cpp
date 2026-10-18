#include "model.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace ra;

static unsigned failures = 0;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expr); \
        ++failures; \
    } \
} while (0)

static bool equals(const Text &value, const char *expected) {
    return value.size() == strlen(expected) && !memcmp(value.c_str(), expected, value.size());
}

static bool arg_is(const Command &command, size_t at, const char *expected) {
    return at < command.args.size() && equals(command.args[at], expected);
}

static void reject_json(const char *json, size_t length) {
    PackageParser parser;
    bool okay = parser.begin(json, length);
    for (unsigned i = 0; okay && !parser.done() && i < 4; ++i) okay = parser.step(8);
    CHECK(!okay || !parser.error().empty());
    Array<Package> result = parser.take();
    CHECK(result.empty());
}

static void test_parser_fields_and_unicode() {
    const char json[] =
        "[{\"name\":\"relief-demo\",\"version\":\"2.4-r1\","
        "\"description\":\"救援 \\uD83D\\uDE80\\nline\",\"arch\":\"x86_64\","
        "\"license\":\"MIT\",\"url\":\"https://example.invalid\","
        "\"origin\":\"relief-source\",\"installed-size\":4096,\"file-size\":1024,"
        "\"status\":\"installed\",\"depends\":[\"so:libc.musl-x86_64.so.1\",\"cmd:sh\"],"
        "\"reverse-depends\":[\"relief-shell\"],\"provides\":[\"cmd:relief-demo\"],"
        "\"repositories\":[\"main\",\"community\"],"
        "\"contents\":{\"usr/bin/relief-demo\":\"file\",\"usr/share/relief-demo/\":\"dir\"},"
        "\"unknown\":{\"nested\":[true,null,{\"n\":-1.25e+4}]}}]";
    PackageParser parser;
    CHECK(parser.begin(json, sizeof(json) - 1));
    CHECK(!parser.done());
    CHECK(parser.step(1));
    CHECK(parser.done());
    Array<Package> packages = parser.take();
    CHECK(packages.size() == 1);
    if (packages.size() == 1) {
        const Package &package = packages[0];
        CHECK(equals(package.name, "relief-demo"));
        CHECK(equals(package.version, "2.4-r1"));
        CHECK(equals(package.description, "救援 🚀\nline"));
        CHECK(equals(package.arch, "x86_64"));
        CHECK(equals(package.license, "MIT"));
        CHECK(equals(package.url, "https://example.invalid"));
        CHECK(equals(package.origin, "relief-source"));
        CHECK(package.installed_size == 4096 && package.file_size == 1024);
        CHECK(package.installed);
        CHECK(equals(package.depends, "so:libc.musl-x86_64.so.1\ncmd:sh"));
        CHECK(equals(package.reverse_depends, "relief-shell"));
        CHECK(equals(package.provides, "cmd:relief-demo"));
        CHECK(equals(package.repositories, "main\ncommunity"));
        CHECK(equals(package.contents, "usr/bin/relief-demo\nusr/share/relief-demo/"));
    }
    CHECK(parser.take().empty());

    const char array_status[] = "[{\"name\":\"array-status\",\"version\":\"1\",\"status\":[\"installed\"]}]";
    CHECK(parser.begin(array_status, sizeof(array_status) - 1));
    CHECK(parser.step(1));
    Array<Package> with_array_status = parser.take();
    CHECK(with_array_status.size() == 1 && with_array_status[0].installed);
}

static void test_parser_budget_and_atomic_error() {
    const char json[] =
        "[{\"name\":\"a\",\"version\":\"1\"},"
        "{\"name\":\"b\",\"version\":\"2\"},"
        "{\"name\":\"c\",\"version\":\"3\"}]";
    PackageParser parser;
    CHECK(parser.begin(json, sizeof(json) - 1));
    CHECK(parser.step(1));
    CHECK(!parser.done());
    CHECK(parser.take().empty());
    CHECK(parser.step(1));
    CHECK(!parser.done());
    CHECK(parser.step(1));
    CHECK(parser.done());
    Array<Package> packages = parser.take();
    CHECK(packages.size() == 3);

    const char malformed[] = "[{\"name\":\"ok\",\"version\":\"1\"},{\"name\":2}]";
    CHECK(parser.begin(malformed, sizeof(malformed) - 1));
    CHECK(!parser.step(8));
    CHECK(!parser.error().empty());
    CHECK(parser.take().empty());
}

static void test_parser_sorts_by_name() {
    const char json[] =
        "[{\"name\":\"zeta\",\"version\":\"1\"},"
        "{\"name\":\"middle\",\"version\":\"1\"},"
        "{\"name\":\"alpha\",\"version\":\"1\"}]";
    PackageParser parser;
    CHECK(parser.begin(json, sizeof(json) - 1));
    CHECK(parser.step(8));
    Array<Package> packages = parser.take();
    CHECK(packages.size() == 3);
    if (packages.size() == 3) {
        CHECK(equals(packages[0].name, "alpha"));
        CHECK(equals(packages[1].name, "middle"));
        CHECK(equals(packages[2].name, "zeta"));
    }
}

static void test_parser_scanner_handles_braces_in_strings() {
    const char json[] =
        "[{\"name\":\"braces\",\"version\":\"1\","
        "\"unknown\":{\"text\":\"} { \\\"quoted\\\"\"}}]";
    PackageParser parser;
    CHECK(parser.begin(json, sizeof(json) - 1));
    CHECK(parser.step(1));
    CHECK(parser.done());
    Array<Package> packages = parser.take();
    CHECK(packages.size() == 1);
    if (packages.size() == 1) CHECK(equals(packages[0].name, "braces"));
}

static void test_parser_rejects_invalid_json() {
    reject_json("{}", 2);
    reject_json("[{\"name\":\"x\"}]", strlen("[{\"name\":\"x\"}]"));
    reject_json("[{\"name\":\"x\",\"version\":3}]", strlen("[{\"name\":\"x\",\"version\":3}]"));
    reject_json("[{\"name\":\"x\",\"version\":\"1\",\"depends\":\"bad\"}]",
                strlen("[{\"name\":\"x\",\"version\":\"1\",\"depends\":\"bad\"}]"));
    reject_json("[{\"name\":\"x\",\"version\":\"1\"}] trailing",
                strlen("[{\"name\":\"x\",\"version\":\"1\"}] trailing"));
    reject_json("[{\"name\":\"x\",\"version\":\"1\",\"description\":\"\\uD800\"}]",
                strlen("[{\"name\":\"x\",\"version\":\"1\",\"description\":\"\\uD800\"}]"));
    const char invalid_utf8[] = "[{\"name\":\"x\",\"version\":\"1\",\"description\":\"\xc0\x80\"}]";
    reject_json(invalid_utf8, sizeof(invalid_utf8) - 1);
    const char raw_control[] = "[{\"name\":\"x\",\"version\":\"1\",\"description\":\"a\nb\"}]";
    reject_json(raw_control, sizeof(raw_control) - 1);
    const char raw_del[] = "[{\"name\":\"x\",\"version\":\"1\",\"description\":\"\x7f\"}]";
    reject_json(raw_del, sizeof(raw_del) - 1);

    PackageParser parser;
    CHECK(!parser.begin("[]", 64u * 1024u * 1024u + 1u));
    CHECK(!parser.error().empty());
}

static void test_parser_catalog_budget_and_limit() {
    const unsigned count = 300;
    const size_t capacity = static_cast<size_t>(count) * 48 + 4;
    char *json = static_cast<char *>(malloc(capacity));
    CHECK(json != nullptr);
    if (!json) return;
    size_t used = 0;
    json[used++] = '[';
    for (unsigned i = 0; i < count; ++i) {
        int n = snprintf(json + used, capacity - used,
                         "%s{\"name\":\"pkg%u\",\"version\":\"1\"}",
                         i ? "," : "", i);
        CHECK(n > 0 && static_cast<size_t>(n) < capacity - used);
        if (n <= 0 || static_cast<size_t>(n) >= capacity - used) break;
        used += static_cast<size_t>(n);
    }
    json[used++] = ']';
    PackageParser parser;
    CHECK(parser.begin(json, used));
    CHECK(parser.step(17));
    CHECK(!parser.done());
    CHECK(parser.step(17));
    CHECK(!parser.done());
    unsigned steps = 2;
    while (!parser.done() && steps++ < 32) CHECK(parser.step(17));
    CHECK(parser.done());
    Array<Package> packages = parser.take();
    CHECK(packages.size() == count);
    bool found_last_name = false;
    for (size_t i = 0; i < packages.size(); ++i) {
        if (equals(packages[i].name, "pkg299")) found_last_name = true;
        if (i) CHECK(strcmp(packages[i - 1].name.c_str(), packages[i].name.c_str()) < 0);
    }
    CHECK(found_last_name);
    free(json);
}

static void test_parser_byte_budget_large_catalog() {
    const unsigned count = 40;
    const size_t description_size = 4096;
    const size_t capacity = static_cast<size_t>(count) * (description_size + 96) + 2;
    char *json = static_cast<char *>(malloc(capacity));
    CHECK(json != nullptr);
    if (!json) return;
    size_t used = 0;
    json[used++] = '[';
    for (unsigned i = 0; i < count; ++i) {
        const int n = snprintf(json + used, capacity - used,
            "%s{\"name\":\"large%02u\",\"version\":\"1\",\"description\":\"",
            i ? "," : "", i);
        CHECK(n > 0 && static_cast<size_t>(n) < capacity - used);
        if (n <= 0 || static_cast<size_t>(n) >= capacity - used) break;
        used += static_cast<size_t>(n);
        memset(json + used, 'x', description_size);
        used += description_size;
        memcpy(json + used, "\"}", 2);
        used += 2;
    }
    json[used++] = ']';

    PackageParser parser;
    CHECK(parser.begin(json, used));
    CHECK(parser.step(1000));
    CHECK(!parser.done());
    CHECK(parser.take().empty());
    unsigned calls = 1;
    while (!parser.done() && parser.error().empty() && calls < 16) {
        CHECK(parser.step(1000));
        ++calls;
    }
    CHECK(parser.done());
    CHECK(calls > 1);
    Array<Package> packages = parser.take();
    CHECK(packages.size() == count);
    if (packages.size() == count) {
        CHECK(packages[0].description.size() == description_size);
        CHECK(equals(packages[0].name, "large00"));
    }
    free(json);
}

static void test_parser_rejects_oversized_package_incrementally() {
    const char prefix[] =
        "[{\"name\":\"prior\",\"version\":\"1\"},"
        "{\"name\":\"oversized\",\"version\":\"1\"";
    const size_t members = 700000;
    const size_t capacity = sizeof(prefix) - 1 + members * 6 + 2;
    char *json = static_cast<char *>(malloc(capacity));
    CHECK(json != nullptr);
    if (!json) return;
    size_t used = sizeof(prefix) - 1;
    memcpy(json, prefix, used);
    for (size_t i = 0; i < members; ++i) {
        memcpy(json + used, ",\"x\":0", 6);
        used += 6;
    }
    memcpy(json + used, "}]", 2);
    used += 2;

    PackageParser parser;
    CHECK(parser.begin(json, used));
    bool okay = parser.step(100000);
    CHECK(okay);
    CHECK(!parser.done());
    unsigned calls = 1;
    while (okay && parser.error().empty() && calls < 24) {
        okay = parser.step(100000);
        ++calls;
    }
    CHECK(!okay);
    CHECK(parser.error() == "package object exceeds limit");
    CHECK(calls <= 18);
    CHECK(parser.take().empty());
    free(json);
}

static void test_parser_accepts_bounded_large_details() {
    const size_t description_size = 96 * 1024;
    const char prefix[] = "[{\"name\":\"bounded\",\"version\":\"1\",\"description\":\"";
    const size_t capacity = sizeof(prefix) - 1 + description_size + 3;
    char *json = static_cast<char *>(malloc(capacity));
    CHECK(json != nullptr);
    if (!json) return;
    size_t used = sizeof(prefix) - 1;
    memcpy(json, prefix, used);
    memset(json + used, 'd', description_size);
    used += description_size;
    memcpy(json + used, "\"}]", 3);
    used += 3;

    PackageParser parser;
    CHECK(parser.begin(json, used));
    unsigned calls = 0;
    while (!parser.done() && parser.error().empty() && calls < 8) {
        CHECK(parser.step(8));
        ++calls;
    }
    CHECK(parser.done());
    CHECK(calls > 1);
    Array<Package> packages = parser.take();
    CHECK(packages.size() == 1);
    if (packages.size() == 1) CHECK(packages[0].description.size() == description_size);
    free(json);
}

static void test_constraints() {
    CHECK(valid_constraint("apk-tools>=3.0.8-r0"));
    CHECK(valid_constraint("so:libssl.so.3"));
    CHECK(valid_constraint("reliefos-base@community"));
    CHECK(valid_constraint("!busybox"));
    CHECK(valid_constraint("busybox><Q12l/v/T0pPK3aQaUWmI9djiLpF+g="));
    CHECK(valid_constraint("/tmp/relief-addon.apk", true));
    CHECK(!valid_constraint("-f"));
    CHECK(!valid_constraint("apk-tools;touch /tmp/x"));
    CHECK(!valid_constraint("pkg$(id)"));
    CHECK(!valid_constraint("pkg name"));
    CHECK(!valid_constraint("busybox>>1.0"));
    CHECK(!valid_constraint("busybox@"));
    CHECK(!valid_constraint("/tmp/not-a-package", true));
    CHECK(!valid_constraint("relative.apk", true));
}

static void test_command_argv_and_policy() {
    Array<Text> targets;
    CHECK(targets.push(text("apk-tools>=3.0.8-r0")));
    Settings settings;
    settings.offline = true;
    settings.timeout = 11;
    settings.cache_max_age = 240;
    Command command;
    Text error;
    CHECK(make_command(Action::Install, targets, settings, true, command, error));
    CHECK(error.empty());
    CHECK(!command.privileged && !command.mutating && command.cancelable);
    const char *expected[] = {
        "/sbin/apk", "--no-network", "--timeout", "11", "--cache-max-age", "240",
        "--no-interactive", "--no-progress", "add", "--simulate", "--", "apk-tools>=3.0.8-r0"
    };
    CHECK(command.args.size() == sizeof(expected) / sizeof(expected[0]));
    for (size_t i = 0; i < command.args.size() && i < sizeof(expected) / sizeof(expected[0]); ++i)
        CHECK(arg_is(command, i, expected[i]));

    CHECK(make_command(Action::Install, targets, settings, false, command, error));
    CHECK(command.privileged && command.mutating && !command.cancelable);

    Array<Text> hostile;
    CHECK(hostile.push(text("pkg;touch /tmp/x")));
    CHECK(!make_command(Action::Install, hostile, settings, false, command, error));
    CHECK(!error.empty());
    CHECK(command.args.empty());

    const char hidden_suffix[] = {'a','p','k','-','t','o','o','l','s','\0',';','x'};
    Text embedded;
    CHECK(embedded.set(hidden_suffix, sizeof(hidden_suffix)));
    Array<Text> embedded_target;
    CHECK(embedded_target.push(static_cast<Text &&>(embedded)));
    CHECK(!make_command(Action::Install, embedded_target, settings, false, command, error));

    CHECK(query_command(2, settings, command, error));
    CHECK(!command.privileged && !command.mutating && command.cancelable);
    CHECK(arg_is(command, 0, "/sbin/apk"));
    CHECK(arg_is(command, 1, "--no-network"));
    bool has_fields = false, has_upgradable = false;
    bool has_all_names_pattern = false, has_all_matches = false;
    for (size_t i = 0; i < command.args.size(); ++i) {
        if (arg_is(command, i, "--fields") && i + 1 < command.args.size() &&
            equals(command.args[i + 1], "name,version,description,installed-size,file-size"))
            has_fields = true;
        if (arg_is(command, i, "--upgradable")) has_upgradable = true;
        if (arg_is(command, i, "*")) has_all_names_pattern = true;
        if (arg_is(command, i, "--all-matches")) has_all_matches = true;
    }
    CHECK(has_fields && has_upgradable);
    CHECK(has_all_names_pattern && !has_all_matches);

    CHECK(query_command(1, settings, command, error));
    bool installed_filter = false, from_installed = false;
    for (size_t i = 0; i < command.args.size(); ++i) {
        if (arg_is(command, i, "--installed")) installed_filter = true;
        if (arg_is(command, i, "--from") && i + 1 < command.args.size() &&
            arg_is(command, i + 1, "installed")) from_installed = true;
    }
    CHECK(installed_filter && from_installed);
    // apk 3 reports failure when installed-only "status" is requested for a
    // repository package. Keep it confined to the installed snapshot.
    for (int filter = 0; filter < 5; ++filter) {
        CHECK(query_command(filter, settings, command, error));
        bool correct_fields = false;
        for (size_t i = 0; i + 1 < command.args.size(); ++i)
            if (arg_is(command, i, "--fields"))
                correct_fields = arg_is(command, i + 1, filter == 1 ?
                    "name,version,description,installed-size,file-size,status" :
                    "name,version,description,installed-size,file-size");
        CHECK(correct_fields);
    }
    CHECK(!query_command(99, settings, command, error));
    CHECK(!error.empty());

    Array<Text> package_target;
    CHECK(package_target.push(text("apk-tools")));
    CHECK(make_command(Action::ReverseDepends, package_target, settings, false, command, error));
    bool reverse_names = false;
    for (size_t i = 0; i + 1 < command.args.size(); ++i) {
        if (arg_is(command, i, "--fields") &&
            equals(command.args[i + 1], "name,version,reverse-depends:name")) reverse_names = true;
    }
    CHECK(reverse_names);

    CHECK(query_command(3, settings, command, error));
    bool world_filter = false, world_pattern = false;
    for (size_t i = 0; i < command.args.size(); ++i) {
        if (arg_is(command, i, "--world")) world_filter = true;
        if (arg_is(command, i, "*")) world_pattern = true;
    }
    CHECK(world_filter && !world_pattern);
}

static void test_action_mapping_and_size_format() {
    Array<Text> targets;
    Settings settings;
    Command command;
    Text error;
    CHECK(targets.push(text("relief-addon")));
    CHECK(make_command(Action::Reinstall, targets, settings, false, command, error));
    bool is_fix = false, reinstall = false;
    for (size_t i = 0; i < command.args.size(); ++i) {
        if (arg_is(command, i, "fix")) is_fix = true;
        if (arg_is(command, i, "--reinstall")) reinstall = true;
    }
    CHECK(is_fix && reinstall && command.mutating && command.privileged);

    targets.clear();
    CHECK(make_command(Action::UpgradeAll, targets, settings, true, command, error));
    bool upgrade_all = false, available = false, simulated = false;
    for (size_t i = 0; i < command.args.size(); ++i) {
        if (arg_is(command, i, "upgrade")) upgrade_all = true;
        if (arg_is(command, i, "--available")) available = true;
        if (arg_is(command, i, "--simulate")) simulated = true;
    }
    CHECK(upgrade_all && !available && simulated && !command.mutating &&
          !command.privileged && command.cancelable);

    targets.clear();
    CHECK(targets.push(text("relief-addon")));
    CHECK(make_command(Action::Upgrade, targets, settings, true, command, error));
    bool selected_fix = false, selected_upgrade = false, selected_simulate = false;
    for (size_t i = 0; i < command.args.size(); ++i) {
        if (arg_is(command, i, "fix")) selected_fix = true;
        if (arg_is(command, i, "--upgrade")) selected_upgrade = true;
        if (arg_is(command, i, "--simulate")) selected_simulate = true;
    }
    CHECK(selected_fix && selected_upgrade && selected_simulate);
    CHECK(!command.privileged && !command.mutating && command.cancelable);

    CHECK(make_command(Action::Reinstall, targets, settings, false, command, error));
    CHECK(command.privileged && command.mutating && !command.cancelable);

    CHECK(equals(format_size(0), "0 B"));
    CHECK(equals(format_size(1024), "1.0 KiB"));
    CHECK(equals(format_size(1536), "1.5 KiB"));
}

static void test_settings_local_paths_and_fetch() {
    Settings settings;
    settings.cache_dir = text("/var/cache/apk with space");
    Array<Text> targets;
    CHECK(targets.push(text("apk-tools")));
    Command command;
    Text error;
    CHECK(make_command(Action::Install, targets, settings, false, command, error));
    bool cache_dir_before_command = false;
    for (size_t i = 0; i + 2 < command.args.size(); ++i) {
        if (arg_is(command, i, "--cache-dir") &&
            equals(command.args[i + 1], "/var/cache/apk with space") &&
            arg_is(command, i + 2, "add")) cache_dir_before_command = true;
    }
    CHECK(cache_dir_before_command);

    const char *local_apk = "/tmp/救援 addon.apk";
    CHECK(valid_constraint(local_apk, true));
    targets.clear();
    CHECK(targets.push(text(local_apk)));
    CHECK(make_command(Action::Install, targets, settings, false, command, error));
    CHECK(arg_is(command, command.args.size() - 1, local_apk));

    settings.fetch_dependencies = true;
    targets.clear();
    CHECK(targets.push(text("/tmp/fetch packages")));
    CHECK(targets.push(text("apk-tools")));
    CHECK(make_command(Action::Fetch, targets, settings, false, command, error));
    CHECK(!command.privileged && command.mutating && command.cancelable);
    bool recursive = false, output = false, separator = false;
    for (size_t i = 0; i < command.args.size(); ++i) {
        if (arg_is(command, i, "--recursive")) recursive = true;
        if (arg_is(command, i, "--output") && i + 1 < command.args.size() &&
            equals(command.args[i + 1], "/tmp/fetch packages")) output = true;
        if (arg_is(command, i, "--") && i + 1 < command.args.size() &&
            equals(command.args[i + 1], "apk-tools")) separator = true;
    }
    CHECK(recursive && output && separator);

    Settings invalid;
    invalid.timeout = 0;
    CHECK(!make_command(Action::Stats, targets, invalid, false, command, error));
    CHECK(!error.empty());
    invalid.timeout = 30;
    invalid.cache_max_age = UINT32_MAX;
    CHECK(!query_command(0, invalid, command, error));
    CHECK(!error.empty());
    invalid.cache_max_age = 240;
    invalid.cache_dir = text("relative/cache");
    CHECK(!query_command(0, invalid, command, error));
}

int main() {
    test_parser_fields_and_unicode();
    test_parser_budget_and_atomic_error();
    test_parser_sorts_by_name();
    test_parser_scanner_handles_braces_in_strings();
    test_parser_rejects_invalid_json();
    test_parser_catalog_budget_and_limit();
    test_parser_byte_budget_large_catalog();
    test_parser_rejects_oversized_package_incrementally();
    test_parser_accepts_bounded_large_details();
    test_constraints();
    test_command_argv_and_policy();
    test_action_mapping_and_size_format();
    test_settings_local_paths_and_fetch();
    if (failures) {
        fprintf(stderr, "%u ReliefAPK model checks failed\n", failures);
        return 1;
    }
    puts("ReliefAPK model parser and command tests passed");
    return 0;
}
