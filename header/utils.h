#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>

uint64_t current_time_ms();

/* Proof-of-Work helpers.
 * Compute SHA-256( node_id || nonce_str ) and check that the hex digest
 * starts with `difficulty` zero nibbles (hex chars).
 * Returns 1 if the digest satisfies the difficulty, 0 otherwise.
 */
int pow_check(const char *node_id, unsigned long nonce, int difficulty,
              char *digest_hex_out);   /* digest_hex_out must be >= 65 bytes */

/* Mine a valid nonce. Fills *nonce_out and digest_hex_out (>=65 bytes).
 * Returns the number of iterations tried. */
unsigned long pow_mine(const char *node_id, int difficulty,
                       unsigned long *nonce_out, char *digest_hex_out);

#endif
