/*
 *  SSLv3/TLSv1 server-side functions
 *
 *  Copyright (C) 2006-2013, Brainspark B.V.
 *
 *  This file is part of PolarSSL (http://www.polarssl.org)
 *  Lead Maintainer: Paul Bakker <polarssl_maintainer at polarssl.org>
 *
 *  All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "polarssl/config.h"

#if defined(POLARSSL_SSL_SRV_C)

#include "polarssl/debug.h"
#include "polarssl/ssl.h"
#if defined(POLARSSL_ECP_C)
#include "polarssl/ecp.h"
#endif

#include <stdlib.h>
#include <stdio.h>
#include <time.h>

static int ssl_parse_servername_ext( ssl_context *ssl,
                                     const unsigned char *buf,
                                     size_t len )
{
    int ret;
    size_t servername_list_size, hostname_len;
    const unsigned char *p;

    servername_list_size = ( ( buf[0] << 8 ) | ( buf[1] ) );
    if( servername_list_size + 2 != len )
    {
        SSL_DEBUG_MSG( 1, ( "bad client hello message" ) );
        return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_HELLO );
    }

    p = buf + 2;
    while( servername_list_size > 0 )
    {
        hostname_len = ( ( p[1] << 8 ) | p[2] );
        if( hostname_len + 3 > servername_list_size )
        {
            SSL_DEBUG_MSG( 1, ( "bad client hello message" ) );
            return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_HELLO );
        }

        if( p[0] == TLS_EXT_SERVERNAME_HOSTNAME )
        {
            ret = ssl->f_sni( ssl->p_sni, ssl, p + 3, hostname_len );
            if( ret != 0 )
            {
                ssl_send_alert_message( ssl, SSL_ALERT_LEVEL_FATAL,
                        SSL_ALERT_MSG_UNRECOGNIZED_NAME );
                return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_HELLO );
            }
            return( 0 );
        }

        servername_list_size -= hostname_len + 3;
        p += hostname_len + 3;
    }

    if( servername_list_size != 0 )
    {
        SSL_DEBUG_MSG( 1, ( "bad client hello message" ) );
        return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_HELLO );
    }

    return( 0 );
}

static int ssl_parse_renegotiation_info( ssl_context *ssl,
                                         const unsigned char *buf,
                                         size_t len )
{
    int ret;

    if( ssl->renegotiation == SSL_INITIAL_HANDSHAKE )
    {
        if( len != 1 || buf[0] != 0x0 )
        {
            SSL_DEBUG_MSG( 1, ( "non-zero length renegotiated connection field" ) );

            if( ( ret = ssl_send_fatal_handshake_failure( ssl ) ) != 0 )
                return( ret );

            return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_HELLO );
        }

        ssl->secure_renegotiation = SSL_SECURE_RENEGOTIATION;
    }
    else
    {
        if( len    != 1 + ssl->verify_data_len ||
            buf[0] !=     ssl->verify_data_len ||
            memcmp( buf + 1, ssl->peer_verify_data, ssl->verify_data_len ) != 0 )
        {
            SSL_DEBUG_MSG( 1, ( "non-matching renegotiated connection field" ) );

            if( ( ret = ssl_send_fatal_handshake_failure( ssl ) ) != 0 )
                return( ret );

            return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_HELLO );
        }
    }

    return( 0 );
}

static int ssl_parse_signature_algorithms_ext( ssl_context *ssl,
                                               const unsigned char *buf,
                                               size_t len )
{
    size_t sig_alg_list_size;
    const unsigned char *p;

    sig_alg_list_size = ( ( buf[0] << 8 ) | ( buf[1] ) );
    if( sig_alg_list_size + 2 != len ||
        sig_alg_list_size %2 != 0 )
    {
        SSL_DEBUG_MSG( 1, ( "bad client hello message" ) );
        return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_HELLO );
    }

    p = buf + 2;
    while( sig_alg_list_size > 0 )
    {
        if( p[1] != SSL_SIG_RSA )
        {
            sig_alg_list_size -= 2;
            p += 2;
            continue;
        }
#if defined(POLARSSL_SHA512_C)
        if( p[0] == SSL_HASH_SHA512 )
        {
            ssl->handshake->sig_alg = SSL_HASH_SHA512;
            break;
        }
        if( p[0] == SSL_HASH_SHA384 )
        {
            ssl->handshake->sig_alg = SSL_HASH_SHA384;
            break;
        }
#endif
#if defined(POLARSSL_SHA256_C)
        if( p[0] == SSL_HASH_SHA256 )
        {
            ssl->handshake->sig_alg = SSL_HASH_SHA256;
            break;
        }
        if( p[0] == SSL_HASH_SHA224 )
        {
            ssl->handshake->sig_alg = SSL_HASH_SHA224;
            break;
        }
#endif
        if( p[0] == SSL_HASH_SHA1 )
        {
            ssl->handshake->sig_alg = SSL_HASH_SHA1;
            break;
        }
        if( p[0] == SSL_HASH_MD5 )
        {
            ssl->handshake->sig_alg = SSL_HASH_MD5;
            break;
        }

        sig_alg_list_size -= 2;
        p += 2;
    }

    SSL_DEBUG_MSG( 3, ( "client hello v3, signature_algorithm ext: %d",
                   ssl->handshake->sig_alg ) );

    return( 0 );
}

#if defined(POLARSSL_ECP_C)
static int ssl_parse_supported_elliptic_curves( ssl_context *ssl,
                                                const unsigned char *buf,
                                                size_t len )
{
    size_t list_size;
    const unsigned char *p;

    list_size = ( ( buf[0] << 8 ) | ( buf[1] ) );
    if( list_size + 2 != len ||
        list_size % 2 != 0 )
    {
        SSL_DEBUG_MSG( 1, ( "bad client hello message" ) );
        return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_HELLO );
    }

    p = buf + 2;
    while( list_size > 0 )
    {
#if defined(POLARSSL_ECP_DP_SECP192R1_ENABLED)
        if( p[0] == 0x00 && p[1] == POLARSSL_ECP_DP_SECP192R1 )
        {
            ssl->handshake->ec_curve = p[1];
            return( 0 );
        }
#endif
#if defined(POLARSSL_ECP_DP_SECP224R1_ENABLED)
        if( p[0] == 0x00 && p[1] == POLARSSL_ECP_DP_SECP224R1 )
        {
            ssl->handshake->ec_curve = p[1];
            return( 0 );
        }
#endif
#if defined(POLARSSL_ECP_DP_SECP256R1_ENABLED)
        if( p[0] == 0x00 && p[1] == POLARSSL_ECP_DP_SECP256R1 )
        {
            ssl->handshake->ec_curve = p[1];
            return( 0 );
        }
#endif
#if defined(POLARSSL_ECP_DP_SECP384R1_ENABLED)
        if( p[0] == 0x00 && p[1] == POLARSSL_ECP_DP_SECP384R1 )
        {
            ssl->handshake->ec_curve = p[1];
            return( 0 );
        }
#endif
#if defined(POLARSSL_ECP_DP_SECP521R1_ENABLED)
        if( p[0] == 0x00 && p[1] == POLARSSL_ECP_DP_SECP521R1 )
        {
            ssl->handshake->ec_curve = p[1];
            return( 0 );
        }
#endif

        list_size -= 2;
        p += 2;
    }

    return( 0 );
}

static int ssl_parse_supported_point_formats( ssl_context *ssl,
                                              const unsigned char *buf,
                                              size_t len )
{
    size_t list_size;
    const unsigned char *p;

    list_size = buf[0];
    if( list_size + 1 != len )
    {
        SSL_DEBUG_MSG( 1, ( "bad client hello message" ) );
        return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_HELLO );
    }

    p = buf + 2;
    while( list_size > 0 )
    {
        if( p[0] == POLARSSL_ECP_PF_UNCOMPRESSED ||
            p[0] == POLARSSL_ECP_PF_COMPRESSED )
        {
            ssl->handshake->ec_point_format = p[0];
            return( 0 );
        }

        list_size--;
        p++;
    }

    return( 0 );
}
#endif /* POLARSSL_ECP_C */

