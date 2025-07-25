#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "pubnub_internal.h"

#if PUBNUB_USE_SSL

#ifndef ESP_PLATFORM
#error "MBEDTLS is supported only on ESP32 platform. Contact PubNub support for other platforms."
#endif

#include "pbpal.h"
#include "pubnub_log.h"
#include "msstopwatch/msstopwatch.h"
#include "pubnub_api_types.h"
#include "pubnub_internal.h"
#include "pubnub_internal_common.h"
#include "pubnub_pal.h"
#include "pubnub_assert.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <mbedtls/error.h>

#ifdef ESP_PLATFORM
#include "esp_crt_bundle.h"
#endif


static void pbntf_setup(pubnub_t* pb);
static void options_setup(pubnub_t* pb);
static void buffer_setup(pubnub_t* pb);
static void pbpal_mbedtls_close(pubnub_t* pb, bool notify);


void pbpal_init(pubnub_t* pb)
{
    PUBNUB_LOG_DEBUG("pbpal_init()\n");
    memset(&pb->pal, 0, sizeof pb->pal);

    pbntf_setup(pb);
    options_setup(pb);
    buffer_setup(pb);
}


int pbpal_send(pubnub_t* pb, void const* data, size_t n)
{
    PUBNUB_ASSERT_INT_OPT(pb->sock_state, ==, STATE_NONE);
    PUBNUB_LOG_DEBUG("pbpal_send(pb=%p, data=%p, n=%zu)\n", pb, data, n);

    pb->ptr = (uint8_t*)data;
    pb->len = n;
    pb->sock_state = STATE_SENDING_DATA;
    pb->left = sizeof pb->core.http_buf / sizeof pb->core.http_buf[0];

    return pbpal_send_status(pb);
}


int pbpal_send_str(pubnub_t* pb, char const* s)
{
    return pbpal_send(pb, s, strlen(s));
}


enum pubnub_res pbpal_handle_socket_condition(int result, pubnub_t* pb, char const* file, int line)
{
    char reason[100] = {0};

    PUBNUB_LOG_TRACE("pbpal_handle_socket_condition(pb=%p, result=%d, file=%s, line=%d)\n", pb, result, file, line);

    if (pb->pal.ssl == NULL) {
        return pbpal_handle_socket_error(result, pb, file, line);
    }

    PUBNUB_ASSERT(pb->options.useSSL);

    switch(result) {
        case 0: // success
            break;
        case MBEDTLS_ERR_SSL_WANT_READ:
        case MBEDTLS_ERR_SSL_WANT_WRITE:
            if (pbms_active(pb->pal.connection_timer) 
                || (pbms_elapsed(pb->pal.connection_timer) < pb->transaction_timeout_ms)) {
                PUBNUB_LOG_TRACE("pb=%p TLS/SSL_I/O operation should retry\n", pb);
                return PNR_IN_PROGRESS;
            }

            pbpal_mbedtls_close(pb, true);
            PUBNUB_LOG_ERROR("pb=%p TLS/SSL_I/O operation failed, PNR_TIMEOUT\n", pb);

            return PNR_TIMEOUT;
        case MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY:
            pbpal_mbedtls_close(pb, true);
            PUBNUB_LOG_INFO("pb=%p TLS/SSL_I/O peer closed connection\n", pb);

            return PNR_OK;
        default:
            pbpal_mbedtls_close(pb, false);
            mbedtls_strerror(result, reason, sizeof reason);
            PUBNUB_LOG_ERROR("pb=%p TLS/SSL_I/O operation failed, PNR_IO_ERROR: %s\n", pb, reason);

            return PNR_IO_ERROR;
    }

    PUBNUB_LOG_TRACE("pb=%p TLS/SSL_I/O operation successful\n", pb);
    return PNR_OK;
}


