/* vim: set tabstop=8 shiftwidth=4 softtabstop=4 expandtab smarttab colorcolumn=80: */
/*
 * Copyright 2016 Red Hat, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "misc.h"
#include <jose/b64.h>
#include <jose/jwk.h>
#include <jose/jwe.h>

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <string.h>

#define CRYPT_NAMES "A128GCM", "A192GCM", "A256GCM"
#define WRAP_NAMES "A128GCMKW", "A192GCMKW", "A256GCMKW"

static bool
resolve(json_t *jwk)
{
    json_auto_t *upd = NULL;
    const char *kty = NULL;
    const char *alg = NULL;
    const char *opa = NULL;
    const char *opb = NULL;
    json_t *bytes = NULL;
    json_int_t len = 0;

    if (json_unpack(jwk, "{s?s,s?s,s?o}",
                    "kty", &kty, "alg", &alg, "bytes", &bytes) == -1)
        return false;

    switch (str2enum(alg, CRYPT_NAMES, WRAP_NAMES, NULL)) {
    case 0: len = 16; opa = "encrypt"; opb = "decrypt"; break;
    case 1: len = 24; opa = "encrypt"; opb = "decrypt"; break;
    case 2: len = 32; opa = "encrypt"; opb = "decrypt"; break;
    case 3: len = 16; opa = "wrapKey"; opb = "unwrapKey"; break;
    case 4: len = 24; opa = "wrapKey"; opb = "unwrapKey"; break;
    case 5: len = 32; opa = "wrapKey"; opb = "unwrapKey"; break;
    default: return true;
    }

    if (!kty && json_object_set_new(jwk, "kty", json_string("oct")) == -1)
        return false;
    if (kty && strcmp(kty, "oct") != 0)
        return false;

    if (!bytes && json_object_set_new(jwk, "bytes", json_integer(len)) == -1)
        return false;
    if (bytes && (!json_is_integer(bytes) || json_integer_value(bytes) != len))
        return false;

    upd = json_pack("{s:s,s:[s,s]}", "use", "enc", "key_ops", opa, opb);
    if (!upd)
        return false;

    return json_object_update_missing(jwk, upd) == 0;
}

static const char *
suggest_crypt(const json_t *jwk)
{
    const char *kty = NULL;
    const char *k = NULL;

    if (json_unpack((json_t *) jwk, "{s:s,s:s}", "kty", &kty, "k", &k) == -1)
        return NULL;

    if (strcmp(kty, "oct") != 0)
        return NULL;

    switch (jose_b64_dlen(strlen(k))) {
    case 16: return "A128GCM";
    case 24: return "A192GCM";
    case 32: return "A256GCM";
    default: return NULL;
    }
}

static const char *
suggest_wrap(const json_t *jwk)
{
    const char *kty = NULL;
    const char *k = NULL;

    if (json_unpack((json_t *) jwk, "{s:s,s:s}", "kty", &kty, "k", &k) == -1)
        return NULL;

    if (strcmp(kty, "oct") != 0)
        return NULL;

    switch (jose_b64_dlen(strlen(k))) {
    case 16: return "A128GCMKW";
    case 24: return "A192GCMKW";
    case 32: return "A256GCMKW";
    default: return NULL;
    }
}

static bool
do_encrypt(const json_t *cek, const uint8_t pt[], size_t ptl,
           const char *enc, const char *prot, const char *aad,
           json_t *ivtgobj, json_t *ctobj, const char *ctn)
{
    const EVP_CIPHER *cph = NULL;
    EVP_CIPHER_CTX *ctx = NULL;
    uint8_t *ky = NULL;
    uint8_t *ct = NULL;
    bool ret = false;
    size_t kyl = 0;
    size_t ctl = 0;
    int len;

    switch (str2enum(enc, CRYPT_NAMES, NULL)) {
    case 0: cph = EVP_aes_128_gcm(); break;
    case 1: cph = EVP_aes_192_gcm(); break;
    case 2: cph = EVP_aes_256_gcm(); break;
    default: return false;
    }

    uint8_t iv[EVP_CIPHER_iv_length(cph)];
    uint8_t tg[16];

    ct = malloc(ptl + EVP_CIPHER_block_size(cph) - 1);
    if (!ct)
        goto egress;

    ky = jose_b64_decode_json(json_object_get(cek, "k"), &kyl);
    if (!ky)
        goto egress;

    if ((int) kyl != EVP_CIPHER_key_length(cph))
        goto egress;

    if (RAND_bytes(iv, sizeof(iv)) <= 0)
        goto egress;

    ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        goto egress;

    if (EVP_EncryptInit(ctx, cph, NULL, NULL) <= 0)
        goto egress;

    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN,
                            sizeof(iv), NULL) <= 0)
        goto egress;

    if (EVP_EncryptInit(ctx, NULL, ky, iv) <= 0)
        goto egress;

    if (prot) {
        if (EVP_EncryptUpdate(ctx, NULL, &len, (uint8_t *) prot,
                              strlen(prot)) <= 0)
            goto egress;
    }

    if (aad) {
        if (EVP_EncryptUpdate(ctx, NULL, &len, (uint8_t *) ".", 1) <= 0)
            goto egress;

        if (EVP_EncryptUpdate(ctx, NULL, &len, (uint8_t *) aad,
                              strlen(aad)) <= 0)
            goto egress;
    }

    if (EVP_EncryptUpdate(ctx, ct, &len, pt, ptl) <= 0)
        goto egress;
    ctl = len;

    if (EVP_EncryptFinal(ctx, &ct[len], &len) <= 0)
        goto egress;
    ctl += len;

    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, sizeof(tg), tg) <= 0)
        goto egress;

    if (json_object_set_new(ivtgobj, "iv",
                            jose_b64_encode_json(iv, sizeof(iv))) == -1)
        goto egress;

    if (json_object_set_new(ctobj, ctn, jose_b64_encode_json(ct, ctl)) == -1)
        goto egress;

    if (json_object_set_new(ivtgobj, "tag",
                            jose_b64_encode_json(tg, sizeof(tg))) == -1)
        goto egress;

    ret = true;

egress:
    EVP_CIPHER_CTX_free(ctx);
    clear_free(ky, kyl);
    free(ct);
    return ret;
}

static bool
encrypt(json_t *jwe, const json_t *cek, const uint8_t pt[], size_t ptl,
        const char *enc, const char *prot, const char *aad)
{
    return do_encrypt(cek, pt, ptl, enc, prot, aad, jwe, jwe, "ciphertext");
}

static uint8_t *
decrypt(const json_t *jwe, const json_t *cek, const char *enc,
        const char *prot, const char *aad, size_t *ptl)
{
    const EVP_CIPHER *cph = NULL;
    EVP_CIPHER_CTX *ctx = NULL;
    uint8_t *ky = NULL;
    uint8_t *iv = NULL;
    uint8_t *ct = NULL;
    uint8_t *tg = NULL;
    uint8_t *pt = NULL;
    size_t kyl = 0;
    size_t ivl = 0;
    size_t ctl = 0;
    size_t tgl = 0;
    int len = 0;

    switch (str2enum(enc, CRYPT_NAMES, NULL)) {
    case 0: cph = EVP_aes_128_gcm(); break;
    case 1: cph = EVP_aes_192_gcm(); break;
    case 2: cph = EVP_aes_256_gcm(); break;
    default: return NULL;
    }

    len = EVP_CIPHER_iv_length(cph);
    ky = jose_b64_decode_json(json_object_get(cek, "k"), &kyl);
    iv = jose_b64_decode_json(json_object_get(jwe, "iv"), &ivl);
    ct = jose_b64_decode_json(json_object_get(jwe, "ciphertext"), &ctl);
    tg = jose_b64_decode_json(json_object_get(jwe, "tag"), &tgl);
    pt = malloc(ctl);
    if (!ky || kyl != (size_t) EVP_CIPHER_key_length(cph) ||
        !iv || ivl != (size_t) EVP_CIPHER_iv_length(cph) ||
        !tg || tgl != 16 || !ct || !pt)
        goto error;

    ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        goto error;

    if (EVP_DecryptInit(ctx, cph, ky, iv) <= 0)
        goto error;

    if (prot) {
        if (EVP_DecryptUpdate(ctx, NULL, &len,
                              (uint8_t *) prot, strlen(prot)) <= 0)
            goto error;
    }

    if (aad) {
        if (EVP_DecryptUpdate(ctx, NULL, &len, (uint8_t *) ".", 1) <= 0)
            goto error;

        if (EVP_DecryptUpdate(ctx, NULL, &len,
                              (uint8_t *) aad, strlen(aad)) <= 0)
            goto error;
    }

    if (EVP_DecryptUpdate(ctx, pt, &len, ct, ctl) <= 0)
        goto error;
    *ptl = len;

    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, tgl, tg) <= 0)
        goto error;

    if (EVP_DecryptFinal(ctx, &pt[len], &len) <= 0)
        goto error;
    *ptl += len;

    memset(ky, 0, kyl);
    EVP_CIPHER_CTX_free(ctx);
    clear_free(ky, kyl);
    free(iv);
    free(ct);
    free(tg);
    return pt;

error:
    EVP_CIPHER_CTX_free(ctx);
    clear_free(pt, *ptl);
    clear_free(ky, kyl);
    free(iv);
    free(ct);
    free(tg);
    return NULL;
}

static bool
wrap(json_t *jwe, json_t *cek, const json_t *jwk, json_t *rcp,
     const char *alg)
{
    uint8_t *pt = NULL;
    json_t *h = NULL;
    bool ret = false;
    size_t ptl = 0;

    if (!json_object_get(cek, "k") && !jose_jwk_generate(cek))
        return false;

    switch (str2enum(alg, WRAP_NAMES, NULL)) {
    case 0: alg = "A128GCM"; break;
    case 1: alg = "A192GCM"; break;
    case 2: alg = "A256GCM"; break;
    default: return false;
    }

    pt = jose_b64_decode_json(json_object_get(cek, "k"), &ptl);
    if (!pt)
        goto egress;

    h = json_object_get(rcp, "header");
    if (!h) {
        if (json_object_set_new(rcp, "header", h = json_object()) == -1)
            goto egress;
    }
    if (!json_is_object(h))
        goto egress;

    ret = do_encrypt(jwk, pt, ptl, alg, "", NULL, h, rcp, "encrypted_key");

egress:
    clear_free(pt, ptl);
    return ret;
}

static bool
unwrap(const json_t *jwe, const json_t *jwk, const json_t *rcp,
       const char *alg, json_t *cek)
{
    json_auto_t *obj = NULL;
    json_auto_t *p = NULL;
    uint8_t *pt = NULL;
    json_t *iv = NULL;
    json_t *ct = NULL;
    json_t *tg = NULL;
    bool ret = false;
    size_t ptl = 0;

    switch (str2enum(alg, WRAP_NAMES, NULL)) {
    case 0: alg = "A128GCM"; break;
    case 1: alg = "A192GCM"; break;
    case 2: alg = "A256GCM"; break;
    default: return false;
    }

    p = json_object_get(jwe, "protected");
    if (p) {
        p = jose_b64_decode_json_load(p);
        if (!p)
            return false;
    }

    if (json_unpack(p, "{s:o}", "iv", &iv) == -1 &&
        json_unpack((json_t *) jwe, "{s:{s:o}}", "unprotected", "iv", &iv) == -1 &&
        json_unpack((json_t *) rcp, "{s:{s:o}}", "header", "iv", &iv) == -1)
        goto egress;

    if (json_unpack(p, "{s:o}", "tag", &tg) == -1 &&
        json_unpack((json_t *) jwe, "{s:{s:o}}", "unprotected", "tag", &tg) == -1 &&
        json_unpack((json_t *) rcp, "{s:{s:o}}", "header", "tag", &tg) == -1)
        goto egress;

    if (json_unpack((json_t *) rcp, "{s:o}", "encrypted_key", &ct) == -1)
        goto egress;

    obj = json_pack("{s:O,s:O,s:O}", "iv", iv, "ciphertext", ct, "tag", tg);
    if (!obj)
        goto egress;

    pt = decrypt(obj, jwk, alg, "", NULL, &ptl);
    if (!pt)
        goto egress;

    if (json_object_set_new(cek, "k", jose_b64_encode_json(pt, ptl)) == -1)
        goto egress;

    ret = true;

egress:
    clear_free(pt, ptl);
    return ret;
}

static void __attribute__((constructor))
constructor(void)
{
    static const char *encs[] = { CRYPT_NAMES, NULL };

    static const char *algs[] = { WRAP_NAMES, NULL };

    static jose_jwk_resolver_t resolver = {
        .resolve = resolve
    };

    static jose_jwe_crypter_t crypter = {
        .encs = encs,
        .suggest = suggest_crypt,
        .encrypt = encrypt,
        .decrypt = decrypt,
    };

    static jose_jwe_wrapper_t wrapper = {
        .algs = algs,
        .suggest = suggest_wrap,
        .wrap = wrap,
        .unwrap = unwrap,
    };

    jose_jwk_register_resolver(&resolver);
    jose_jwe_register_crypter(&crypter);
    jose_jwe_register_wrapper(&wrapper);
}