#if defined(POLARSSL_SSL_SRV_SUPPORT_SSLV2_CLIENT_HELLO)
static int ssl_parse_client_hello_v2( ssl_context *ssl )
{
    int ret;
    unsigned int i, j;
    size_t n;
    unsigned int ciph_len, sess_len, chal_len;
    unsigned char *buf, *p;
    const int *ciphersuites;
    const ssl_ciphersuite_t *ciphersuite_info;

    SSL_DEBUG_MSG( 2, ( "=> parse client hello v2" ) );

    if( ssl->renegotiation != SSL_INITIAL_HANDSHAKE )
    {
        SSL_DEBUG_MSG( 1, ( "client hello v2 illegal for renegotiation" ) );

        if( ( ret = ssl_send_fatal_handshake_failure( ssl ) ) != 0 )
            return( ret );

        return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_HELLO );
    }

    buf = ssl->in_hdr;

    SSL_DEBUG_BUF( 4, "record header", buf, 5 );

    SSL_DEBUG_MSG( 3, ( "client hello v2, message type: %d",
                   buf[2] ) );
    SSL_DEBUG_MSG( 3, ( "client hello v2, message len.: %d",
                   ( ( buf[0] & 0x7F ) << 8 ) | buf[1] ) );
    SSL_DEBUG_MSG( 3, ( "client hello v2, max. version: [%d:%d]",
                   buf[3], buf[4] ) );

    /*
     * SSLv2 Client Hello
     *
     * Record layer:
     *     0  .   1   message length
     *
     * SSL layer:
     *     2  .   2   message type
     *     3  .   4   protocol version
     */
    if( buf[2] != SSL_HS_CLIENT_HELLO ||
        buf[3] != SSL_MAJOR_VERSION_3 )
    {
        SSL_DEBUG_MSG( 1, ( "bad client hello message" ) );
        return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_HELLO );
    }

    n = ( ( buf[0] << 8 ) | buf[1] ) & 0x7FFF;

    if( n < 17 || n > 512 )
    {
        SSL_DEBUG_MSG( 1, ( "bad client hello message" ) );
        return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_HELLO );
    }

    ssl->major_ver = SSL_MAJOR_VERSION_3;
    ssl->minor_ver = ( buf[4] <= ssl->max_minor_ver )
                     ? buf[4]  : ssl->max_minor_ver;

    if( ssl->minor_ver < ssl->min_minor_ver )
    {
        SSL_DEBUG_MSG( 1, ( "client only supports ssl smaller than minimum"
                            " [%d:%d] < [%d:%d]", ssl->major_ver, ssl->minor_ver,
                            ssl->min_major_ver, ssl->min_minor_ver ) );

        ssl_send_alert_message( ssl, SSL_ALERT_LEVEL_FATAL,
                                     SSL_ALERT_MSG_PROTOCOL_VERSION );
        return( POLARSSL_ERR_SSL_BAD_HS_PROTOCOL_VERSION );
    }

    ssl->handshake->max_major_ver = buf[3];
    ssl->handshake->max_minor_ver = buf[4];

    if( ( ret = ssl_fetch_input( ssl, 2 + n ) ) != 0 )
    {
        SSL_DEBUG_RET( 1, "ssl_fetch_input", ret );
        return( ret );
    }

    ssl->handshake->update_checksum( ssl, buf + 2, n );

    buf = ssl->in_msg;
    n = ssl->in_left - 5;

    /*
     *    0  .   1   ciphersuitelist length
     *    2  .   3   session id length
     *    4  .   5   challenge length
     *    6  .  ..   ciphersuitelist
     *   ..  .  ..   session id
     *   ..  .  ..   challenge
     */
    SSL_DEBUG_BUF( 4, "record contents", buf, n );

    ciph_len = ( buf[0] << 8 ) | buf[1];
    sess_len = ( buf[2] << 8 ) | buf[3];
    chal_len = ( buf[4] << 8 ) | buf[5];

    SSL_DEBUG_MSG( 3, ( "ciph_len: %d, sess_len: %d, chal_len: %d",
                   ciph_len, sess_len, chal_len ) );

    /*
     * Make sure each parameter length is valid
     */
    if( ciph_len < 3 || ( ciph_len % 3 ) != 0 )
    {
        SSL_DEBUG_MSG( 1, ( "bad client hello message" ) );
        return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_HELLO );
    }

    if( sess_len > 32 )
    {
        SSL_DEBUG_MSG( 1, ( "bad client hello message" ) );
        return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_HELLO );
    }

    if( chal_len < 8 || chal_len > 32 )
    {
        SSL_DEBUG_MSG( 1, ( "bad client hello message" ) );
        return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_HELLO );
    }

    if( n != 6 + ciph_len + sess_len + chal_len )
    {
        SSL_DEBUG_MSG( 1, ( "bad client hello message" ) );
        return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_HELLO );
    }

    SSL_DEBUG_BUF( 3, "client hello, ciphersuitelist",
                   buf + 6, ciph_len );
    SSL_DEBUG_BUF( 3, "client hello, session id",
                   buf + 6 + ciph_len, sess_len );
    SSL_DEBUG_BUF( 3, "client hello, challenge",
                   buf + 6 + ciph_len + sess_len, chal_len );

    p = buf + 6 + ciph_len;
    ssl->session_negotiate->length = sess_len;
    memset( ssl->session_negotiate->id, 0, sizeof( ssl->session_negotiate->id ) );
    memcpy( ssl->session_negotiate->id, p, ssl->session_negotiate->length );

    p += sess_len;
    memset( ssl->handshake->randbytes, 0, 64 );
    memcpy( ssl->handshake->randbytes + 32 - chal_len, p, chal_len );

    /*
     * Check for TLS_EMPTY_RENEGOTIATION_INFO_SCSV
     */
    for( i = 0, p = buf + 6; i < ciph_len; i += 3, p += 3 )
    {
        if( p[0] == 0 && p[1] == 0 && p[2] == SSL_EMPTY_RENEGOTIATION_INFO )
        {
            SSL_DEBUG_MSG( 3, ( "received TLS_EMPTY_RENEGOTIATION_INFO " ) );
            if( ssl->renegotiation == SSL_RENEGOTIATION )
            {
                SSL_DEBUG_MSG( 1, ( "received RENEGOTIATION SCSV during renegotiation" ) );

                if( ( ret = ssl_send_fatal_handshake_failure( ssl ) ) != 0 )
                    return( ret );

                return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_HELLO );
            }
            ssl->secure_renegotiation = SSL_SECURE_RENEGOTIATION;
            break;
        }
    }

    ciphersuites = ssl->ciphersuite_list[ssl->minor_ver];
    for( i = 0; ciphersuites[i] != 0; i++ )
    {
        for( j = 0, p = buf + 6; j < ciph_len; j += 3, p += 3 )
        {
            // Only allow non-ECC ciphersuites as we do not have extensions
            //
            if( p[0] == 0 && p[1] == 0 &&
                ( ( ciphersuites[i] >> 8 ) & 0xFF ) == 0 &&
                p[2] == ( ciphersuites[i] & 0xFF ) )
            {
                ciphersuite_info = ssl_ciphersuite_from_id( ciphersuites[i] );

                if( ciphersuite_info == NULL )
                {
                    SSL_DEBUG_MSG( 1, ( "ciphersuite info for %02x not found",
                                   ciphersuites[i] ) );
                    return( POLARSSL_ERR_SSL_BAD_INPUT_DATA );
                }

                if( ciphersuite_info->min_minor_ver > ssl->minor_ver ||
                    ciphersuite_info->max_minor_ver < ssl->minor_ver )
                    continue;

                goto have_ciphersuite_v2;
            }
        }
    }

    SSL_DEBUG_MSG( 1, ( "got no ciphersuites in common" ) );

    return( POLARSSL_ERR_SSL_NO_CIPHER_CHOSEN );

have_ciphersuite_v2:
    ssl->session_negotiate->ciphersuite = ciphersuites[i];
    ssl->transform_negotiate->ciphersuite_info = ciphersuite_info;
    ssl_optimize_checksum( ssl, ssl->transform_negotiate->ciphersuite_info );

    /*
     * SSLv2 Client Hello relevant renegotiation security checks
     */
    if( ssl->secure_renegotiation == SSL_LEGACY_RENEGOTIATION &&
        ssl->allow_legacy_renegotiation == SSL_LEGACY_BREAK_HANDSHAKE )
    {
        SSL_DEBUG_MSG( 1, ( "legacy renegotiation, breaking off handshake" ) );

        if( ( ret = ssl_send_fatal_handshake_failure( ssl ) ) != 0 )
            return( ret );

        return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_HELLO );
    }

    ssl->in_left = 0;
    ssl->state++;

    SSL_DEBUG_MSG( 2, ( "<= parse client hello v2" ) );

    return( 0 );
}
#endif /* POLARSSL_SSL_SRV_SUPPORT_SSLV2_CLIENT_HELLO */

