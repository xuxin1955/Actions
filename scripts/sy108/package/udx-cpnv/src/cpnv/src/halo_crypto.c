/* SPDX-License-Identifier: GPL-2.0-only */
#include "halo.h"
#include "cpnv_io.h"
#include <openssl/sha.h>
#include <openssl/bn.h>
#include <errno.h>

static void wipe(void *p, size_t n)
{
	volatile uint8_t *q = p;
	while (n--)
		*q++ = 0;
}

static int digest_words(const uint8_t *in, unsigned words, uint8_t out[20])
{
	uint8_t bytes[64];
	for (unsigned i = 0; i < words; i++)
		for (unsigned j = 0; j < 4; j++)
			bytes[4 * i + j] = in[4 * i + 3 - j];
	return SHA1(bytes, words * 4, out) ? 0 : -EIO;
}

int halo_nonce_digest(const uint8_t in[16], uint8_t out[20])
{
	return digest_words(in, 4, out);
}

int halo_feature_digest(const uint8_t in[64], uint8_t out[20])
{
	return digest_words(in, 16, out);
}

int halo_material_block(const uint8_t nonce[16], uint32_t word, uint8_t out[128])
{
	unsigned i;
	out[0] = 0;
	out[1] = 1;
	for (i = 2; i < 103; i++)
		out[i] = 0xff;
	out[103] = 0;
	if (halo_nonce_digest(nonce, out + 104))
		return -EIO;
	for (i = 0; i < 4; i++)
		out[124 + i] = (uint8_t)(word >> (24 - 8 * i));
	return 0;
}

int halo_rsa1024_public(const uint8_t sig[128], const uint8_t modulus[128], uint8_t out[128])
{
	if (!sig || !modulus || !out || !modulus[0] || !(modulus[127] & 1))
		return -EINVAL;
	BIGNUM *n = BN_bin2bn(modulus, 128, NULL), *a = BN_bin2bn(sig, 128, NULL);
	BIGNUM *e = BN_new(), *result = BN_new();
	BN_CTX *ctx = BN_CTX_new();
	int error = -ENOMEM;
	if (!n || !a || !e || !result || !ctx)
		goto done;
	error = -EKEYREJECTED;
	if (BN_cmp(a, n) >= 0)
		goto done;
	error = -EIO;
	if (BN_set_word(e, 65537) && BN_mod_exp(result, a, e, n, ctx) &&
	    BN_bn2binpad(result, out, 128) == 128)
		error = 0;
done:
	BN_free(n);
	BN_free(a);
	BN_free(e);
	BN_free(result);
	BN_CTX_free(ctx);
	return error;
}

int halo_material_verify(const uint8_t material[144], const uint8_t modulus[128],
			 uint32_t *peer_clock)
{
	uint8_t block[128], digest[20];
	unsigned diff = 0, i;
	uint32_t word = 0;
	int e;
	if (!material || !modulus || !peer_clock)
		return -EINVAL;
	e = halo_rsa1024_public(material + 16, modulus, block);
	if (e)
		return e;
	diff = block[0] | (unsigned)(block[1] ^ 1) | block[103];
	for (i = 2; i < 103; i++)
		diff |= (unsigned)(block[i] ^ 0xff);
	if (halo_nonce_digest(material, digest))
		return -EIO;
	for (i = 0; i < 20; i++)
		diff |= (unsigned)(digest[i] ^ block[104 + i]);
	for (i = 0; i < 4; i++)
		word = (word << 8) | block[124 + i];
	wipe(block, sizeof(block));
	wipe(digest, sizeof(digest));
	if (diff)
		return -EKEYREJECTED;
	*peer_clock = word;
	return 0; /* No observed freshness check in the AP caller. */
}

#include <openssl/evp.h>
#include <openssl/core_names.h>
#include <openssl/param_build.h>
#include <openssl/rsa.h>
#include <openssl/crypto.h>

int halo_sign_material(const char *path, const uint8_t nonce[16], uint32_t word,
		       uint8_t signature[128])
{
	uint8_t material[256] = {0}, block[128] = {0};
	int error = -EIO;
	BIGNUM *n = NULL, *d = NULL, *e = NULL;
	OSSL_PARAM_BLD *builder = NULL;
	OSSL_PARAM *params = NULL;
	EVP_PKEY_CTX *import = NULL, *sign = NULL;
	EVP_PKEY *key = NULL;
	int count = cpnv_private_read(path, material, sizeof material, true);
	if (count < 0) {
		error = count;
		goto done;
	}
	if ((error = halo_material_block(nonce, word, block)))
		goto done;
	error = -ENOMEM;
	n = BN_bin2bn(material, 128, NULL);
	d = BN_secure_new();
	e = BN_new();
	builder = OSSL_PARAM_BLD_new();
	import = EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL);
	if (!n || !d || !e || !builder || !import || !BN_bin2bn(material + 128, 128, d))
		goto done;
	OPENSSL_cleanse(material, sizeof material);
	error = -EIO;
	if (!BN_set_word(e, 65537) || !OSSL_PARAM_BLD_push_BN(builder, OSSL_PKEY_PARAM_RSA_N, n) ||
	    !OSSL_PARAM_BLD_push_BN(builder, OSSL_PKEY_PARAM_RSA_E, e) ||
	    !OSSL_PARAM_BLD_push_BN(builder, OSSL_PKEY_PARAM_RSA_D, d))
		goto done;
	params = OSSL_PARAM_BLD_to_param(builder);
	if (!params || EVP_PKEY_fromdata_init(import) <= 0 ||
	    EVP_PKEY_fromdata(import, &key, EVP_PKEY_KEYPAIR, params) <= 0)
		goto done;
	sign = EVP_PKEY_CTX_new(key, NULL);
	size_t length = 128;
	if (!sign || EVP_PKEY_sign_init(sign) <= 0 ||
	    EVP_PKEY_CTX_set_rsa_padding(sign, RSA_NO_PADDING) <= 0 ||
	    EVP_PKEY_sign(sign, signature, &length, block, sizeof block) <= 0 || length != 128)
		goto done;
	error = 0;
done:
	if (error)
		OPENSSL_cleanse(signature, 128);
	OPENSSL_cleanse(material, sizeof material);
	OPENSSL_cleanse(block, sizeof block);
	EVP_PKEY_CTX_free(sign);
	EVP_PKEY_CTX_free(import);
	EVP_PKEY_free(key);
	OSSL_PARAM_free(params);
	OSSL_PARAM_BLD_free(builder);
	BN_free(n);
	BN_clear_free(d);
	BN_free(e);
	return error;
}
