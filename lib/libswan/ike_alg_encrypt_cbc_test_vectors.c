/*
 * Copyright (C) 2014,2016 Andrew Cagney <cagney@gnu.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <https://www.gnu.org/licenses/gpl2.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include <stdio.h>
#include <stdlib.h>

#include "constants.h"
#include "lswalloc.h"

#include "ike_alg.h"
#include "test_buffer.h"
#include "ike_alg_test_cbc.h"
#include "crypt_cipher.h"

#include "fips_mode.h"
#include "pk11pub.h"
#include "crypt_symkey.h"
#include "crypt_mac.h"

#include "lswlog.h"

/*
 * Ref: https://tools.ietf.org/html/rfc3602: Test Vectors
 */
static const struct cbc_test_vector aes_cbc_test_vectors[] = {
	{
		.description = "Encrypting 16 bytes (1 block) using AES-CBC with 128-bit key",
		.key = "0x06a9214036b8a15b512e03d534120006",
		.iv = "0x3dafba429d9eb430b422da802c9fac41",
		.plaintext = "Single block msg",
		.ciphertext = "0xe353779c1079aeb82708942dbe77181a"
	},
	{
		.description = "Encrypting 32 bytes (2 blocks) using AES-CBC with 128-bit key",
		.key = "0xc286696d887c9aa0611bbb3e2025a45a",
		.iv = "0x562e17996d093d28ddb3ba695a2e6f58",
		.plaintext =
		"0x000102030405060708090a0b0c0d0e0f"
		"101112131415161718191a1b1c1d1e1f",
		.ciphertext =
		"0xd296cd94c2cccf8a3a863028b5e1dc0a"
		"7586602d253cfff91b8266bea6d61ab1"
	},
	{
		.description = "Encrypting 48 bytes (3 blocks) using AES-CBC with 128-bit key",
		.key = "0x6c3ea0477630ce21a2ce334aa746c2cd",
		.iv = "0xc782dc4c098c66cbd9cd27d825682c81",
		.plaintext = "This is a 48-byte message (exactly 3 AES blocks)",
		.ciphertext =
		"0xd0a02b3836451753d493665d33f0e886"
		"2dea54cdb293abc7506939276772f8d5"
		"021c19216bad525c8579695d83ba2684"
	},
	{
		.description = "Encrypting 64 bytes (4 blocks) using AES-CBC with 128-bit key",
		.key = "0x56e47a38c5598974bc46903dba290349",
		.iv = "0x8ce82eefbea0da3c44699ed7db51b7d9",
		.plaintext =
		"0xa0a1a2a3a4a5a6a7a8a9aaabacadaeaf"
		"b0b1b2b3b4b5b6b7b8b9babbbcbdbebf"
		"c0c1c2c3c4c5c6c7c8c9cacbcccdcecf"
		"d0d1d2d3d4d5d6d7d8d9dadbdcdddedf",
		.ciphertext =
		"0xc30e32ffedc0774e6aff6af0869f71aa"
		"0f3af07a9a31a9c684db207eb0ef8e4e"
		"35907aa632c3ffdf868bb7b29d3d46ad"
		"83ce9f9a102ee99d49a53e87f4c3da55"
	},
	{
		.description = NULL,
	}
};
const struct cbc_test_vector *const aes_cbc_tests = aes_cbc_test_vectors;

/*
 * https://tools.ietf.org/html/rfc4312
 * https://info.isl.ntt.co.jp/crypt/index.html
 * https://info.isl.ntt.co.jp/crypt/eng/camellia/dl/cryptrec/t_camellia.txt
 */