static int ssl_parse_client_hello( ssl_context *ssl )
{
    int ret;
    unsigned int i, j;
    size_t n;
    unsigned int ciph_len, sess_len;
    unsigned int comp_len;
    unsigned int ext_len = 0;
    unsigned char *buf, *p, *ext;
    int renegotiation_info_seen = 0;
    int handshake_failure = 0;
    const int *ciphersuites;
    const ssl_ciphersuite_t *ciphersuite_info;

    SSL_DEBUG_MSG( 2, ( "=> parse client hello" ) );

    if( ssl->renegotiation == SSL_INITIAL_HANDSHAKE &&
        ( ret = ssl_fetch_input( ssl, 5 ) ) != 0 )
    {
        SSL_DEBUG_RET( 1, "ssl_fetch_input", ret );
        return( ret );
    }

    buf = ssl->in_hdr;

#if defined(POLARSSL_SSL_SRV_SUPPORT_SSLV2_CLIENT_HELLO)
    if( ( buf[0] & 0x80 ) != 0 )
        return ssl_parse_client_hello_v2( ssl );
#endif

    SSL_DEBUG_BUF( 4, "record header", buf, 5 );

    SSL_DEBUG_MSG( 3, ( "client hello v3, message type: %d",
                   buf[0] ) );
    SSL_DEBUG_MSG( 3, ( "client hello v3, message len.: %d",
                   ( buf[3] << 8 ) | buf[4] ) );
    SSL_DEBUG_MSG( 3, ( "client hello v3, protocol ver: [%d:%d]",
                   buf[1], buf[2] ) );

    /*
     * SSLv3 Client Hello
     *
     * Record layer:
     *     0  .   0   message type
     *     1  .   2   protocol version
     *     3  .   4   message length
     */
    if( buf[0] != SSL_MSG_HANDSHAKE ||
        buf[1] != SSL_MAJOR_VERSION_3 )
    {
        SSL_DEBUG_MSG( 1, ( "bad client hello message" ) );
        return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_HELLO );
    }

    n = ( buf[3] << 8 ) | buf[4];

    if( n < 45 || n > 512 )
    {
        SSL_DEBUG_MSG( 1, ( "bad client hello message" ) );
        return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_HELLO );
    }

    if( ssl->renegotiation == SSL_INITIAL_HANDSHAKE &&
        ( ret = ssl_fetch_input( ssl, 5 + n ) ) != 0 )
    {
        SSL_DEBUG_RET( 1, "ssl_fetch_input", ret );
        return( ret );
    }

    buf = ssl->in_msg;
    if( !ssl->renegotiation )
        n = ssl->in_left - 5;
    else
        n = ssl->in_msglen;

    ssl->handshake->update_checksum( ssl, buf, n );

    /*
     * SSL layer:
     *     0  .   0   handshake type
     *     1  .   3   handshake length
     *     4  .   5   protocol version
     *     6  .   9   UNIX time()
     *    10  .  37   random bytes
     *    38  .  38   session id length
     *    39  . 38+x  session id
     *   39+x . 40+x  ciphersuitelist length
     *   41+x .  ..   ciphersuitelist
     *    ..  .  ..   compression alg.
     *    ..  .  ..   extensions
     */
    SSL_DEBUG_BUF( 4, "record contents", buf, n );

    SSL_DEBUG_MSG( 3, ( "client hello v3, handshake type: %d",
                   buf[0] ) );
    SSL_DEBUG_MSG( 3, ( "client hello v3, handshake len.: %d",
                   ( buf[1] << 16 ) | ( buf[2] << 8 ) | buf[3] ) );
    SSL_DEBUG_MSG( 3, ( "client hello v3, max. version: [%d:%d]",
                   buf[4], buf[5] ) );

    /*
     * Check the handshake type and protocol version
     */
    if( buf[0] != SSL_HS_CLIENT_HELLO ||
        buf[4] != SSL_MAJOR_VERSION_3 )
    {
        SSL_DEBUG_MSG( 1, ( "bad client hello message" ) );
        return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_HELLO );
    }

    ssl->major_ver = SSL_MAJOR_VERSION_3;
    ssl->minor_ver = ( buf[5] <= ssl->max_minor_ver )
                     ? buf[5]  : ssl->max_minor_ver;

    if( ssl->minor_ver < ssl->min_minor_ver )
    {
        SSL_DEBUG_MSG( 1, ( "client only supports ssl smaller than minimum"
                            " [%d:%d] < [%d:%d]", ssl->major_ver, ssl->minor_ver,
                            ssl->min_major_ver, ssl->min_minor_ver ) );

        ssl_send_alert_message( ssl, SSL_ALERT_LEVEL_FATAL,
                                     SSL_ALERT_MSG_PROTOCOL_VERSION );

        return( POLARSSL_ERR_SSL_BAD_HS_PROTOCOL_VERSION );
    }

    ssl->handshake->max_major_ver = buf[4];
    ssl->handshake->max_minor_ver = buf[5];

    memcpy( ssl->handshake->randbytes, buf + 6, 32 );

    /*
     * Check the handshake message length
     */
    if( buf[1] != 0 || n != (unsigned int) 4 + ( ( buf[2] << 8 ) | buf[3] ) )
    {
        SSL_DEBUG_MSG( 1, ( "bad client hello message" ) );
        return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_HELLO );
    }

    /*
     * Check the session length
     */
    sess_len = buf[38];

    if( sess_len > 32 )
    {
        SSL_DEBUG_MSG( 1, ( "bad client hello message" ) );
        return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_HELLO );
    }

    ssl->session_negotiate->length = sess_len;
    memset( ssl->session_negotiate->id, 0,
            sizeof( ssl->session_negotiate->id ) );
    memcpy( ssl->session_negotiate->id, buf + 39,
            ssl->session_negotiate->length );

    /*
     * Check the ciphersuitelist length
     */
    ciph_len = ( buf[39 + sess_len] << 8 )
             | ( buf[40 + sess_len]      );

    if( ciph_len < 2 || ciph_len > 256 || ( ciph_len % 2 ) != 0 )
    {
        SSL_DEBUG_MSG( 1, ( "bad client hello message" ) );
        return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_HELLO );
    }

    /*
     * Check the compression algorithms length
     */
    comp_len = buf[41 + sess_len + ciph_len];

    if( comp_len < 1 || comp_len > 16 )
    {
        SSL_DEBUG_MSG( 1, ( "bad client hello message" ) );
        return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_HELLO );
    }

    /*
     * Check the extension length
     */
    if( n > 42 + sess_len + ciph_len + comp_len )
    {
        ext_len = ( buf[42 + sess_len + ciph_len + comp_len] << 8 )
                | ( buf[43 + sess_len + ciph_len + comp_len]      );

        if( ( ext_len > 0 && ext_len < 4 ) ||
            n != 44 + sess_len + ciph_len + comp_len + ext_len )
        {
            SSL_DEBUG_MSG( 1, ( "bad client hello message" ) );
            SSL_DEBUG_BUF( 3, "Ext", buf + 44 + sess_len + ciph_len + comp_len, ext_len);
            return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_HELLO );
        }
    }

    ssl->session_negotiate->compression = SSL_COMPRESS_NULL;
#if defined(POLARSSL_ZLIB_SUPPORT)
    for( i = 0; i < comp_len; ++i )
    {
        if( buf[42 + sess_len + ciph_len + i] == SSL_COMPRESS_DEFLATE )
        {
            ssl->session_negotiate->compression = SSL_COMPRESS_DEFLATE;
            break;
        }
    }
#endif

    SSL_DEBUG_BUF( 3, "client hello, random bytes",
                   buf +  6,  32 );
    SSL_DEBUG_BUF( 3, "client hello, session id",
                   buf + 38,  sess_len );
    SSL_DEBUG_BUF( 3, "client hello, ciphersuitelist",
                   buf + 41 + sess_len,  ciph_len );
    SSL_DEBUG_BUF( 3, "client hello, compression",
                   buf + 42 + sess_len + ciph_len, comp_len );

    /*
     * Check for TLS_EMPTY_RENEGOTIATION_INFO_SCSV
     */
    for( i = 0, p = buf + 41 + sess_len; i < ciph_len; i += 2, p += 2 )
    {
        if( p[0] == 0 && p[1] == SSL_EMPTY_RENEGOTIATION_INFO )
        {
            SSL_DEBUG_MSG( 3, ( "received TLS_EMPTY_RENEGOTIATION_INFO " ) );
            if( ssl->renegotiation == SSL_RENEGOTIATION )
            {
                SSL_DEBUG_MSG( 1, ( "received RENEGOTIATION SCSV during renegotiation" ) );

                if( ( ret = ssl_send_fatal_handshake_failure( ssl ) ) != 0 )
                    return( ret );

                return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_HELLO );
            }
            ssl->secure_renegotiation = SSL_SECURE_RENEGOTIATION;
            break;
        }
    }

    ext = buf + 44 + sess_len + ciph_len + comp_len;

    while( ext_len )
    {
        unsigned int ext_id   = ( ( ext[0] <<  8 )
                                | ( ext[1]       ) );
        unsigned int ext_size = ( ( ext[2] <<  8 )
                                | ( ext[3]       ) );

        if( ext_size + 4 > ext_len )
        {
            SSL_DEBUG_MSG( 1, ( "bad client hello message" ) );
            return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_HELLO );
        }
        switch( ext_id )
        {
        case TLS_EXT_SERVERNAME:
            SSL_DEBUG_MSG( 3, ( "found ServerName extension" ) );
            if( ssl->f_sni == NULL )
                break;

            ret = ssl_parse_servername_ext( ssl, ext + 4, ext_size );
            if( ret != 0 )
                return( ret );
            break;

        case TLS_EXT_RENEGOTIATION_INFO:
            SSL_DEBUG_MSG( 3, ( "found renegotiation extension" ) );
            renegotiation_info_seen = 1;

            ret = ssl_parse_renegotiation_info( ssl, ext + 4, ext_size );
            if( ret != 0 )
                return( ret );
            break;

        case TLS_EXT_SIG_ALG:
            SSL_DEBUG_MSG( 3, ( "found signature_algorithms extension" ) );
            if( ssl->renegotiation == SSL_RENEGOTIATION )
                break;

            ret = ssl_parse_signature_algorithms_ext( ssl, ext + 4, ext_size );
            if( ret != 0 )
                return( ret );
            break;

#if defined(POLARSSL_ECP_C)
        case TLS_EXT_SUPPORTED_ELLIPTIC_CURVES:
            SSL_DEBUG_MSG( 3, ( "found supported elliptic curves extension" ) );

            ret = ssl_parse_supported_elliptic_curves( ssl, ext + 4, ext_size );
            if( ret != 0 )
                return( ret );
            break;

        case TLS_EXT_SUPPORTED_POINT_FORMATS:
            SSL_DEBUG_MSG( 3, ( "found supported point formats extension" ) );

            ret = ssl_parse_supported_point_formats( ssl, ext + 4, ext_size );
            if( ret != 0 )
                return( ret );
            break;
#endif /* POLARSSL_ECP_C */

        default:
            SSL_DEBUG_MSG( 3, ( "unknown extension found: %d (ignoring)",
                           ext_id ) );
        }

        ext_len -= 4 + ext_size;
        ext += 4 + ext_size;

        if( ext_len > 0 && ext_len < 4 )
        {
            SSL_DEBUG_MSG( 1, ( "bad client hello message" ) );
            return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_HELLO );
        }
    }

    /*
     * Renegotiation security checks
     */
    if( ssl->secure_renegotiation == SSL_LEGACY_RENEGOTIATION &&
        ssl->allow_legacy_renegotiation == SSL_LEGACY_BREAK_HANDSHAKE )
    {
        SSL_DEBUG_MSG( 1, ( "legacy renegotiation, breaking off handshake" ) );
        handshake_failure = 1;
    }
    else if( ssl->renegotiation == SSL_RENEGOTIATION &&
             ssl->secure_renegotiation == SSL_SECURE_RENEGOTIATION &&
             renegotiation_info_seen == 0 )
    {
        SSL_DEBUG_MSG( 1, ( "renegotiation_info extension missing (secure)" ) );
        handshake_failure = 1;
    }
    else if( ssl->renegotiation == SSL_RENEGOTIATION &&
             ssl->secure_renegotiation == SSL_LEGACY_RENEGOTIATION &&
             ssl->allow_legacy_renegotiation == SSL_LEGACY_NO_RENEGOTIATION )
    {
        SSL_DEBUG_MSG( 1, ( "legacy renegotiation not allowed" ) );
        handshake_failure = 1;
    }
    else if( ssl->renegotiation == SSL_RENEGOTIATION &&
             ssl->secure_renegotiation == SSL_LEGACY_RENEGOTIATION &&
             renegotiation_info_seen == 1 )
    {
        SSL_DEBUG_MSG( 1, ( "renegotiation_info extension present (legacy)" ) );
        handshake_failure = 1;
    }

    if( handshake_failure == 1 )
    {
        if( ( ret = ssl_send_fatal_handshake_failure( ssl ) ) != 0 )
            return( ret );

        return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_HELLO );
    }

    /*
     * Search for a matching ciphersuite
     * (At the end because we need information from the EC-based extensions)
     */
    ciphersuites = ssl->ciphersuite_list[ssl->minor_ver];
    for( i = 0; ciphersuites[i] != 0; i++ )
    {
        for( j = 0, p = buf + 41 + sess_len; j < ciph_len;
            j += 2, p += 2 )
        {
            if( p[0] == ( ( ciphersuites[i] >> 8 ) & 0xFF ) &&
                p[1] == ( ( ciphersuites[i]      ) & 0xFF ) )
            {
                ciphersuite_info = ssl_ciphersuite_from_id( ciphersuites[i] );

                if( ciphersuite_info == NULL )
                {
                    SSL_DEBUG_MSG( 1, ( "ciphersuite info for %02x not found",
                                   ciphersuites[i] ) );
                    return( POLARSSL_ERR_SSL_BAD_INPUT_DATA );
                }

                if( ciphersuite_info->min_minor_ver > ssl->minor_ver ||
                    ciphersuite_info->max_minor_ver < ssl->minor_ver )
                    continue;

                if( ( ciphersuite_info->flags & POLARSSL_CIPHERSUITE_EC ) &&
                    ssl->handshake->ec_curve == 0 )
                    continue;

                goto have_ciphersuite;
            }
        }
    }

    SSL_DEBUG_MSG( 1, ( "got no ciphersuites in common" ) );

    if( ( ret = ssl_send_fatal_handshake_failure( ssl ) ) != 0 )
        return( ret );

    return( POLARSSL_ERR_SSL_NO_CIPHER_CHOSEN );

