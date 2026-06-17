# Vendored Mbed-TLS, built as a single static library.
#
# Birdie uses the upstream stock config (include/mbedtls/mbedtls_config.h),
# which mbedtls includes automatically, so no MBEDTLS_CONFIG_FILE is needed.
# The three upstream archives (crypto / x509 / tls) are merged here into one
# `mbedtls` library; link order does not matter for a static lib of all
# objects. See UPSTREAM for the vendored revision and scripts/update-mbedtls.sh
# to refresh it.

LIBRARIES += mbedtls

mbedtls_DIR  := $(dir $(lastword $(MAKEFILE_LIST)))
mbedtls_SRCS := $(patsubst $(mbedtls_DIR)%,%,$(wildcard $(mbedtls_DIR)library/*.c))

mbedtls_CPPFLAGS          = -I$(mbedtls_DIR)include
mbedtls_EXPORTED_CPPFLAGS = -I$(mbedtls_DIR)include

# mbedtls is third-party; don't hold it to birdie's -Wall -W (and the extra
# warnings are pure noise here). Keep the project's other flags.
mbedtls_CFLAGS = -w
