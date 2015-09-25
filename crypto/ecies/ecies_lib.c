#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/ecdh.h>
#include "ecies.h"
#include "kdf.h"


ECIES_CIPHERTEXT_VALUE *ECIES_do_encrypt(const ECIES_PARAMS *param,
	const unsigned char *in, size_t inlen, const EC_KEY *pub_key)
{
	int e = 1;
	ECIES_CIPHERTEXT_VALUE *cv = NULL;
	EC_KEY *ephem_key = NULL;
	unsigned char *share = NULL;
	unsigned char *enckey, *mackey, *p;
	int sharelen, enckeylen, mackeylen, len;

	EVP_CIPHER_CTX cipher_ctx;
	EVP_CIPHER_CTX_init(&cipher_ctx);

	if (!(cv = ECIES_CIPHERTEXT_VALUE_new())) 
		{
		ECIESerr(ECIES_F_ECIES_DO_ENCRYPT, ERR_R_MALLOC_FAILURE);
		goto err;
		}
	/*
	 * generate and encode ephem_point
	 */
	if (!(ephem_key = EC_KEY_new()))
		{
		ECIESerr(ECIES_F_ECIES_DO_ENCRYPT, ERR_R_MALLOC_FAILURE);
		goto err;
		}
	if (!EC_KEY_set_group(ephem_key, EC_KEY_get0_group(pub_key)))
		{
		ECIESerr(ECIES_F_ECIES_DO_ENCRYPT, ERR_R_EC_LIB);
		goto err;
		}
	if (!EC_KEY_generate_key(ephem_key))
		{
		ECIESerr(ECIES_F_ECIES_DO_ENCRYPT, ERR_R_EC_LIB);
		goto err;
		}

	len = (int)EC_POINT_point2oct(EC_KEY_get0_group(ephem_key),
		EC_KEY_get0_public_key(ephem_key), POINT_CONVERSION_COMPRESSED,
		NULL, 0, NULL);
	if (!M_ASN1_OCTET_STRING_set(cv->ephem_point, NULL, len))
		{
		ECIESerr(ECIES_F_ECIES_DO_ENCRYPT, ERR_R_ASN1_LIB);
		goto err;
		}
	if (EC_POINT_point2oct(EC_KEY_get0_group(ephem_key),
		EC_KEY_get0_public_key(ephem_key), POINT_CONVERSION_COMPRESSED,
		cv->ephem_point->data, len, NULL) <= 0)
		{
		ECIESerr(ECIES_F_ECIES_DO_ENCRYPT, ERR_R_EC_LIB);
		goto err;
		}		

	/*
	 * use ecdh to compute enckey and mackey
	 */
	if (param->sym_cipher)
		enckeylen = EVP_CIPHER_key_length(param->sym_cipher);
	else	enckeylen = inlen;
	mackeylen = EVP_MD_size(param->mac_md); //TODO: is this true for hmac-half-ecies?
	sharelen = enckeylen + mackeylen;

	if (!(share = OPENSSL_malloc(sharelen)))
		{
		ECIESerr(ECIES_F_ECIES_DO_ENCRYPT, ERR_R_MALLOC_FAILURE);
		goto err;
		}

	if (!ECDH_compute_key(share, sharelen, 
		EC_KEY_get0_public_key(pub_key), ephem_key,
		KDF_get_x9_63(param->kdf_md)))
		{
		ECIESerr(ECIES_F_ECIES_DO_ENCRYPT, ECIES_R_ECDH_FAILED);
		goto err;
		}
	enckey = share;
	mackey = share + enckeylen;

	/*
	 * encrypt data and encode result to ciphertext
	 */
	if (param->sym_cipher)
		len = (int)(inlen + EVP_MAX_BLOCK_LENGTH * 2);
	else	len = inlen;
	
	if (!M_ASN1_OCTET_STRING_set(cv->ciphertext, NULL, len))
		{
		ECIESerr(ECIES_F_ECIES_DO_ENCRYPT, ERR_R_MALLOC_FAILURE);
		goto err;
		}
	
	if (param->sym_cipher) 
		{
		unsigned char iv[EVP_MAX_IV_LENGTH];
		memset(iv, 0, sizeof(iv));

		if (!EVP_EncryptInit(&cipher_ctx, param->sym_cipher, enckey, iv))
			{
			ECIESerr(ECIES_F_ECIES_DO_ENCRYPT,
				ECIES_R_ENCRYPT_FAILED);
			goto err;
			}
		p = cv->ciphertext->data;
		if (!EVP_EncryptUpdate(&cipher_ctx, p, &len, in, (int)inlen)) 
			{
			ECIESerr(ECIES_F_ECIES_DO_ENCRYPT,
				ECIES_R_ENCRYPT_FAILED);
			goto err;
			}
		p += len;
		if (!EVP_EncryptFinal(&cipher_ctx, p, &len))
			{
			ECIESerr(ECIES_F_ECIES_DO_ENCRYPT,
				ECIES_R_ENCRYPT_FAILED);
			goto err;
			}
		p += len;
		cv->ciphertext->length = (int)(p - cv->ciphertext->data);		
		}
	else
		{
		int i;
		for (i = 0; i < len; i++)
			cv->ciphertext->data[i] = in[i] ^ enckey[i];
		cv->ciphertext->length = len;
		}

	/*
	 * calculate mactag of ciphertext and encode
	 */
	cv->mactag->length = EVP_MD_size(param->mac_md);
	
	if (!M_ASN1_OCTET_STRING_set(cv->mactag, NULL, EVP_MD_size(param->mac_md)))
		{
		ECIESerr(ECIES_F_ECIES_DO_ENCRYPT, ERR_R_MALLOC_FAILURE);
		goto err;		
		}
	if (!HMAC(param->mac_md, mackey, mackeylen,
		cv->ciphertext->data, (size_t)cv->ciphertext->length,
		cv->mactag->data, (unsigned int *)&len))
		{
		ECIESerr(ECIES_F_ECIES_DO_ENCRYPT, ECIES_R_GEN_MAC_FAILED);
		goto err;
		}

	e = 0;
err:
	EVP_CIPHER_CTX_cleanup(&cipher_ctx);
	if (share) OPENSSL_free(share);
	if (ephem_key) EC_KEY_free(ephem_key);	
	if (e && cv) 
		{
		ECIES_CIPHERTEXT_VALUE_free(cv);
		cv = NULL;
		}
	return cv;
}