have_ciphersuite:
    ssl->session_negotiate->ciphersuite = ciphersuites[i];
    ssl->transform_negotiate->ciphersuite_info = ciphersuite_info;
    ssl_optimize_checksum( ssl, ssl->transform_negotiate->ciphersuite_info );

    ssl->in_left = 0;
    ssl->state++;

    SSL_DEBUG_MSG( 2, ( "<= parse client hello" ) );

    return( 0 );
}

static int ssl_write_server_hello( ssl_context *ssl )
{
    time_t t;
    int ret, n;
    size_t ext_len = 0;
    unsigned char *buf, *p;

    SSL_DEBUG_MSG( 2, ( "=> write server hello" ) );

    /*
     *     0  .   0   handshake type
     *     1  .   3   handshake length
     *     4  .   5   protocol version
     *     6  .   9   UNIX time()
     *    10  .  37   random bytes
     */
    buf = ssl->out_msg;
    p = buf + 4;

    *p++ = (unsigned char) ssl->major_ver;
    *p++ = (unsigned char) ssl->minor_ver;

    SSL_DEBUG_MSG( 3, ( "server hello, chosen version: [%d:%d]",
                   buf[4], buf[5] ) );

    t = time( NULL );
    *p++ = (unsigned char)( t >> 24 );
    *p++ = (unsigned char)( t >> 16 );
    *p++ = (unsigned char)( t >>  8 );
    *p++ = (unsigned char)( t       );

    SSL_DEBUG_MSG( 3, ( "server hello, current time: %lu", t ) );

    if( ( ret = ssl->f_rng( ssl->p_rng, p, 28 ) ) != 0 )
        return( ret );

    p += 28;

    memcpy( ssl->handshake->randbytes + 32, buf + 6, 32 );

    SSL_DEBUG_BUF( 3, "server hello, random bytes", buf + 6, 32 );

    /*
     *    38  .  38   session id length
     *    39  . 38+n  session id
     *   39+n . 40+n  chosen ciphersuite
     *   41+n . 41+n  chosen compression alg.
     */
    ssl->session_negotiate->length = n = 32;
    *p++ = (unsigned char) ssl->session_negotiate->length;

    if( ssl->renegotiation != SSL_INITIAL_HANDSHAKE ||
        ssl->f_get_cache == NULL ||
        ssl->f_get_cache( ssl->p_get_cache, ssl->session_negotiate ) != 0 )
    {
        /*
         * Not found, create a new session id
         */
        ssl->handshake->resume = 0;
        ssl->state++;

        if( ( ret = ssl->f_rng( ssl->p_rng, ssl->session_negotiate->id,
                                n ) ) != 0 )
            return( ret );
    }
    else
    {
        /*
         * Found a matching session, resuming it
         */
        ssl->handshake->resume = 1;
        ssl->state = SSL_SERVER_CHANGE_CIPHER_SPEC;

        if( ( ret = ssl_derive_keys( ssl ) ) != 0 )
        {
            SSL_DEBUG_RET( 1, "ssl_derive_keys", ret );
            return( ret );
        }
    }

    memcpy( p, ssl->session_negotiate->id, ssl->session_negotiate->length );
    p += ssl->session_negotiate->length;

    SSL_DEBUG_MSG( 3, ( "server hello, session id len.: %d", n ) );
    SSL_DEBUG_BUF( 3,   "server hello, session id", buf + 39, n );
    SSL_DEBUG_MSG( 3, ( "%s session has been resumed",
                   ssl->handshake->resume ? "a" : "no" ) );

    *p++ = (unsigned char)( ssl->session_negotiate->ciphersuite >> 8 );
    *p++ = (unsigned char)( ssl->session_negotiate->ciphersuite      );
    *p++ = (unsigned char)( ssl->session_negotiate->compression      );

    SSL_DEBUG_MSG( 3, ( "server hello, chosen ciphersuite: %d",
                   ssl->session_negotiate->ciphersuite ) );
    SSL_DEBUG_MSG( 3, ( "server hello, compress alg.: %d",
                   ssl->session_negotiate->compression ) );

    if( ssl->secure_renegotiation == SSL_SECURE_RENEGOTIATION )
    {
        SSL_DEBUG_MSG( 3, ( "server hello, prepping for secure renegotiation extension" ) );
        ext_len += 5 + ssl->verify_data_len * 2;

        SSL_DEBUG_MSG( 3, ( "server hello, total extension length: %d",
                       ext_len ) );

        *p++ = (unsigned char)( ( ext_len >> 8 ) & 0xFF );
        *p++ = (unsigned char)( ( ext_len      ) & 0xFF );

        /*
         * Secure renegotiation
         */
        SSL_DEBUG_MSG( 3, ( "client hello, secure renegotiation extension" ) );

        *p++ = (unsigned char)( ( TLS_EXT_RENEGOTIATION_INFO >> 8 ) & 0xFF );
        *p++ = (unsigned char)( ( TLS_EXT_RENEGOTIATION_INFO      ) & 0xFF );

        *p++ = 0x00;
        *p++ = ( ssl->verify_data_len * 2 + 1 ) & 0xFF;
        *p++ = ssl->verify_data_len * 2 & 0xFF;

        memcpy( p, ssl->peer_verify_data, ssl->verify_data_len );
        p += ssl->verify_data_len;
        memcpy( p, ssl->own_verify_data, ssl->verify_data_len );
        p += ssl->verify_data_len;
    }

    ssl->out_msglen  = p - buf;
    ssl->out_msgtype = SSL_MSG_HANDSHAKE;
    ssl->out_msg[0]  = SSL_HS_SERVER_HELLO;

    ret = ssl_write_record( ssl );

    SSL_DEBUG_MSG( 2, ( "<= write server hello" ) );

    return( ret );
}

#if !defined(POLARSSL_KEY_EXCHANGE_RSA_ENABLED)       && \
    !defined(POLARSSL_KEY_EXCHANGE_DHE_RSA_ENABLED)   && \
    !defined(POLARSSL_KEY_EXCHANGE_ECDHE_RSA_ENABLED)