int pbpal_send_status(pubnub_t* pb)
{
    int result = 0;

    PUBNUB_LOG_TRACE("pbpal_send_status(pb=%p)\n", pb);

    if (0 == pb->len) {
        PUBNUB_LOG_TRACE("pb=%p pb->len=0, nothing to send\n", pb);
        return 0;
    }

    PUBNUB_LOG_TRACE("STATE = %d\n", pb->sock_state);
    PUBNUB_LOG_TRACE("is sending %d\n", pb->sock_state == STATE_SENDING_DATA);
    PUBNUB_ASSERT(pb->sock_state == STATE_SENDING_DATA);

    PUBNUB_LOG_TRACE("check for null");
    if (NULL == pb->pal.ssl) {
        PUBNUB_LOG_ERROR("pbpal_send_status(pb=%p) called with NULL SSL context\n", pb);
        return -1;
    }
    PUBNUB_LOG_TRACE("not a null");
    
    PUBNUB_LOG_TRACE("will write...");
    if (0 >= (result = mbedtls_ssl_write(pb->pal.ssl, pb->ptr, pb->len))) {
        result = pbpal_handle_socket_condition(result, pb, __FILE__, __LINE__);
    } else {
        PUBNUB_ASSERT_OPT((unsigned)result <= pb->len);
        pb->ptr += result;
        pb->len -= result;
        result = (0 == pb->len) ? 0 : +1;
    }
    PUBNUB_LOG_TRACE("written");

    if (0 >= result) {
        pb->sock_state = STATE_NONE;
        pb->unreadlen = 0;
        pb->ptr = (uint8_t*)pb->core.http_buf;
    }
    PUBNUB_LOG_TRACE("sent");
 
    return result;
}


int pbpal_start_read_line(pubnub_t* pb)
{
    unsigned distance;

    PUBNUB_ASSERT_INT_OPT(pb->sock_state, ==, STATE_NONE);

    if (pb->unreadlen > 0) {
        PUBNUB_ASSERT_OPT((char*)pb->ptr + pb->unreadlen
                          <= pb->core.http_buf + PUBNUB_BUF_MAXLEN);
        memmove(pb->core.http_buf, pb->ptr, pb->unreadlen);
    }
    distance = pb->ptr - (uint8_t*)pb->core.http_buf;
    PUBNUB_ASSERT_UINT(distance + pb->left + pb->unreadlen,
                       ==,
                       sizeof pb->core.http_buf / sizeof pb->core.http_buf[0]);
    pb->ptr -= distance;
    pb->left += distance;

    pb->sock_state = STATE_READ_LINE;

    return +1;
}


enum pubnub_res pbpal_line_read_status(pubnub_t* pb)
{
    PUBNUB_LOG_TRACE("pbpal_line_read_status(pb=%p)\n", pb);
    PUBNUB_ASSERT(STATE_READ_LINE == pb->sock_state);

    for (;;) {
        if (pb->unreadlen == 0) {
            PUBNUB_ASSERT_OPT((char*)pb->ptr + pb->left
                              == pb->core.http_buf + PUBNUB_BUF_MAXLEN);

            int result = mbedtls_ssl_read(pb->pal.ssl, pb->ptr, pb->left);

            if (result <= 0) {
                return pbpal_handle_socket_condition(result, pb, __FILE__, __LINE__);
            }

            PUBNUB_ASSERT_OPT(result <= pb->left);
            PUBNUB_LOG_TRACE("pb=%p have new data of length=%d: %.*s\n",
                             pb,
                             result,
                             result,
                             pb->ptr);

            pb->unreadlen = result;
            pb->left -= result;
        }

        while (pb->unreadlen > 0) {
            char c;

            --pb->unreadlen;

            c = *pb->ptr++;
            if (c == '\n') {
                WATCH_USHORT(pb->unreadlen);
                pb->sock_state = STATE_NONE;

                return PNR_OK;
            }
        }

        if (pb->left == 0) {
            PUBNUB_LOG_ERROR("pbpal_line_read_status(pb=%p): buffer full\n", pb);
            pb->sock_state = STATE_NONE;

            return PNR_RX_BUFF_NOT_EMPTY;
        }

        if (NULL == pb->pal.ssl) {
            PUBNUB_LOG_ERROR("pbpal_line_read_status(pb=%p) called with NULL SSL context\n", pb);
            return PNR_INTERNAL_ERROR;
        }
    }

