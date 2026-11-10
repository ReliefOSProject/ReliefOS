/* Exercise the independent APK ABIs through the shipped, relocatable SDK. */
#include <reliefos/tls.h>
#include <mbedtls/gcm.h>
#include <mbedtls/pkcs5.h>
#include <png.h>
#include <sqlite3.h>
#include <zlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int (*volatile tls_entry)(int, const char *, uint32_t, const void *, uint32_t,
                                const void *, uint32_t, char *, uint32_t, uint32_t *) = reliefos_tls_http_exchange;

#define REQUIRE(expr) do { if (!(expr)) { fprintf(stderr, "APK_PROBE_FAIL line=%d %s\n", __LINE__, #expr); return 1; } } while (0)

int main(int argc, char **argv)
{
    REQUIRE(argc == 2 && tls_entry != NULL);
    const unsigned char input[] = "ReliefOS binary dependency roundtrip";
    unsigned char compressed[256], restored[256];
    uLongf compressed_size = sizeof(compressed), restored_size = sizeof(restored);
    REQUIRE(compress2(compressed, &compressed_size, input, sizeof(input), 6) == Z_OK);
    REQUIRE(uncompress(restored, &restored_size, compressed, compressed_size) == Z_OK);
    REQUIRE(restored_size == sizeof(input) && !memcmp(input, restored, restored_size));

    png_image image = {0};
    image.version = PNG_IMAGE_VERSION; image.width = image.height = 1; image.format = PNG_FORMAT_RGBA;
    const unsigned char pixel[] = {17, 34, 51, 255};
    unsigned char png[512], decoded[4];
    png_alloc_size_t png_size = sizeof(png);
    REQUIRE(png_image_write_to_memory(&image, png, &png_size, 0, pixel, 0, NULL));
    png_image_free(&image); memset(&image, 0, sizeof(image)); image.version = PNG_IMAGE_VERSION;
    REQUIRE(png_image_begin_read_from_memory(&image, png, png_size));
    image.format = PNG_FORMAT_RGBA;
    REQUIRE(png_image_finish_read(&image, NULL, decoded, 0, NULL));
    REQUIRE(!memcmp(decoded, pixel, sizeof(pixel))); png_image_free(&image);

    unsigned char key[32], cipher[sizeof(input)], plain[sizeof(input)], tag[16], nonce[12] = {0};
    REQUIRE(mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, input, sizeof(input), pixel, sizeof(pixel), 100, sizeof(key), key) == 0);
    mbedtls_gcm_context gcm; mbedtls_gcm_init(&gcm);
    REQUIRE(mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256) == 0);
    REQUIRE(mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, sizeof(input), nonce, sizeof(nonce), NULL, 0, input, cipher, sizeof(tag), tag) == 0);
    REQUIRE(mbedtls_gcm_auth_decrypt(&gcm, sizeof(input), nonce, sizeof(nonce), NULL, 0, tag, sizeof(tag), cipher, plain) == 0);
    REQUIRE(!memcmp(plain, input, sizeof(input))); tag[0] ^= 1;
    REQUIRE(mbedtls_gcm_auth_decrypt(&gcm, sizeof(input), nonce, sizeof(nonce), NULL, 0, tag, sizeof(tag), cipher, plain) == MBEDTLS_ERR_GCM_AUTH_FAILED);
    mbedtls_gcm_free(&gcm);

    sqlite3 *db = NULL, *other = NULL, *snapshot = NULL;
    REQUIRE(sqlite3_open(argv[1], &db) == SQLITE_OK);
    REQUIRE(sqlite3_exec(db, "PRAGMA mmap_size=0;PRAGMA journal_mode=DELETE;PRAGMA synchronous=FULL;CREATE TABLE IF NOT EXISTS probe(v);DELETE FROM probe;INSERT INTO probe VALUES(42);", NULL, NULL, NULL) == SQLITE_OK);
    REQUIRE(sqlite3_open(argv[1], &other) == SQLITE_OK);
    REQUIRE(sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
    REQUIRE(sqlite3_exec(other, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_BUSY);
    REQUIRE(sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);
    REQUIRE(sqlite3_close(other) == SQLITE_OK);
    sqlite3_int64 size = 0; unsigned char *serialized = sqlite3_serialize(db, "main", &size, 0);
    REQUIRE(serialized && size > 0);
    REQUIRE(sqlite3_open(":memory:", &snapshot) == SQLITE_OK);
    REQUIRE(sqlite3_deserialize(snapshot, "main", serialized, size, size, SQLITE_DESERIALIZE_FREEONCLOSE) == SQLITE_OK);
    sqlite3_stmt *statement = NULL;
    REQUIRE(sqlite3_prepare_v2(snapshot, "SELECT v FROM probe", -1, &statement, NULL) == SQLITE_OK);
    REQUIRE(sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_int(statement, 0) == 42);
    REQUIRE(sqlite3_finalize(statement) == SQLITE_OK); REQUIRE(sqlite3_close(snapshot) == SQLITE_OK);
    REQUIRE(sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &statement, NULL) == SQLITE_OK);
    REQUIRE(sqlite3_step(statement) == SQLITE_ROW && !strcmp((const char *)sqlite3_column_text(statement, 0), "ok"));
    REQUIRE(sqlite3_finalize(statement) == SQLITE_OK); REQUIRE(sqlite3_close(db) == SQLITE_OK);
    REQUIRE(unlink(argv[1]) == 0);
    printf("APK_PROBE_OK zlib=%s png=%s sqlite=%s crypto=pbkdf2+gcm+badtag lock=busy serialize=roundtrip\n", zlibVersion(), PNG_LIBPNG_VER_STRING, sqlite3_libversion());
    return 0;
}