static int ssl_write_certificate_request( ssl_context *ssl )
{
    int ret = POLARSSL_ERR_SSL_FEATURE_UNAVAILABLE;
    const ssl_ciphersuite_t *ciphersuite_info = ssl->transform_negotiate->ciphersuite_info;

    SSL_DEBUG_MSG( 2, ( "=> write certificate request" ) );

    if( ciphersuite_info->key_exchange == POLARSSL_KEY_EXCHANGE_PSK ||
        ciphersuite_info->key_exchange == POLARSSL_KEY_EXCHANGE_DHE_PSK )
    {
        SSL_DEBUG_MSG( 2, ( "<= skip write certificate request" ) );
        ssl->state++;
        return( 0 );
    }

    return( ret );
}
#else
static int ssl_write_certificate_request( ssl_context *ssl )
{
    int ret = POLARSSL_ERR_SSL_FEATURE_UNAVAILABLE;
    const ssl_ciphersuite_t *ciphersuite_info = ssl->transform_negotiate->ciphersuite_info;
    size_t n = 0, dn_size, total_dn_size;
    unsigned char *buf, *p;
    const x509_cert *crt;

    SSL_DEBUG_MSG( 2, ( "=> write certificate request" ) );

    ssl->state++;

    if( ciphersuite_info->key_exchange == POLARSSL_KEY_EXCHANGE_PSK ||
        ciphersuite_info->key_exchange == POLARSSL_KEY_EXCHANGE_DHE_PSK ||
        ssl->authmode == SSL_VERIFY_NONE )
    {
        SSL_DEBUG_MSG( 2, ( "<= skip write certificate request" ) );
        return( 0 );
    }

    /*
     *     0  .   0   handshake type
     *     1  .   3   handshake length
     *     4  .   4   cert type count
     *     5  .. m-1  cert types
     *     m  .. m+1  sig alg length (TLS 1.2 only)
     *    m+1 .. n-1  SignatureAndHashAlgorithms (TLS 1.2 only) 
     *     n  .. n+1  length of all DNs
     *    n+2 .. n+3  length of DN 1
     *    n+4 .. ...  Distinguished Name #1
     *    ... .. ...  length of DN 2, etc.
     */
    buf = ssl->out_msg;
    p = buf + 4;

    /*
     * At the moment, only RSA certificates are supported
     */
    *p++ = 1;
    *p++ = SSL_CERT_TYPE_RSA_SIGN;

    /*
     * Add signature_algorithms for verify (TLS 1.2)
     * Only add current running algorithm that is already required for
     * requested ciphersuite.
     *
     * Length is always 2
     */
    if( ssl->minor_ver == SSL_MINOR_VERSION_3 )
    {
        ssl->handshake->verify_sig_alg = SSL_HASH_SHA256;

        *p++ = 0;
        *p++ = 2;

        if( ssl->transform_negotiate->ciphersuite_info->mac ==
            POLARSSL_MD_SHA384 )
        {
            ssl->handshake->verify_sig_alg = SSL_HASH_SHA384;
        }

        *p++ = ssl->handshake->verify_sig_alg;
        *p++ = SSL_SIG_RSA;

        n += 4;
    }

    p += 2;
    crt = ssl->ca_chain;

    total_dn_size = 0;
    while( crt != NULL )
    {
        if( p - buf > 4096 )
            break;

        dn_size = crt->subject_raw.len;
        *p++ = (unsigned char)( dn_size >> 8 );
        *p++ = (unsigned char)( dn_size      );
        memcpy( p, crt->subject_raw.p, dn_size );
        p += dn_size;

        SSL_DEBUG_BUF( 3, "requested DN", p, dn_size );

        total_dn_size += 2 + dn_size;
        crt = crt->next;
    }

    ssl->out_msglen  = p - buf;
    ssl->out_msgtype = SSL_MSG_HANDSHAKE;
    ssl->out_msg[0]  = SSL_HS_CERTIFICATE_REQUEST;
    ssl->out_msg[6 + n]  = (unsigned char)( total_dn_size  >> 8 );
    ssl->out_msg[7 + n]  = (unsigned char)( total_dn_size       );

    ret = ssl_write_record( ssl );

    SSL_DEBUG_MSG( 2, ( "<= write certificate request" ) );

    return( ret );
}
#endif /* !POLARSSL_KEY_EXCHANGE_RSA_ENABLED &&
          !POLARSSL_KEY_EXCHANGE_DHE_RSA_ENABLED &&
          !POLARSSL_KEY_EXCHANGE_ECDHE_RSA_ENABLED */

static int ssl_write_server_key_exchange( ssl_context *ssl )
{
    int ret;
    size_t n = 0, len;
    unsigned char hash[64];
    md_type_t md_alg = POLARSSL_MD_NONE;
    unsigned int hashlen = 0;
    unsigned char *p = ssl->out_msg + 4;
    unsigned char *dig_sig = p;
    size_t dig_sig_len = 0;

    const ssl_ciphersuite_t *ciphersuite_info;
    ciphersuite_info = ssl->transform_negotiate->ciphersuite_info;

    SSL_DEBUG_MSG( 2, ( "=> write server key exchange" ) );

    if( ciphersuite_info->key_exchange != POLARSSL_KEY_EXCHANGE_DHE_RSA &&
        ciphersuite_info->key_exchange != POLARSSL_KEY_EXCHANGE_ECDHE_RSA &&
        ciphersuite_info->key_exchange != POLARSSL_KEY_EXCHANGE_DHE_PSK )
    {
        SSL_DEBUG_MSG( 2, ( "<= skip write server key exchange" ) );
        ssl->state++;
        return( 0 );
    }

#if defined(POLARSSL_RSA_C)
    if( ssl->rsa_key == NULL )
    {
        SSL_DEBUG_MSG( 1, ( "got no private key" ) );
        return( POLARSSL_ERR_SSL_PRIVATE_KEY_REQUIRED );
    }
#endif /* POLARSSL_RSA_C */

#if defined(POLARSSL_KEY_EXCHANGE_DHE_PSK_ENABLED)
    if( ciphersuite_info->key_exchange == POLARSSL_KEY_EXCHANGE_DHE_PSK )
    {
        /* TODO: Support identity hints */
        *(p++) = 0x00;
        *(p++) = 0x00;

        n += 2;
    }
#endif /* POLARSSL_KEY_EXCHANGE_DHE_PSK_ENABLED */

#if defined(POLARSSL_KEY_EXCHANGE_DHE_RSA_ENABLED) ||                       \
    defined(POLARSSL_KEY_EXCHANGE_DHE_PSK_ENABLED)
    if( ciphersuite_info->key_exchange == POLARSSL_KEY_EXCHANGE_DHE_RSA ||
        ciphersuite_info->key_exchange == POLARSSL_KEY_EXCHANGE_DHE_PSK )
    {
        /*
         * Ephemeral DH parameters:
         *
         * struct {
         *     opaque dh_p<1..2^16-1>;
         *     opaque dh_g<1..2^16-1>;
         *     opaque dh_Ys<1..2^16-1>;
         * } ServerDHParams;
         */
        if( ( ret = mpi_copy( &ssl->handshake->dhm_ctx.P, &ssl->dhm_P ) ) != 0 ||
            ( ret = mpi_copy( &ssl->handshake->dhm_ctx.G, &ssl->dhm_G ) ) != 0 )
        {
            SSL_DEBUG_RET( 1, "mpi_copy", ret );
            return( ret );
        }

        if( ( ret = dhm_make_params( &ssl->handshake->dhm_ctx,
                                      mpi_size( &ssl->handshake->dhm_ctx.P ),
                                      p,
                                      &len, ssl->f_rng, ssl->p_rng ) ) != 0 )
        {
            SSL_DEBUG_RET( 1, "dhm_make_params", ret );
            return( ret );
        }

        dig_sig = p;
        dig_sig_len = len;

        p += len;
        n += len;

        SSL_DEBUG_MPI( 3, "DHM: X ", &ssl->handshake->dhm_ctx.X  );
        SSL_DEBUG_MPI( 3, "DHM: P ", &ssl->handshake->dhm_ctx.P  );
        SSL_DEBUG_MPI( 3, "DHM: G ", &ssl->handshake->dhm_ctx.G  );
        SSL_DEBUG_MPI( 3, "DHM: GX", &ssl->handshake->dhm_ctx.GX );
    }
#endif /* POLARSSL_KEY_EXCHANGE_DHE_RSA_ENABLED ||
          POLARSSL_KEY_EXCHANGE_DHE_PSK_ENABLED */

#if defined(POLARSSL_KEY_EXCHANGE_ECDHE_RSA_ENABLED)
    if( ciphersuite_info->key_exchange == POLARSSL_KEY_EXCHANGE_ECDHE_RSA )
    {
        /*
         * Ephemeral ECDH parameters:
         *
         * struct {
         *     ECParameters curve_params;
         *     ECPoint      public;
         * } ServerECDHParams;
         */
        ecdh_init( &ssl->handshake->ecdh_ctx );
        if( ( ret = ecp_use_known_dp( &ssl->handshake->ecdh_ctx.grp,
                                       ssl->handshake->ec_curve ) ) != 0 )
        {
            SSL_DEBUG_RET( 1, "ecp_use_known_dp", ret );
            return( ret );
        }

        if( ( ret = ecdh_make_params( &ssl->handshake->ecdh_ctx,
                                      &len,
                                      p,
                                      1000, ssl->f_rng, ssl->p_rng ) ) != 0 )
        {
            SSL_DEBUG_RET( 1, "ecdh_make_params", ret );
            return( ret );
        }

        dig_sig = p;
        dig_sig_len = len;

        p += len;
        n += len;

        SSL_DEBUG_ECP( 3, "ECDH: Q ", &ssl->handshake->ecdh_ctx.Q );
    }
#endif /* POLARSSL_KEY_EXCHANGE_ECDHE_RSA_ENABLED */

#if defined(POLARSSL_KEY_EXCHANGE_DHE_RSA_ENABLED) ||                       \
    defined(POLARSSL_KEY_EXCHANGE_ECDHE_RSA_ENABLED)
    if( ciphersuite_info->key_exchange == POLARSSL_KEY_EXCHANGE_DHE_RSA ||
        ciphersuite_info->key_exchange == POLARSSL_KEY_EXCHANGE_ECDHE_RSA )
    {
        size_t rsa_key_len = 0;

        if( ssl->minor_ver != SSL_MINOR_VERSION_3 )
        {
            md5_context md5;
            sha1_context sha1;

            /*
             * digitally-signed struct {
             *     opaque md5_hash[16];
             *     opaque sha_hash[20];
             * };
             *
             * md5_hash
             *     MD5(ClientHello.random + ServerHello.random
             *                            + ServerParams);
             * sha_hash
             *     SHA(ClientHello.random + ServerHello.random
             *                            + ServerParams);
             */
            md5_starts( &md5 );
            md5_update( &md5, ssl->handshake->randbytes,  64 );
            md5_update( &md5, dig_sig, dig_sig_len );
            md5_finish( &md5, hash );

            sha1_starts( &sha1 );
            sha1_update( &sha1, ssl->handshake->randbytes,  64 );
            sha1_update( &sha1, dig_sig, dig_sig_len );
            sha1_finish( &sha1, hash + 16 );

            hashlen = 36;
            md_alg = POLARSSL_MD_NONE;
        }
        else
        {
            md_context_t ctx;

            /*
             * digitally-signed struct {
             *     opaque client_random[32];
             *     opaque server_random[32];
             *     ServerDHParams params;
             * };
             */
            switch( ssl->handshake->sig_alg )
            {
#if defined(POLARSSL_MD5_C)
                case SSL_HASH_MD5:
                    md_alg = POLARSSL_MD_MD5;
                    break;
#endif
#if defined(POLARSSL_SHA1_C)
                case SSL_HASH_SHA1:
                    md_alg = POLARSSL_MD_SHA1;
                    break;
#endif
#if defined(POLARSSL_SHA256_C)
                case SSL_HASH_SHA224:
                    md_alg = POLARSSL_MD_SHA224;
                    break;
                case SSL_HASH_SHA256:
                    md_alg = POLARSSL_MD_SHA256;
                    break;
#endif
#if defined(POLARSSL_SHA512_C)
                case SSL_HASH_SHA384:
                    md_alg = POLARSSL_MD_SHA384;
                    break;
                case SSL_HASH_SHA512:
                    md_alg = POLARSSL_MD_SHA512;
                    break;
#endif
                default:
                    /* Should never happen */
                    return( -1 );
            }

            if( ( ret = md_init_ctx( &ctx, md_info_from_type( md_alg ) ) ) != 0 )
            {
                SSL_DEBUG_RET( 1, "md_init_ctx", ret );
                return( ret );
            }

            md_starts( &ctx );
            md_update( &ctx, ssl->handshake->randbytes, 64 );
            md_update( &ctx, dig_sig, dig_sig_len );
            md_finish( &ctx, hash );
        }

        SSL_DEBUG_BUF( 3, "parameters hash", hash, hashlen );

        if ( ssl->rsa_key )
            rsa_key_len = ssl->rsa_key_len( ssl->rsa_key );

        if( ssl->minor_ver == SSL_MINOR_VERSION_3 )
        {
            *(p++) = ssl->handshake->sig_alg;
            *(p++) = SSL_SIG_RSA;

            n += 2;
        }

        *(p++) = (unsigned char)( rsa_key_len >> 8 );
        *(p++) = (unsigned char)( rsa_key_len      );
        n += 2;

        if ( ssl->rsa_key )
        {
            ret = ssl->rsa_sign( ssl->rsa_key, ssl->f_rng, ssl->p_rng,
                    RSA_PRIVATE, md_alg, hashlen, hash, p );
        }

        if( ret != 0 )
        {
            SSL_DEBUG_RET( 1, "pkcs1_sign", ret );
            return( ret );
        }

        SSL_DEBUG_BUF( 3, "my RSA sig", p, rsa_key_len );

        p += rsa_key_len;
        n += rsa_key_len;
    }
#endif /* POLARSSL_KEY_EXCHANGE_DHE_RSA_ENABLED) ||
          POLARSSL_KEY_EXCHANGE_ECDHE_RSA_ENABLED */

    ssl->out_msglen  = 4 + n;
    ssl->out_msgtype = SSL_MSG_HANDSHAKE;
    ssl->out_msg[0]  = SSL_HS_SERVER_KEY_EXCHANGE;

    ssl->state++;

    if( ( ret = ssl_write_record( ssl ) ) != 0 )
    {
        SSL_DEBUG_RET( 1, "ssl_write_record", ret );
        return( ret );
    }

    SSL_DEBUG_MSG( 2, ( "<= write server key exchange" ) );

    return( 0 );
}