int ECIES_do_decrypt(const ECIES_CIPHERTEXT_VALUE *cv,
	const ECIES_PARAMS *param, unsigned char *out, size_t *outlen, 
	EC_KEY *pri_key)
{
	int r = 0;
	EC_POINT *ephem_point = NULL;
	unsigned char *share = NULL;
	unsigned char mac[EVP_MAX_MD_SIZE];
	unsigned char *enckey, *mackey;
	int sharelen, enckeylen, mackeylen, len;
	unsigned char *p;

	EVP_CIPHER_CTX ctx;
	EVP_CIPHER_CTX_init(&ctx);

	// check output buffer size
	if (!out)
		{
		*outlen = cv->ciphertext->length;
		r = 1;
		goto err;
		}
	if ((int)(*outlen) < cv->ciphertext->length)
		{
		*outlen = cv->ciphertext->length;
		ECIESerr(ECIES_F_ECIES_DO_DECRYPT, ECIES_R_BUFFER_TOO_SMALL);
		goto err;
		}


	/*
	 * decode ephem_point
	 */
	if (!cv->ephem_point || !cv->ephem_point->data)
		{
		ECIESerr(ECIES_F_ECIES_DO_DECRYPT, ECIES_R_BAD_DATA);
		goto err;
		}
	if (!(ephem_point = EC_POINT_new(EC_KEY_get0_group(pri_key))))
		{
		ECIESerr(ECIES_F_ECIES_DO_ENCRYPT, ERR_R_MALLOC_FAILURE);
		goto err;
		}
	if (!EC_POINT_oct2point(EC_KEY_get0_group(pri_key), ephem_point,
		cv->ephem_point->data, cv->ephem_point->length, NULL))
		{
		ECIESerr(ECIES_F_ECIES_DO_DECRYPT, ECIES_R_BAD_DATA);
		goto err;
		}
	
	/*
	 * use ecdh to compute enckey and mackey
	 */	
	if (param->sym_cipher)
		enckeylen = EVP_CIPHER_key_length(param->sym_cipher);
	else	enckeylen = cv->ciphertext->length;
	mackeylen = EVP_MD_size(param->mac_md);
	sharelen = enckeylen + mackeylen;

	if (!(share = OPENSSL_malloc(sharelen)))
		{
		ECIESerr(ECIES_F_ECIES_DO_DECRYPT, ERR_R_MALLOC_FAILURE);
		goto err;
		}
	
	if (!ECDH_compute_key(share, enckeylen + mackeylen,
		ephem_point, pri_key,
		KDF_get_x9_63(param->kdf_md))) 
		{
		ECIESerr(ECIES_F_ECIES_DO_DECRYPT, ECIES_R_ECDH_FAILED);
		goto err;
		}		
	enckey = share;
	mackey = share + enckeylen;
	
	/*
	 * generate and verify mac
	 */
	if (!cv->mactag || !cv->mactag->data)
		{
		ECIESerr(ECIES_F_ECIES_DO_DECRYPT, ECIES_R_BAD_DATA);
		goto err;
		}
	if (!HMAC(param->mac_md, mackey, mackeylen,
		cv->ciphertext->data, (size_t)cv->ciphertext->length,
		mac, (unsigned int *)&len)) 
		{
		ECIESerr(ECIES_F_ECIES_DO_DECRYPT, ECIES_R_GEN_MAC_FAILED);
		goto err;
		}
	if (len != cv->mactag->length)
		{
		ECIESerr(ECIES_F_ECIES_DO_DECRYPT, ECIES_R_VERIFY_MAC_FAILED);
		goto err;
		}
	if (memcmp(cv->mactag->data, mac, len))
		{
		ECIESerr(ECIES_F_ECIES_DO_DECRYPT, ECIES_R_VERIFY_MAC_FAILED);
		goto err;
		}

	/*
	 * decrypt ciphertext and output
	 */
	if (param->sym_cipher)
		{
		unsigned char iv[EVP_MAX_IV_LENGTH];
		memset(iv, 0, sizeof(iv));
		if (!EVP_DecryptInit(&ctx, param->sym_cipher, enckey, iv))
			{
			ECIESerr(ECIES_F_ECIES_DO_DECRYPT, ECIES_R_DECRYPT_FAILED);
			goto err;
			}
		p = out;
		if (!EVP_DecryptUpdate(&ctx, p, &len,
			cv->ciphertext->data, cv->ciphertext->length))
			{
			ECIESerr(ECIES_F_ECIES_DO_DECRYPT, ECIES_R_DECRYPT_FAILED);
			goto err;
			}
		p += len;
		if (!EVP_DecryptFinal(&ctx, p, &len))
			{
			ECIESerr(ECIES_F_ECIES_DO_DECRYPT, ECIES_R_DECRYPT_FAILED);
			goto err;
			}
		p += len;
		*outlen = (int)(p - out);
		}
		else 
		{
		int i;
		for (i = 0; i < cv->ciphertext->length; i++)
			out[i] = cv->ciphertext->data[i] ^ enckey[i];
		*outlen = cv->ciphertext->length;
		}
		
	r = 1;
err:
	if (share) OPENSSL_free(share);
	EVP_CIPHER_CTX_cleanup(&ctx);
	if (ephem_point) EC_POINT_free(ephem_point);

	return r;
}

