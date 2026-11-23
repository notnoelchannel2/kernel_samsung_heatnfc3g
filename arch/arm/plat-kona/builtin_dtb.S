/*
 * Board device tree linked into the kernel image (CONFIG_KONA_BUILTIN_DTB).
 *
 * Kept in .rodata (not an init section): unflatten_device_tree() keeps
 * pointers into the flat blob for node names and property values.
 */
#include <linux/linkage.h>

	.section .rodata, "a"
	.balign	8
	.global	__kona_builtin_dtb_begin
__kona_builtin_dtb_begin:
	.incbin	KONA_DTB_PATH
	.global	__kona_builtin_dtb_end
__kona_builtin_dtb_end:
	.balign	8