static int ssl_write_server_hello_done( ssl_context *ssl )
{
    int ret;

    SSL_DEBUG_MSG( 2, ( "=> write server hello done" ) );

    ssl->out_msglen  = 4;
    ssl->out_msgtype = SSL_MSG_HANDSHAKE;
    ssl->out_msg[0]  = SSL_HS_SERVER_HELLO_DONE;

    ssl->state++;

    if( ( ret = ssl_write_record( ssl ) ) != 0 )
    {
        SSL_DEBUG_RET( 1, "ssl_write_record", ret );
        return( ret );
    }

    SSL_DEBUG_MSG( 2, ( "<= write server hello done" ) );

    return( 0 );
}

#if defined(POLARSSL_KEY_EXCHANGE_DHE_RSA_ENABLED) ||                       \
    defined(POLARSSL_KEY_EXCHANGE_DHE_PSK_ENABLED)
static int ssl_parse_client_dh_public( ssl_context *ssl, unsigned char **p,
                                       const unsigned char *end )
{
    int ret = POLARSSL_ERR_SSL_FEATURE_UNAVAILABLE;
    size_t n;

    /*
     * Receive G^Y mod P, premaster = (G^Y)^X mod P
     */
    if( *p + 2 > end )
    {
        SSL_DEBUG_MSG( 1, ( "bad client key exchange message" ) );
        return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_KEY_EXCHANGE );
    }

    n = ( (*p)[0] << 8 ) | (*p)[1];
    *p += 2;

    if( n < 1 || n > ssl->handshake->dhm_ctx.len || *p + n > end )
    {
        SSL_DEBUG_MSG( 1, ( "bad client key exchange message" ) );
        return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_KEY_EXCHANGE );
    }

    if( ( ret = dhm_read_public( &ssl->handshake->dhm_ctx,
                                  *p, n ) ) != 0 )
    {
        SSL_DEBUG_RET( 1, "dhm_read_public", ret );
        return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_KEY_EXCHANGE_RP );
    }

    SSL_DEBUG_MPI( 3, "DHM: GY", &ssl->handshake->dhm_ctx.GY );

    return( ret );
}
#endif /* POLARSSL_KEY_EXCHANGE_DHE_RSA_ENABLED ||
          POLARSSL_KEY_EXCHANGE_DHE_PSK_ENABLED */

#if defined(POLARSSL_KEY_EXCHANGE_ECDHE_RSA_ENABLED)
static int ssl_parse_client_ecdh_public( ssl_context *ssl )
{
    int ret = POLARSSL_ERR_SSL_FEATURE_UNAVAILABLE;
    size_t n;

    /*
     * Receive client public key and calculate premaster
     */
    n = ssl->in_msg[3];

    if( n < 1 || n > mpi_size( &ssl->handshake->ecdh_ctx.grp.P ) * 2 + 2 ||
        n + 4 != ssl->in_hslen )
    {
        SSL_DEBUG_MSG( 1, ( "bad client key exchange message" ) );
        return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_KEY_EXCHANGE );
    }

    if( ( ret = ecdh_read_public( &ssl->handshake->ecdh_ctx,
                                   ssl->in_msg + 4, n ) ) != 0 )
    {
        SSL_DEBUG_RET( 1, "ecdh_read_public", ret );
        return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_KEY_EXCHANGE_RP );
    }

    SSL_DEBUG_ECP( 3, "ECDH: Qp ", &ssl->handshake->ecdh_ctx.Qp );

    return( ret );
}
#endif /* POLARSSL_KEY_EXCHANGE_ECDHE_RSA_ENABLED */

