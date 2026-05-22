#include <linux/module.h>
#include <linux/export-internal.h>
#include <linux/compiler.h>

MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};



static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0xc256de77, "sync_dirty_buffer" },
	{ 0x7fd6bafd, "__brelse" },
	{ 0x7c2db8bc, "kmalloc_caches" },
	{ 0x2b6b3dec, "__kmalloc_cache_noprof" },
	{ 0x5a921311, "strncmp" },
	{ 0xad877825, "d_splice_alias" },
	{ 0x5c3c7387, "kstrtoull" },
	{ 0xe0831b8e, "new_inode" },
	{ 0xb68001ac, "simple_inode_init_ts" },
	{ 0x1b2fa1a9, "vmalloc_noprof" },
	{ 0x4dfa8d4b, "mutex_lock" },
	{ 0x999e8297, "vfree" },
	{ 0x3213f038, "mutex_unlock" },
	{ 0x69acdf38, "memcpy" },
	{ 0x88db9f48, "__check_object_size" },
	{ 0x6b10bee1, "_copy_to_user" },
	{ 0x75ca79b5, "__fortify_panic" },
	{ 0x656e4a6e, "snprintf" },
	{ 0x65487097, "__x86_indirect_thunk_rax" },
	{ 0x1059ed9e, "d_parent_ino" },
	{ 0x670ecece, "__x86_indirect_thunk_rbx" },
	{ 0x69dd3b5b, "crc32_le" },
	{ 0x13c49cc2, "_copy_from_user" },
	{ 0xcefb0c9f, "__mutex_init" },
	{ 0x9a33835f, "sb_set_blocksize" },
	{ 0x411d97b, "set_nlink" },
	{ 0x60381eca, "d_make_root" },
	{ 0x7cc063c, "kill_block_super" },
	{ 0x5af3de97, "generic_delete_inode" },
	{ 0xd6119b30, "simple_statfs" },
	{ 0xeef71b04, "generic_file_llseek" },
	{ 0xbf972301, "generic_read_dir" },
	{ 0x13e10d7, "default_llseek" },
	{ 0x7c5c6b6f, "param_ops_uint" },
	{ 0x3aa71661, "param_ops_charp" },
	{ 0xbdfb6dbb, "__fentry__" },
	{ 0xe2d5255a, "strcmp" },
	{ 0x9a5a10a6, "mount_bdev" },
	{ 0x92997ed8, "_printk" },
	{ 0x37a0cba, "kfree" },
	{ 0x4ff0af24, "kmem_cache_free" },
	{ 0x8f5ce24e, "kmem_cache_alloc_noprof" },
	{ 0xab1446cc, "inode_init_once" },
	{ 0x5b8239ca, "__x86_return_thunk" },
	{ 0x5761955a, "unregister_filesystem" },
	{ 0x60a13e90, "rcu_barrier" },
	{ 0x529ad0e9, "kmem_cache_destroy" },
	{ 0x225c3541, "__kmem_cache_create_args" },
	{ 0xd609b895, "register_filesystem" },
	{ 0xf0fdf6cb, "__stack_chk_fail" },
	{ 0xd13d068a, "__bread_gfp" },
	{ 0xc3423e5b, "mark_buffer_dirty" },
	{ 0xe2e2ca37, "module_layout" },
};

MODULE_INFO(depends, "");


MODULE_INFO(srcversion, "46168ABFBC615CEAAC57B43");