static const struct cbc_test_vector camellia_cbc_test_vectors[] = {
	{
		.description = "Camellia: 16 bytes with 128-bit key",
		.key = "0x" "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00",
		.iv = "0x" "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00",
		.plaintext = "0x" "80 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00",
		.ciphertext = "0x" "07 92 3A 39 EB 0A 81 7D 1C 4D 87 BD B8 2D 1F 1C"
	},
	{
		.description = "Camellia: 16 bytes with 128-bit key",
		.key = "0x" "00 11 22 33 44 55 66 77 88 99 AA BB CC DD EE FF",
		.iv = "0x" "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00",
		.plaintext = "0x" "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 01 ",
		.ciphertext = "0x" "14 4D 2B 0F 50 0C 27 B7 EC 2C D1 2D 91 59 6F 37"
	},
	{
		.description = "Camellia: 16 bytes with 256-bit key",
		.key = "0x" "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00",
		.iv = "0x" "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00",
		.plaintext = "0x" "80 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00",
		.ciphertext = "0x" "B0 C6 B8 8A EA 51 8A B0 9E 84 72 48 E9 1B 1B 9D"
	},
	{
		.description = "Camellia: 16 bytes with 256-bit key",
		.key = "0x" "00 11 22 33 44 55 66 77 88 99 AA BB CC DD EE FF FF EE DD CC BB AA 99 88 77 66 55 44 33 22 11 00",
		.iv = "0x" "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00",
		.plaintext = "0x" "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 01",
		.ciphertext = "0x" "CC 39 FF EE 18 56 D3 EB 61 02 5E 93 21 9B 65 23 "
	},
	{
		.description = NULL,
	}
};
const struct cbc_test_vector *const camellia_cbc_tests = camellia_cbc_test_vectors;

static bool test_cbc_op(const struct encrypt_desc *encrypt_desc,
			const char *description,
			enum cipher_op op,
			PK11SymKey *sym_key, const char *encoded_iv,
			const char *input_name, const char *input,
			const char *output_name, const char *output,
			struct logger *logger)
{
	const char *opstr = str_cipher_op(op);

	bool ok = true;
	struct crypt_mac iv = decode_to_mac("IV: ", encoded_iv, logger, HERE);

	/*
	 * If encrypting, the new iv is in the output, if decrypting,
	 * the new iv is the input.  The expected IV is found in the
	 * last few bytes.
	 */
	chunk_t expected_iv =
		decode_to_chunk("new IV: ", (op == ENCRYPT ? output :
					     op == DECRYPT ? input :
					     NULL),
				logger, HERE);
	chunk_t tmp = decode_to_chunk(input_name, input, logger, HERE);
	chunk_t expected = decode_to_chunk(output_name, output, logger, HERE);

	/* do_crypt modifies the data and IV in place. */
	cipher_ikev1(encrypt_desc, op, tmp, &iv, sym_key, logger);

	if (!verify_hunk(description, opstr, expected, tmp, logger, HERE)) {
		ok = false;
	}
	if (!verify_bytes(description, "updated CBC IV", iv.ptr, iv.len,
			  expected_iv.ptr + expected_iv.len - iv.len, iv.len,
			  logger, HERE)) {
		ok = false;
	}

	free_chunk_content(&expected_iv);
	free_chunk_content(&tmp);
	free_chunk_content(&expected);

	return ok;
}

/*
 * https://developer.mozilla.org/en-US/docs/Mozilla/Projects/NSS/nss_sample_code/NSS_Sample_Code_sample2
 * https://developer.mozilla.org/en-US/docs/Mozilla/Projects/NSS/NSS_Tech_Notes/nss_tech_note5
 */

static bool test_cbc_vector(const struct encrypt_desc *encrypt_desc,
			    const struct cbc_test_vector *test,
			    struct logger *logger)
{
	bool ok = true;

	PK11SymKey *sym_key = decode_to_key(encrypt_desc, test->key, logger, HERE);
	if (!test_cbc_op(encrypt_desc, test->description, 1,
			 sym_key, test->iv,
			 "plaintext: ", test->plaintext,
			 "ciphertext: ", test->ciphertext,
			 logger)) {
		ok = false;
	}
	if (!test_cbc_op(encrypt_desc, test->description, 0,
			 sym_key, test->iv,
			 "ciphertext: ", test->ciphertext,
			 "plaintext: ", test->plaintext,
			 logger)) {
		ok = false;
	}

	/* Clean up. */
	symkey_delref(logger, "sym_key", &sym_key);

	ldbg(logger, "%s() %s: %s", __func__, test->description, (ok ? "passed" : "failed"));
	return ok;
}

bool test_cbc_vectors(const struct encrypt_desc *desc,
		      const struct cbc_test_vector *tests,
		      struct logger *logger)
{
	bool ok = true;
	const struct cbc_test_vector *test;
	for (test = tests; test->description != NULL; test++) {
		llog(RC_LOG, logger, "  %s", test->description);
		if (!test_cbc_vector(desc, test, logger)) {
			ok = false;
		}
	}
	return ok;
}