#if defined(POLARSSL_KEY_EXCHANGE_RSA_ENABLED)
static int ssl_parse_encrypted_pms_secret( ssl_context *ssl )
{
    int ret = POLARSSL_ERR_SSL_FEATURE_UNAVAILABLE;
    size_t i, n = 0;

    if( ssl->rsa_key == NULL )
    {
        SSL_DEBUG_MSG( 1, ( "got no private key" ) );
        return( POLARSSL_ERR_SSL_PRIVATE_KEY_REQUIRED );
    }

    /*
     * Decrypt the premaster using own private RSA key
     */
    i = 4;
    if( ssl->rsa_key )
        n = ssl->rsa_key_len( ssl->rsa_key );
    ssl->handshake->pmslen = 48;

    if( ssl->minor_ver != SSL_MINOR_VERSION_0 )
    {
        i += 2;
        if( ssl->in_msg[4] != ( ( n >> 8 ) & 0xFF ) ||
            ssl->in_msg[5] != ( ( n      ) & 0xFF ) )
        {
            SSL_DEBUG_MSG( 1, ( "bad client key exchange message" ) );
            return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_KEY_EXCHANGE );
        }
    }

    if( ssl->in_hslen != i + n )
    {
        SSL_DEBUG_MSG( 1, ( "bad client key exchange message" ) );
        return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_KEY_EXCHANGE );
    }

    if( ssl->rsa_key ) {
        ret = ssl->rsa_decrypt( ssl->rsa_key, RSA_PRIVATE,
                               &ssl->handshake->pmslen,
                                ssl->in_msg + i,
                                ssl->handshake->premaster,
                                sizeof(ssl->handshake->premaster) );
    }

    if( ret != 0 || ssl->handshake->pmslen != 48 ||
        ssl->handshake->premaster[0] != ssl->handshake->max_major_ver ||
        ssl->handshake->premaster[1] != ssl->handshake->max_minor_ver )
    {
        SSL_DEBUG_MSG( 1, ( "bad client key exchange message" ) );

        /*
         * Protection against Bleichenbacher's attack:
         * invalid PKCS#1 v1.5 padding must not cause
         * the connection to end immediately; instead,
         * send a bad_record_mac later in the handshake.
         */
        ssl->handshake->pmslen = 48;

        ret = ssl->f_rng( ssl->p_rng, ssl->handshake->premaster,
                          ssl->handshake->pmslen );
        if( ret != 0 )
            return( ret );
    }

    return( ret );
}
#endif /* POLARSSL_KEY_EXCHANGE_RSA_ENABLED */

#if defined(POLARSSL_KEY_EXCHANGE_PSK_ENABLED) ||                           \
    defined(POLARSSL_KEY_EXCHANGE_DHE_PSK_ENABLED)
static int ssl_parse_client_psk_identity( ssl_context *ssl, unsigned char **p,
                                          const unsigned char *end )
{
    int ret = POLARSSL_ERR_SSL_FEATURE_UNAVAILABLE;
    size_t n;

    if( ssl->psk == NULL || ssl->psk_identity == NULL ||
        ssl->psk_identity_len == 0 || ssl->psk_len == 0 )
    {
        SSL_DEBUG_MSG( 1, ( "got no pre-shared key" ) );
        return( POLARSSL_ERR_SSL_PRIVATE_KEY_REQUIRED );
    }

    /*
     * Receive client pre-shared key identity name
     */
    if( *p + 2 > end )
    {
        SSL_DEBUG_MSG( 1, ( "bad client key exchange message" ) );
        return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_KEY_EXCHANGE );
    }

    n = ( (*p)[0] << 8 ) | (*p)[1];
    *p += 2;

    if( n < 1 || n > 65535 || *p + n > end )
    {
        SSL_DEBUG_MSG( 1, ( "bad client key exchange message" ) );
        return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_KEY_EXCHANGE );
    }

    if( n != ssl->psk_identity_len ||
        memcmp( ssl->psk_identity, *p, n ) != 0 )
    {
        SSL_DEBUG_BUF( 3, "Unknown PSK identity", *p, n );
        return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_KEY_EXCHANGE );
    }

    *p += n;
    ret = 0;

    return( ret );
}
#endif /* POLARSSL_KEY_EXCHANGE_PSK_ENABLED ||
          POLARSSL_KEY_EXCHANGE_DHE_PSK_ENABLED */

static int ssl_parse_client_key_exchange( ssl_context *ssl )
{
    int ret;
    const ssl_ciphersuite_t *ciphersuite_info;
    unsigned char *p, *end;

    ciphersuite_info = ssl->transform_negotiate->ciphersuite_info;

    SSL_DEBUG_MSG( 2, ( "=> parse client key exchange" ) );

    if( ( ret = ssl_read_record( ssl ) ) != 0 )
    {
        SSL_DEBUG_RET( 1, "ssl_read_record", ret );
        return( ret );
    }

    if( ssl->in_msgtype != SSL_MSG_HANDSHAKE )
    {
        SSL_DEBUG_MSG( 1, ( "bad client key exchange message" ) );
        return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_KEY_EXCHANGE );
    }

    if( ssl->in_msg[0] != SSL_HS_CLIENT_KEY_EXCHANGE )
    {
        SSL_DEBUG_MSG( 1, ( "bad client key exchange message" ) );
        return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_KEY_EXCHANGE );
    }

    p = ssl->in_msg + 4;
    end = ssl->in_msg + ssl->in_msglen;

#if defined(POLARSSL_KEY_EXCHANGE_DHE_RSA_ENABLED)
    if( ciphersuite_info->key_exchange == POLARSSL_KEY_EXCHANGE_DHE_RSA )
    {
        if( ( ret = ssl_parse_client_dh_public( ssl, &p, end ) ) != 0 )
        {
            SSL_DEBUG_RET( 1, ( "ssl_parse_client_dh_public" ), ret );
            return( ret );
        }

        ssl->handshake->pmslen = ssl->handshake->dhm_ctx.len;

        if( ( ret = dhm_calc_secret( &ssl->handshake->dhm_ctx,
                                      ssl->handshake->premaster,
                                     &ssl->handshake->pmslen ) ) != 0 )
        {
            SSL_DEBUG_RET( 1, "dhm_calc_secret", ret );
            return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_KEY_EXCHANGE_CS );
        }

        SSL_DEBUG_MPI( 3, "DHM: K ", &ssl->handshake->dhm_ctx.K  );
    }
    else
#endif /* POLARSSL_KEY_EXCHANGE_DHE_RSA_ENABLED */
#if defined(POLARSSL_KEY_EXCHANGE_ECDHE_RSA_ENABLED)
    if( ciphersuite_info->key_exchange == POLARSSL_KEY_EXCHANGE_ECDHE_RSA )
    {
        if( ( ret = ssl_parse_client_ecdh_public( ssl ) ) != 0 )
        {
            SSL_DEBUG_RET( 1, ( "ssl_parse_client_ecdh_public" ), ret );
            return( ret );
        }

        if( ( ret = ecdh_calc_secret( &ssl->handshake->ecdh_ctx,
                                      &ssl->handshake->pmslen,
                                       ssl->handshake->premaster,
                                       POLARSSL_MPI_MAX_SIZE ) ) != 0 )
        {
            SSL_DEBUG_RET( 1, "ecdh_calc_secret", ret );
            return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_KEY_EXCHANGE_CS );
        }

        SSL_DEBUG_MPI( 3, "ECDH: z  ", &ssl->handshake->ecdh_ctx.z );
    }
    else
#endif /* POLARSSL_KEY_EXCHANGE_ECDHE_RSA_ENABLED */
#if defined(POLARSSL_KEY_EXCHANGE_PSK_ENABLED)
    if( ciphersuite_info->key_exchange == POLARSSL_KEY_EXCHANGE_PSK )
    {
        if( ( ret = ssl_parse_client_psk_identity( ssl, &p, end ) ) != 0 )
        {
            SSL_DEBUG_RET( 1, ( "ssl_parse_client_psk_identity" ), ret );
            return( ret );
        }

        // Set up the premaster secret
        //
        p = ssl->handshake->premaster;
        *(p++) = (unsigned char)( ssl->psk_len >> 8 );
        *(p++) = (unsigned char)( ssl->psk_len      );
        p += ssl->psk_len;

        *(p++) = (unsigned char)( ssl->psk_len >> 8 );
        *(p++) = (unsigned char)( ssl->psk_len      );
        memcpy( p, ssl->psk, ssl->psk_len );
        p += ssl->psk_len;

        ssl->handshake->pmslen = 4 + 2 * ssl->psk_len;
    }
    else
#endif /* POLARSSL_KEY_EXCHANGE_PSK_ENABLED */
#if defined(POLARSSL_KEY_EXCHANGE_DHE_PSK_ENABLED)
    if( ciphersuite_info->key_exchange == POLARSSL_KEY_EXCHANGE_DHE_PSK )
    {
        size_t n;

        if( ( ret = ssl_parse_client_psk_identity( ssl, &p, end ) ) != 0 )
        {
            SSL_DEBUG_RET( 1, ( "ssl_parse_client_psk_identity" ), ret );
            return( ret );
        }
        if( ( ret = ssl_parse_client_dh_public( ssl, &p, end ) ) != 0 )
        {
            SSL_DEBUG_RET( 1, ( "ssl_parse_client_dh_public" ), ret );
            return( ret );
        }

        // Set up the premaster secret
        //
        p = ssl->handshake->premaster;
        *(p++) = (unsigned char)( ssl->handshake->dhm_ctx.len >> 8 );
        *(p++) = (unsigned char)( ssl->handshake->dhm_ctx.len      );

        if( ( ret = dhm_calc_secret( &ssl->handshake->dhm_ctx,
                                      p, &n ) ) != 0 )
        {
            SSL_DEBUG_RET( 1, "dhm_calc_secret", ret );
            return( POLARSSL_ERR_SSL_BAD_HS_CLIENT_KEY_EXCHANGE_CS );
        }

        if( n != ssl->handshake->dhm_ctx.len )
        {
            SSL_DEBUG_MSG( 1, ( "dhm_calc_secret result smaller than DHM" ) );
            return( POLARSSL_ERR_SSL_BAD_INPUT_DATA );
        }

        SSL_DEBUG_MPI( 3, "DHM: K ", &ssl->handshake->dhm_ctx.K  );

        p += ssl->handshake->dhm_ctx.len;

        *(p++) = (unsigned char)( ssl->psk_len >> 8 );
        *(p++) = (unsigned char)( ssl->psk_len      );
        memcpy( p, ssl->psk, ssl->psk_len );
        p += ssl->psk_len;

        ssl->handshake->pmslen = 4 + ssl->handshake->dhm_ctx.len + ssl->psk_len;
    }
    else
