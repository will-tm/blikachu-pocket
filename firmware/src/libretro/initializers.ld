/* SPDX-License-Identifier: Apache-2.0 */
/* Keep core constructors out of Zephyr's boot-time constructor list: their
 * writable objects are valid only during an owned core session.
 */
. = ALIGN(4);
__lr_gambatte_init_start = .;
KEEP(*liblr_gambatte.a:*(.init_array .init_array.*))
__lr_gambatte_init_end = .;