    return PNR_IN_PROGRESS;
}


int pbpal_read_len(pubnub_t* pb)
{
    return (char*)pb->ptr - pb->core.http_buf;
}


int pbpal_start_read(pubnub_t* pb, size_t n)
{
    unsigned distance;
    PUBNUB_LOG_TRACE("pbpal_start_read(pb=%p, n=%zu)\n", pb, n);

    PUBNUB_ASSERT_UINT_OPT(n, >, 0);
    PUBNUB_ASSERT_INT_OPT(pb->sock_state, ==, STATE_NONE);

    WATCH_USHORT(pb->unreadlen);
    WATCH_USHORT(pb->left);
    if (pb->unreadlen > 0) {
        PUBNUB_ASSERT_OPT((char*)pb->ptr + pb->unreadlen
                          <= pb->core.http_buf + PUBNUB_BUF_MAXLEN);
        memmove(pb->core.http_buf, pb->ptr, pb->unreadlen);
    }
    distance = pb->ptr - (uint8_t*)pb->core.http_buf;
    WATCH_UINT(distance);
    PUBNUB_ASSERT_UINT(distance + pb->unreadlen + pb->left,
                       ==,
                       sizeof pb->core.http_buf / sizeof pb->core.http_buf[0]);
    pb->ptr -= distance;
    pb->left += distance;

    pb->sock_state = STATE_READ;
    pb->len        = n;

    return +1;
}


enum pubnub_res pbpal_read_status(pubnub_t* pb)
{
    int have_read;
    PUBNUB_LOG_TRACE("pbpal_read_status(pb=%p)\n", pb);

    PUBNUB_ASSERT(STATE_READ == pb->sock_state);

    if (NULL == pb->pal.ssl) {
        PUBNUB_LOG_ERROR("pbpal_read_status(pb=%p) called with NULL SSL context\n", pb);
        return PNR_INTERNAL_ERROR;
    }

    for (;;) {
        if (0 == pb->unreadlen) {
            unsigned to_read = pb->len ;
            if (to_read > pb->left) {
                to_read = pb->left;
            }
            PUBNUB_ASSERT_OPT(to_read > 0);

            have_read = mbedtls_ssl_read(pb->pal.ssl, pb->ptr, to_read);

            if (have_read <= 0) {
                return pbpal_handle_socket_condition(have_read, pb, __FILE__, __LINE__);
            }

            PUBNUB_ASSERT_OPT(pb->left >= have_read);
            pb->left -= have_read;
        } else {
            have_read = (pb->unreadlen > pb->len) ? pb->len : pb->unreadlen;
            pb->unreadlen -= have_read;
        }

        pb->len -= have_read;
        pb->ptr += have_read;

        if ((0 == pb->len) || (0 == pb->left)) {
            pb->sock_state = STATE_NONE;

            return PNR_OK;
        }
    }

    return PNR_IN_PROGRESS;
}


bool pbpal_closed(pubnub_t* pb)
{
    return (pb->pal.ssl == NULL) && (pb->pal.socket == SOCKET_INVALID);
}

int pbpal_close(pubnub_t *pb)
{
    pbpal_mbedtls_close(pb, true);

    return 0;
}

void pbpal_forget(pubnub_t *pb)
{
    pbpal_mbedtls_close(pb, false);
}

