/*
 * Copyright 2022 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the Apache License 2.0 (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

#include <openssl/evp.h>
#include <openssl/core_names.h>
#include "../../ssl_local.h"
#include "../record_local.h"
#include "recmethod_local.h"

static int tls13_set_crypto_state(OSSL_RECORD_LAYER *rl, int level,
                                  unsigned char *key, size_t keylen,
                                  unsigned char *iv, size_t ivlen,
                                  unsigned char *mackey, size_t mackeylen,
                                  const EVP_CIPHER *ciph,
                                  size_t taglen,
                                  /* TODO(RECLAYER): This probably should not be an int */
                                  int mactype,
                                  const EVP_MD *md,
                                  const SSL_COMP *comp,
                                  /* TODO(RECLAYER): Remove me */
                                  SSL_CONNECTION *s)
{
    EVP_CIPHER_CTX *ciph_ctx;
    int mode;

    if (ivlen > sizeof(rl->iv)) {
        RLAYERfatal(rl, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        return 0;
    }
    memcpy(rl->iv, iv, ivlen);

    ciph_ctx = rl->enc_read_ctx = EVP_CIPHER_CTX_new();
    if (ciph_ctx == NULL) {
        RLAYERfatal(rl, SSL_AD_INTERNAL_ERROR, ERR_R_MALLOC_FAILURE);
        return 0;
    }

    RECORD_LAYER_reset_read_sequence(&s->rlayer);
    rl->taglen = taglen;

    mode = EVP_CIPHER_get_mode(ciph);

    if (EVP_DecryptInit_ex(ciph_ctx, ciph, NULL, NULL, NULL) <= 0
        || EVP_CIPHER_CTX_ctrl(ciph_ctx, EVP_CTRL_AEAD_SET_IVLEN, ivlen, NULL) <= 0
        || (mode == EVP_CIPH_CCM_MODE
            && EVP_CIPHER_CTX_ctrl(ciph_ctx, EVP_CTRL_AEAD_SET_TAG,  taglen, NULL) <= 0)
        || EVP_DecryptInit_ex(ciph_ctx, NULL, NULL, key, NULL) <= 0) {
        RLAYERfatal(rl, SSL_AD_INTERNAL_ERROR, ERR_R_EVP_LIB);
        return 0;
    }

    return 1;
}

static int tls13_cipher(OSSL_RECORD_LAYER *rl, SSL3_RECORD *recs, size_t n_recs,
                        int sending, SSL_MAC_BUF *mac, size_t macsize,
                        /* TODO(RECLAYER): Remove me */ SSL_CONNECTION *s)
{
    EVP_CIPHER_CTX *ctx;
    unsigned char iv[EVP_MAX_IV_LENGTH], recheader[SSL3_RT_HEADER_LENGTH];
    size_t ivlen, offset, loop, hdrlen;
    unsigned char *staticiv;
    unsigned char *seq;
    int lenu, lenf;
    SSL3_RECORD *rec = &recs[0];
    WPACKET wpkt;
    const EVP_CIPHER *cipher;
    int mode;

    if (n_recs != 1) {
        /* Should not happen */
        RLAYERfatal(rl, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        return 0;
    }

    if (sending) {
        ctx = s->enc_write_ctx;
        staticiv = s->write_iv;
        seq = RECORD_LAYER_get_write_sequence(&s->rlayer);
    } else {
        ctx = rl->enc_read_ctx;
        staticiv = rl->iv;
        seq = RECORD_LAYER_get_read_sequence(&s->rlayer);
    }

    cipher = EVP_CIPHER_CTX_get0_cipher(ctx);
    if (cipher == NULL) {
        RLAYERfatal(rl, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        return 0;
    }
    mode = EVP_CIPHER_get_mode(cipher);

    /*
     * If we're sending an alert and ctx != NULL then we must be forcing
     * plaintext alerts. If we're reading and ctx != NULL then we allow
     * plaintext alerts at certain points in the handshake. If we've got this
     * far then we have already validated that a plaintext alert is ok here.
     */
    if (ctx == NULL || rec->type == SSL3_RT_ALERT) {
        memmove(rec->data, rec->input, rec->length);
        rec->input = rec->data;
        return 1;
    }

    ivlen = EVP_CIPHER_CTX_get_iv_length(ctx);

    if (!sending) {
        /*
         * Take off tag. There must be at least one byte of content type as
         * well as the tag
         */
        if (rec->length < rl->taglen + 1)
            return 0;
        rec->length -= rl->taglen;
    }

    /* Set up IV */
    if (ivlen < SEQ_NUM_SIZE) {
        /* Should not happen */
        RLAYERfatal(rl, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        return 0;
    }
    offset = ivlen - SEQ_NUM_SIZE;
    memcpy(iv, staticiv, offset);
    for (loop = 0; loop < SEQ_NUM_SIZE; loop++)
        iv[offset + loop] = staticiv[offset + loop] ^ seq[loop];

    /* Increment the sequence counter */
    for (loop = SEQ_NUM_SIZE; loop > 0; loop--) {
        ++seq[loop - 1];
        if (seq[loop - 1] != 0)
            break;
    }
    if (loop == 0) {
        /* Sequence has wrapped */
        return 0;
    }

    if (EVP_CipherInit_ex(ctx, NULL, NULL, NULL, iv, sending) <= 0
            || (!sending && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG,
                                                rl->taglen,
                                                rec->data + rec->length) <= 0)) {
        RLAYERfatal(rl, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        return 0;
    }

    /* Set up the AAD */
    if (!WPACKET_init_static_len(&wpkt, recheader, sizeof(recheader), 0)
            || !WPACKET_put_bytes_u8(&wpkt, rec->type)
            || !WPACKET_put_bytes_u16(&wpkt, rec->rec_version)
            || !WPACKET_put_bytes_u16(&wpkt, rec->length + rl->taglen)
            || !WPACKET_get_total_written(&wpkt, &hdrlen)
            || hdrlen != SSL3_RT_HEADER_LENGTH
            || !WPACKET_finish(&wpkt)) {
        RLAYERfatal(rl, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        WPACKET_cleanup(&wpkt);
        return 0;
    }

    /*
     * For CCM we must explicitly set the total plaintext length before we add
     * any AAD.
     */
    if ((mode == EVP_CIPH_CCM_MODE
                 && EVP_CipherUpdate(ctx, NULL, &lenu, NULL,
                                     (unsigned int)rec->length) <= 0)
            || EVP_CipherUpdate(ctx, NULL, &lenu, recheader,
                                sizeof(recheader)) <= 0
            || EVP_CipherUpdate(ctx, rec->data, &lenu, rec->input,
                                (unsigned int)rec->length) <= 0
            || EVP_CipherFinal_ex(ctx, rec->data + lenu, &lenf) <= 0
            || (size_t)(lenu + lenf) != rec->length) {
        return 0;
    }
    if (sending) {
        /* Add the tag */
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, rl->taglen,
                                rec->data + rec->length) <= 0) {
            RLAYERfatal(rl, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
            return 0;
        }
        rec->length += rl->taglen;
    }

    return 1;
}

struct record_functions_st tls_1_3_funcs = {
    tls13_set_crypto_state,
    tls13_cipher,
    NULL
};