#endif /* POLARSSL_KEY_EXCHANGE_DHE_PSK_ENABLED */
#if defined(POLARSSL_KEY_EXCHANGE_RSA_ENABLED)
    if( ciphersuite_info->key_exchange == POLARSSL_KEY_EXCHANGE_RSA )
    {
        if( ( ret = ssl_parse_encrypted_pms_secret( ssl ) ) != 0 )
        {
            SSL_DEBUG_RET( 1, ( "ssl_parse_client_ecdh_public" ), ret );
            return( ret );
        }
    }
    else
#endif /* POLARSSL_KEY_EXCHANGE_RSA_ENABLED */
    {
        return( POLARSSL_ERR_SSL_FEATURE_UNAVAILABLE );
    }

    if( ( ret = ssl_derive_keys( ssl ) ) != 0 )
    {
        SSL_DEBUG_RET( 1, "ssl_derive_keys", ret );
        return( ret );
    }

    ssl->state++;

    SSL_DEBUG_MSG( 2, ( "<= parse client key exchange" ) );

    return( 0 );
}

#if !defined(POLARSSL_KEY_EXCHANGE_RSA_ENABLED)       && \
    !defined(POLARSSL_KEY_EXCHANGE_DHE_RSA_ENABLED)   && \
    !defined(POLARSSL_KEY_EXCHANGE_ECDHE_RSA_ENABLED)
static int ssl_parse_certificate_verify( ssl_context *ssl )
{
    int ret = POLARSSL_ERR_SSL_FEATURE_UNAVAILABLE;
    const ssl_ciphersuite_t *ciphersuite_info = ssl->transform_negotiate->ciphersuite_info;

    SSL_DEBUG_MSG( 2, ( "=> parse certificate verify" ) );

    if( ciphersuite_info->key_exchange == POLARSSL_KEY_EXCHANGE_PSK ||
        ciphersuite_info->key_exchange == POLARSSL_KEY_EXCHANGE_DHE_PSK )
    {
        SSL_DEBUG_MSG( 2, ( "<= skip parse certificate verify" ) );
        ssl->state++;
        return( 0 );
    }

    return( ret );
}
#else
static int ssl_parse_certificate_verify( ssl_context *ssl )
{
    int ret = POLARSSL_ERR_SSL_FEATURE_UNAVAILABLE;
    size_t n = 0, n1, n2;
    unsigned char hash[48];
    md_type_t md_alg = POLARSSL_MD_NONE;
    unsigned int hashlen = 0;
    const ssl_ciphersuite_t *ciphersuite_info = ssl->transform_negotiate->ciphersuite_info;

    SSL_DEBUG_MSG( 2, ( "=> parse certificate verify" ) );

    if( ciphersuite_info->key_exchange == POLARSSL_KEY_EXCHANGE_PSK ||
        ciphersuite_info->key_exchange == POLARSSL_KEY_EXCHANGE_DHE_PSK )
    {
        SSL_DEBUG_MSG( 2, ( "<= skip parse certificate verify" ) );
        ssl->state++;
        return( 0 );
    }

    if( ssl->session_negotiate->peer_cert == NULL )
    {
        SSL_DEBUG_MSG( 2, ( "<= skip parse certificate verify" ) );
        ssl->state++;
        return( 0 );
    }

    ssl->handshake->calc_verify( ssl, hash );

    if( ( ret = ssl_read_record( ssl ) ) != 0 )
    {
        SSL_DEBUG_RET( 1, "ssl_read_record", ret );
        return( ret );
    }

    ssl->state++;

    if( ssl->in_msgtype != SSL_MSG_HANDSHAKE )
    {
        SSL_DEBUG_MSG( 1, ( "bad certificate verify message" ) );
        return( POLARSSL_ERR_SSL_BAD_HS_CERTIFICATE_VERIFY );
    }

    if( ssl->in_msg[0] != SSL_HS_CERTIFICATE_VERIFY )
    {
        SSL_DEBUG_MSG( 1, ( "bad certificate verify message" ) );
        return( POLARSSL_ERR_SSL_BAD_HS_CERTIFICATE_VERIFY );
    }

    if( ssl->minor_ver == SSL_MINOR_VERSION_3 )
    {
        /*
         * As server we know we either have SSL_HASH_SHA384 or
         * SSL_HASH_SHA256
         */
        if( ssl->in_msg[4] != ssl->handshake->verify_sig_alg ||
            ssl->in_msg[5] != SSL_SIG_RSA )
        {
            SSL_DEBUG_MSG( 1, ( "peer not adhering to requested sig_alg for verify message" ) );
            return( POLARSSL_ERR_SSL_BAD_HS_CERTIFICATE_VERIFY );
        }

        if( ssl->handshake->verify_sig_alg == SSL_HASH_SHA384 )
            md_alg = POLARSSL_MD_SHA384;
        else
            md_alg = POLARSSL_MD_SHA256;

        n += 2;
    }
    else
    {
        hashlen = 36;
        md_alg = POLARSSL_MD_NONE;
    }

    n1 = ssl->session_negotiate->peer_cert->rsa.len;
    n2 = ( ssl->in_msg[4 + n] << 8 ) | ssl->in_msg[5 + n];

    if( n + n1 + 6 != ssl->in_hslen || n1 != n2 )
    {
        SSL_DEBUG_MSG( 1, ( "bad certificate verify message" ) );
        return( POLARSSL_ERR_SSL_BAD_HS_CERTIFICATE_VERIFY );
    }

    ret = rsa_pkcs1_verify( &ssl->session_negotiate->peer_cert->rsa, RSA_PUBLIC,
                            md_alg, hashlen, hash, ssl->in_msg + 6 + n );
    if( ret != 0 )
    {
        SSL_DEBUG_RET( 1, "rsa_pkcs1_verify", ret );
        return( ret );
    }

    SSL_DEBUG_MSG( 2, ( "<= parse certificate verify" ) );

    return( ret );
}
#endif /* !POLARSSL_KEY_EXCHANGE_RSA_ENABLED &&
          !POLARSSL_KEY_EXCHANGE_DHE_RSA_ENABLED &&
          !POLARSSL_KEY_EXCHANGE_ECDHE_RSA_ENABLED */

/*
 * SSL handshake -- server side -- single step
 */
int ssl_handshake_server_step( ssl_context *ssl )
{
    int ret = 0;

    if( ssl->state == SSL_HANDSHAKE_OVER )
        return( POLARSSL_ERR_SSL_BAD_INPUT_DATA );

    SSL_DEBUG_MSG( 2, ( "server state: %d", ssl->state ) );

    if( ( ret = ssl_flush_output( ssl ) ) != 0 )
        return( ret );

    switch( ssl->state )
    {
        case SSL_HELLO_REQUEST:
            ssl->state = SSL_CLIENT_HELLO;
            break;

        /*
         *  <==   ClientHello
         */
        case SSL_CLIENT_HELLO:
            ret = ssl_parse_client_hello( ssl );
            break;

        /*
         *  ==>   ServerHello
         *        Certificate
         *      ( ServerKeyExchange  )
         *      ( CertificateRequest )
         *        ServerHelloDone
         */
        case SSL_SERVER_HELLO:
            ret = ssl_write_server_hello( ssl );
            break;

        case SSL_SERVER_CERTIFICATE:
            ret = ssl_write_certificate( ssl );
            break;

        case SSL_SERVER_KEY_EXCHANGE:
            ret = ssl_write_server_key_exchange( ssl );
            break;

        case SSL_CERTIFICATE_REQUEST:
            ret = ssl_write_certificate_request( ssl );
            break;

        case SSL_SERVER_HELLO_DONE:
            ret = ssl_write_server_hello_done( ssl );
            break;

        /*
         *  <== ( Certificate/Alert  )
         *        ClientKeyExchange
         *      ( CertificateVerify  )
         *        ChangeCipherSpec
         *        Finished
         */
        case SSL_CLIENT_CERTIFICATE:
            ret = ssl_parse_certificate( ssl );
            break;

        case SSL_CLIENT_KEY_EXCHANGE:
            ret = ssl_parse_client_key_exchange( ssl );
            break;

        case SSL_CERTIFICATE_VERIFY:
            ret = ssl_parse_certificate_verify( ssl );
            break;

        case SSL_CLIENT_CHANGE_CIPHER_SPEC:
            ret = ssl_parse_change_cipher_spec( ssl );
            break;

        case SSL_CLIENT_FINISHED:
            ret = ssl_parse_finished( ssl );
            break;

        /*
         *  ==>   ChangeCipherSpec
         *        Finished
         */
        case SSL_SERVER_CHANGE_CIPHER_SPEC:
            ret = ssl_write_change_cipher_spec( ssl );
            break;

        case SSL_SERVER_FINISHED:
            ret = ssl_write_finished( ssl );
            break;

        case SSL_FLUSH_BUFFERS:
            SSL_DEBUG_MSG( 2, ( "handshake: done" ) );
            ssl->state = SSL_HANDSHAKE_WRAPUP;
            break;

        case SSL_HANDSHAKE_WRAPUP:
            ssl_handshake_wrapup( ssl );
            break;

        default:
            SSL_DEBUG_MSG( 1, ( "invalid state %d", ssl->state ) );
            return( POLARSSL_ERR_SSL_BAD_INPUT_DATA );
    }

    return( ret );
}
#endif
