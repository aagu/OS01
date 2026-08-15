/* OS01 mbedTLS overrides — included AFTER the default config.
 * Build: -DMBEDTLS_USER_CONFIG_FILE='"mbedtls/os01_mbedtls_config.h"'
 */

/* ── Platform ──────────────────────────────────────────────── */
#undef  MBEDTLS_NET_C
#undef  MBEDTLS_TIMING_C
#undef  MBEDTLS_FS_IO
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_NO_DEFAULT_RNG
#define MBEDTLS_PLATFORM_MS_TIME_ALT  /* provide our own mbedtls_ms_time */

/* ── Server not needed ─────────────────────────────────────── */
#undef  MBEDTLS_SSL_SRV_C
#undef  MBEDTLS_SSL_PROTO_TLS1_3
#undef  MBEDTLS_SSL_PROTO_DTLS
#undef  MBEDTLS_SSL_DTLS_CONNECTION_ID
#undef  MBEDTLS_SSL_ALPN
#undef  MBEDTLS_SSL_SESSION_TICKETS
#undef  MBEDTLS_SSL_TRUNCATED_HMAC
#undef  MBEDTLS_SSL_CONTEXT_SERIALIZATION

/* ── Keep only ECDHE key exchange (required by modern TLS servers) */
#undef  MBEDTLS_KEY_EXCHANGE_PSK_ENABLED
#undef  MBEDTLS_KEY_EXCHANGE_DHE_PSK_ENABLED
#undef  MBEDTLS_KEY_EXCHANGE_ECDHE_PSK_ENABLED
#undef  MBEDTLS_KEY_EXCHANGE_RSA_PSK_ENABLED
#undef  MBEDTLS_KEY_EXCHANGE_RSA_ENABLED
#undef  MBEDTLS_KEY_EXCHANGE_DHE_RSA_ENABLED
#undef  MBEDTLS_KEY_EXCHANGE_ECDH_ECDSA_ENABLED
#undef  MBEDTLS_KEY_EXCHANGE_ECDH_RSA_ENABLED
#undef  MBEDTLS_KEY_EXCHANGE_ECJPAKE_ENABLED

/* ── Unused modules ────────────────────────────────────────── */
#undef  MBEDTLS_LMS_C
#undef  MBEDTLS_CHACHAPOLY_C
#undef  MBEDTLS_CHACHA20_C
#undef  MBEDTLS_POLY1305_C
#undef  MBEDTLS_ARIA_C
#undef  MBEDTLS_CAMELLIA_C
#undef  MBEDTLS_DES_C
#undef  MBEDTLS_BLOWFISH_C
#undef  MBEDTLS_RIPEMD160_C
#undef  MBEDTLS_DHM_C
#undef  MBEDTLS_ECJPAKE_C
#undef  MBEDTLS_CMAC_C
#undef  MBEDTLS_CCM_C
#undef  MBEDTLS_NIST_KW_C
#undef  MBEDTLS_PSA_CRYPTO_C
#undef  MBEDTLS_USE_PSA_CRYPTO
#undef  MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef  MBEDTLS_PSA_ITS_FILE_C

/* ── Unused X.509 ──────────────────────────────────────────── */
#undef  MBEDTLS_X509_CRL_PARSE_C
#undef  MBEDTLS_X509_CSR_PARSE_C
#undef  MBEDTLS_X509_CSR_WRITE_C
#undef  MBEDTLS_X509_CREATE_C
#undef  MBEDTLS_X509_CRT_WRITE_C
#undef  MBEDTLS_PKCS7_C

/* ── Debug disabled ────────────────────────────────────────── */
#undef  MBEDTLS_DEBUG_C