void pbpal_free(pubnub_t *pb)
{
    pbpal_forget(pb);

    // pb->pal.ssl is freed and set to NULL in forget/close

    if (pb->pal.ssl_config != NULL) {
        mbedtls_ssl_config_free(pb->pal.ssl_config);
        free(pb->pal.ssl_config);
        pb->pal.ssl_config = NULL;
    }

    if (pb->pal.ca_certificates != NULL) {
        mbedtls_x509_crt_free(pb->pal.ca_certificates);
        free(pb->pal.ca_certificates);
        pb->pal.ca_certificates = NULL;
    }

    if (pb->pal.ctr_drbg != NULL) {
        mbedtls_ctr_drbg_free(pb->pal.ctr_drbg);
        free(pb->pal.ctr_drbg);
        pb->pal.ctr_drbg = NULL;
    }

    if (pb->pal.entropy != NULL) {
        mbedtls_entropy_free(pb->pal.entropy);
        free(pb->pal.entropy);
        pb->pal.entropy = NULL;
    }

    if (pb->pal.server_fd != NULL) {
        mbedtls_net_free(pb->pal.server_fd);
        free(pb->pal.server_fd);
        pb->pal.server_fd = NULL;
    }

    if (pb->pal.net != NULL) {
        mbedtls_net_free(pb->pal.net);
        free(pb->pal.net);
        pb->pal.net = NULL;
    }
}

#if !defined(PUBNUB_NTF_RUNTIME_SELECTION)
static void pbntf_setup(pubnub_t* pb)
{
    static bool init_done = false;
    PUBNUB_LOG_TRACE("pbntf_setup()\n");

    if (init_done) {
        PUBNUB_LOG_TRACE("pbntf_setup() already done\n");
        return;
    }

    pbntf_init(pb);
    init_done = true;
}
#else 
static void pbntf_setup(pubnub_t* pb)
{
    bool* init_done = NULL;
    static bool init_sync_done = false;
    static bool init_callback_done = false;

    switch(pb->api_policy) {
        case PNA_SYNC:
            s_init = &s_init_sync;
            break;
        case PNA_CALLBACK:
            s_init = &s_init_callback;
            break;
    }

    PUBNUB_LOG_TRACE("pbntf_setup()\n");

    if (*init_done) {
        PUBNUB_LOG_TRACE("pbntf_setup() already done\n");
        return;
    }

    pbntf_init(pb);
    *init_done = true;
}
#endif


static void options_setup(pubnub_t* pb)
{
    pb->options.useSSL = true;
    pb->options.fallbackSSL = true;
    pb->options.use_system_certificate_store = false;
    pb->options.reuse_SSL_session = false;
    pb->flags.trySSL = false;
    pb->ssl_CAfile = NULL;
    pb->ssl_CApath = NULL; 
    pb->ssl_userPEMcert = NULL;
    pb->sock_state = STATE_NONE;
#if PUBNUB_USE_MULTIPLE_ADDRESSES
    pbpal_multiple_addresses_reset_counters(&pb->spare_addresses);
#endif

}


static void buffer_setup(pubnub_t* pb)
{
    pb->ptr  = (uint8_t*)pb->core.http_buf;
    pb->left = sizeof pb->core.http_buf / sizeof pb->core.http_buf[0];
}


static void pbpal_mbedtls_close(pubnub_t* pb, bool notify)
{
    pb->unreadlen = 0;

    if (pb->pal.ssl != NULL) {
        if (notify) {
            mbedtls_ssl_close_notify(pb->pal.ssl);
        }
        mbedtls_ssl_session_reset(pb->pal.ssl);
        mbedtls_ssl_free(pb->pal.ssl);
        pb->pal.ssl = NULL;
    }

    if (pb->pal.socket != SOCKET_INVALID) {
        shutdown(pb->pal.socket, SHUT_RDWR);
        lwip_close(pb->pal.socket);
        pb->pal.socket = SOCKET_INVALID;
    }

    pb->sock_state = STATE_NONE;
}

#endif /* defined PUBNUB_USE_SSL */
