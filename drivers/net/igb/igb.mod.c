#include <linux/module.h>
#include <linux/vermagic.h>
#include <linux/compiler.h>

MODULE_INFO(vermagic, VERMAGIC_STRING);

struct module __this_module
__attribute__((section(".gnu.linkonce.this_module"))) = {
 .name = KBUILD_MODNAME,
 .init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
 .exit = cleanup_module,
#endif
 .arch = MODULE_ARCH_INIT,
};

static const struct modversion_info ____versions[]
__used
__attribute__((section("__versions"))) = {
	{ 0x1f13d65d, "module_layout" },
	{ 0xbc5f2ab2, "alloc_pages_current" },
	{ 0x5a34a45c, "__kmalloc" },
	{ 0xf9a482f9, "msleep" },
	{ 0xc4dc87, "timecounter_init" },
	{ 0x20a7a79f, "pci_enable_sriov" },
	{ 0xd6ee688f, "vmalloc" },
	{ 0x91eb9b4, "round_jiffies" },
	{ 0x7e99ca67, "skb_pad" },
	{ 0x7743ac7b, "dev_set_drvdata" },
	{ 0xfa2e111f, "slab_buffer_size" },
	{ 0x950ffff2, "cpu_online_mask" },
	{ 0x79aa04a2, "get_random_bytes" },
	{ 0xba1bab9b, "dma_set_mask" },
	{ 0x57de844a, "napi_complete" },
	{ 0xd691cba2, "malloc_sizes" },
	{ 0x139be484, "pci_disable_device" },
	{ 0xc7a4fbed, "rtnl_lock" },
	{ 0x462e1dc5, "pci_disable_msix" },
	{ 0x56fd8fd7, "netif_carrier_on" },
	{ 0x2bd43d13, "dynamic_debug_enabled2" },
	{ 0x3b668f0d, "pci_disable_sriov" },
	{ 0xb813ce5a, "timecompare_transform" },
	{ 0x9425cdca, "ethtool_op_get_sg" },
	{ 0xa28e76e6, "schedule_work" },
	{ 0xc0a3d105, "find_next_bit" },
	{ 0x105e2727, "__tracepoint_kmalloc" },
	{ 0x8cc22231, "netif_carrier_off" },
	{ 0x8d8708f, "cancel_work_sync" },
	{ 0xe60f9ebb, "x86_dma_fallback_dev" },
	{ 0xfbc5ad18, "driver_for_each_device" },
	{ 0xeae3dfd6, "__const_udelay" },
	{ 0x6a9f26c9, "init_timer_key" },
	{ 0xfd7b28cb, "pci_enable_wake" },
	{ 0x999e8297, "vfree" },
	{ 0x668b5977, "pci_bus_write_config_word" },
	{ 0x2447533c, "ktime_get_real" },
	{ 0x3c2c5af5, "sprintf" },
	{ 0xa19002a8, "netif_napi_del" },
	{ 0x7d11c268, "jiffies" },
	{ 0xa5d72873, "__netdev_alloc_skb" },
	{ 0x27c33efe, "csum_ipv6_magic" },
	{ 0x9629486a, "per_cpu__cpu_number" },
	{ 0xfe7c4287, "nr_cpu_ids" },
	{ 0x9872d272, "pci_set_master" },
	{ 0xb4297c8f, "dca3_get_tag" },
	{ 0xe83fea1, "del_timer_sync" },
	{ 0xde0bdcff, "memset" },
	{ 0x25af4da7, "alloc_etherdev_mq" },
	{ 0xcd967389, "pci_enable_pcie_error_reporting" },
	{ 0x2e471f01, "dca_register_notify" },
	{ 0xf85ccdae, "kmem_cache_alloc_notrace" },
	{ 0x8225f53d, "pci_enable_msix" },
	{ 0x2e169dd1, "pci_restore_state" },
	{ 0x8006c614, "dca_unregister_notify" },
	{ 0xc16fe12d, "__memcpy" },
	{ 0xea147363, "printk" },
	{ 0xccd379ec, "pm_runtime_resume" },
	{ 0xf3848635, "free_netdev" },
	{ 0x7ec9bfbc, "strncpy" },
	{ 0x85f8a266, "copy_to_user" },
	{ 0x6e35d5b7, "register_netdev" },
	{ 0xb4390f9a, "mcount" },
	{ 0x672144bd, "strlcpy" },
	{ 0x16305289, "warn_slowpath_null" },
	{ 0x6dcaeb88, "per_cpu__kernel_stack" },
	{ 0xd917c158, "per_cpu__node_number" },
	{ 0x4666ffc4, "dev_close" },
	{ 0x45450063, "mod_timer" },
	{ 0xd2a4305a, "netif_set_real_num_tx_queues" },
	{ 0x1902adf, "netpoll_trap" },
	{ 0x8f69ff3a, "netif_napi_add" },
	{ 0x859c6dc7, "request_threaded_irq" },
	{ 0xabee3df5, "dca_add_requester" },
	{ 0xa2ffe665, "skb_pull" },
	{ 0x5122aa5c, "dev_kfree_skb_any" },
	{ 0x95b0a43e, "__pm_runtime_put" },
	{ 0x39a0f6d4, "dev_open" },
	{ 0xe523ad75, "synchronize_irq" },
	{ 0xead58fb9, "print_hex_dump" },
	{ 0x4b88ebbd, "pci_select_bars" },
	{ 0xc0bf6ead, "timecounter_cyc2time" },
	{ 0x9e7107f8, "netif_device_attach" },
	{ 0x45f352d9, "napi_gro_receive" },
	{ 0x40a9b349, "vzalloc" },
	{ 0x78764f4e, "pv_irq_ops" },
	{ 0x2bc9340, "netif_device_detach" },
	{ 0xa8f58290, "__alloc_skb" },
	{ 0x42c8de35, "ioremap_nocache" },
	{ 0xec6940a2, "pci_bus_read_config_word" },
	{ 0xc3ca3e20, "ethtool_op_set_sg" },
	{ 0xa2c5d44, "__napi_schedule" },
	{ 0x295a5546, "pci_cleanup_aer_uncorrect_error_status" },
	{ 0xf0fdf6cb, "__stack_chk_fail" },
	{ 0x108e8985, "param_get_uint" },
	{ 0x9cb480f4, "dynamic_debug_enabled" },
	{ 0x62132861, "kfree_skb" },
	{ 0x579292a3, "pm_schedule_suspend" },
	{ 0x36875389, "__timecompare_update" },
	{ 0x598fb825, "eth_type_trans" },
	{ 0x616040f2, "dev_driver_string" },
	{ 0xf0946c06, "pskb_expand_head" },
	{ 0x62b98286, "pci_unregister_driver" },
	{ 0xcc5005fe, "msleep_interruptible" },
	{ 0xa7ec84ac, "kmem_cache_alloc_node_notrace" },
	{ 0xc4061f09, "node_states" },
	{ 0x77cfcfde, "__tracepoint_kmalloc_node" },
	{ 0xe52947e7, "__phys_addr" },
	{ 0xf6ebc03b, "net_ratelimit" },
	{ 0xc8de4037, "pci_set_power_state" },
	{ 0x4cce2122, "eth_validate_addr" },
	{ 0x8d66a3a, "warn_slowpath_fmt" },
	{ 0x9f9494c8, "pci_disable_pcie_error_reporting" },
	{ 0x37a0cba, "kfree" },
	{ 0x6067a146, "memcpy" },
	{ 0xab510f2c, "pci_disable_msi" },
	{ 0x3285cc48, "param_set_uint" },
	{ 0x69d71162, "dma_supported" },
	{ 0xedc03953, "iounmap" },
	{ 0xbbb35483, "pci_prepare_to_sleep" },
	{ 0x70396578, "__pci_register_driver" },
	{ 0x2288378f, "system_state" },
	{ 0xfacba4d4, "put_page" },
	{ 0xb352177e, "find_first_bit" },
	{  0xfa03d, "pci_get_device" },
	{ 0x4cbbd171, "__bitmap_weight" },
	{ 0x26d73f78, "unregister_netdev" },
	{ 0x1675606f, "bad_dma_address" },
	{ 0x71b74376, "get_page" },
	{ 0x7cf2d8f8, "ethtool_op_get_tso" },
	{ 0x9e0c711d, "vzalloc_node" },
	{ 0x9edbecae, "snprintf" },
	{ 0x2035d916, "pci_enable_msi_block" },
	{ 0x4ecaeb00, "__netif_schedule" },
	{ 0xb19415ad, "__pm_runtime_get" },
	{ 0xdcebc08f, "consume_skb" },
	{ 0x5ef03b2d, "dca_remove_requester" },
	{ 0x82d3beb3, "pci_enable_device_mem" },
	{ 0x85670f1d, "rtnl_is_locked" },
	{ 0x37889d26, "vlan_gro_receive" },
	{ 0xec0a69c5, "skb_tstamp_tx" },
	{ 0xafac4743, "skb_put" },
	{ 0xd28a9d41, "pci_wake_from_d3" },
	{ 0xd085cd57, "pci_release_selected_regions" },
	{ 0xcac04032, "pci_request_selected_regions" },
	{ 0x92760d52, "skb_copy_bits" },
	{ 0x3302b500, "copy_from_user" },
	{ 0x53fd81e, "dev_get_drvdata" },
	{ 0x6e720ff2, "rtnl_unlock" },
	{ 0x9e7d6bd0, "__udelay" },
	{ 0xb712369b, "dma_ops" },
	{ 0xf20dabd8, "free_irq" },
	{ 0x564a70cc, "pci_save_state" },
};

static const char __module_depends[]
__used
__attribute__((section(".modinfo"))) =
"depends=dca";

MODULE_ALIAS("pci:v00008086d00001521sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d00001522sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d00001523sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d00001524sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d0000150Esv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d0000150Fsv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d00001527sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d00001510sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d00001511sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d00001516sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d00000438sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d0000043Asv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d0000043Csv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d00000440sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d000010C9sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d0000150Asv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d00001518sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d000010E6sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d000010E7sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d0000150Dsv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d00001526sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d000010E8sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d000010A7sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d000010A9sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d000010D6sv*sd*bc*sc*i*");

MODULE_INFO(srcversion, "534649E77A23DB868E7BE92");

static const struct rheldata _rheldata __used
__attribute__((section(".rheldata"))) = {
	.rhel_major = 6,
	.rhel_minor = 3,
};
