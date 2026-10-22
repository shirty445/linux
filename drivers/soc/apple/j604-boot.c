// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* J604 boot policy, independently implemented from the clean-room spec. */
#define pr_fmt(fmt) "gravity-j604: " fmt

#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/kstrtox.h>
#include <linux/limits.h>
#include <linux/of.h>
#include <linux/overflow.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/sizes.h>
#include <linux/string.h>

static bool input_enabled __initdata = true;
static bool panel_enabled __initdata;
static bool dptx_retained __initdata = true;
static bool gfx_retained __initdata = true;
static bool aop_enabled __initdata;
static bool jack_enabled __initdata;
static bool hdmi_enabled __initdata;
static bool poweroff_enabled __initdata;
static int hdmi_route __initdata = -1;

#define J604_BOOL_OPTION(name, variable) \
	static int __init variable##_parse(char *value) \
	{ \
		return value ? kstrtobool(value, &variable) : -EINVAL; \
	} \
	early_param(name, variable##_parse)

J604_BOOL_OPTION("gravity.j604_mtp", input_enabled);
J604_BOOL_OPTION("gravity.j604_dcp", panel_enabled);
J604_BOOL_OPTION("gravity.j604_dptx_on", dptx_retained);
J604_BOOL_OPTION("gravity.j604_gfx_on", gfx_retained);
J604_BOOL_OPTION("gravity.j604_aop", aop_enabled);
J604_BOOL_OPTION("gravity.j604_jack", jack_enabled);
J604_BOOL_OPTION("gravity.j604_hdmi", hdmi_enabled);
J604_BOOL_OPTION("gravity.j604_poweroff", poweroff_enabled);

static int __init j604_route_parse(char *value)
{
	return value ? kstrtoint(value, 0, &hdmi_route) : -EINVAL;
}
early_param("gravity.j604_hdmi_mux", j604_route_parse);

/* Keep one reference per reported node until the early policy is complete. */
static struct device_node *reported[64] __initdata;
static unsigned int reported_count __initdata;

static void __init j604_report(struct device_node *node)
{
	unsigned int i;

	for (i = 0; i < reported_count; i++)
		if (reported[i] == node)
			return;
	if (reported_count < ARRAY_SIZE(reported))
		reported[reported_count++] = of_node_get(node);
	pr_info("configured %pOF (%s)\n", node,
		of_device_is_available(node) ? "enabled" : "disabled");
}

static struct property *__init j604_property(const char *name,
					   const void *value, int length)
{
	struct property *prop = kzalloc(sizeof(*prop), GFP_KERNEL);

	if (!prop)
		return NULL;
	prop->name = kstrdup(name, GFP_KERNEL);
	prop->value = length ? kmemdup(value, length, GFP_KERNEL) : NULL;
	prop->length = length;
	if (!prop->name || (length && !prop->value)) {
		kfree(prop->value);
		kfree(prop->name);
		kfree(prop);
		return NULL;
	}
	of_property_set_flag(prop, OF_DYNAMIC);
	return prop;
}

static int __init j604_update(struct device_node *node, const char *name,
			     const void *value, int length)
{
	struct property *old, *prop;
	int ret;

	if (!node)
		return -ENOENT;
	old = of_find_property(node, name, NULL);
	if (old && old->length == length &&
	    (!length || !memcmp(old->value, value, length)))
		return 0;
	prop = j604_property(name, value, length);
	if (!prop) {
		ret = -ENOMEM;
		goto warn;
	}
	ret = of_update_property(node, prop);
	if (!ret)
		return 0; /* The live OF tree now owns the property storage. */
	kfree(prop->value);
	kfree(prop->name);
	kfree(prop);
warn:
	pr_warn("cannot update %pOF:%s: %d\n", node, name, ret);
	return ret;
}

static void __init j604_status(struct device_node *node, bool enable)
{
	const char *status = enable ? "okay" : "disabled";

	if (node && !j604_update(node, "status", status, strlen(status) + 1))
		j604_report(node);
}

static void __init j604_suppliers(struct device_node *node, const char *name,
				 const char *cells, const char *preserve)
{
	struct of_phandle_args args;
	int i;

	if (!node)
		return;
	for (i = 0; !of_parse_phandle_with_args(node, name, cells, i, &args); i++) {
		/* Never enable a live DART if its retention flag could not be set. */
		if (!preserve || !j604_update(args.np, preserve, NULL, 0))
			j604_status(args.np, true);
		of_node_put(args.np);
	}
}

static void __init j604_coproc(struct device_node *node, const char *preserve)
{
	j604_suppliers(node, "iommus", "#iommu-cells", preserve);
	j604_suppliers(node, "mboxes", "#mbox-cells", NULL);
}

static void __init j604_retain_domain(const char *label)
{
	struct device_node *node;
	const char *name;

	for_each_compatible_node(node, NULL, "apple,t8132-pmgr-pwrstate") {
		if (of_property_read_string(node, "label", &name) || strcmp(name, label))
			continue;
		if (!j604_update(node, "apple,always-on", NULL, 0))
			j604_report(node);
	}
}

static void __init j604_input(void)
{
	struct device_node *node;

	node = of_find_compatible_node(NULL, NULL, "apple,t8132-mtp");
	if (input_enabled)
		j604_coproc(node, "gravity,keep-boot-bypass");
	j604_status(node, input_enabled);
	of_node_put(node);
	node = of_find_compatible_node(NULL, NULL, "apple,t8132-dockchannel");
	j604_status(node, input_enabled);
	of_node_put(node);
}

static void __init j604_external_display(void)
{
	struct device_node *node, *child;
	__be32 route = cpu_to_be32(hdmi_route);

	node = of_find_node_by_path("dcpext1");
	j604_status(node, node && of_property_present(node, "apple,j604-static-typec"));
	of_node_put(node);

	node = of_find_node_by_path("dcpext0");
	if (node && hdmi_enabled) {
		j604_coproc(node, NULL);
		child = of_get_child_by_name(node, "piodma");
		j604_suppliers(child, "iommus", "#iommu-cells", NULL);
		of_node_put(child);
		j604_suppliers(node, "mux-controls", "#mux-control-cells", NULL);
		j604_suppliers(node, "phys", "#phy-cells", NULL);
		if (hdmi_route >= 0)
			j604_update(node, "mux-index", &route, sizeof(route));
	}
	j604_status(node, hdmi_enabled);
	of_node_put(node);
}

extern const u8 __dtbo_j604_poweroff_begin[];
extern const u8 __dtbo_j604_poweroff_end[];

static void __init j604_poweroff(void)
{
	int id, ret;

	ret = of_overlay_fdt_apply(__dtbo_j604_poweroff_begin,
		__dtbo_j604_poweroff_end - __dtbo_j604_poweroff_begin, &id, NULL);
	if (ret)
		pr_warn("poweroff overlay failed: %d\n", ret);
	else
		pr_info("applied poweroff overlay\n");
}

static int __init j604_boot_policy(void)
{
	struct device_node *node, *child;
	unsigned int i;

	if (!of_machine_is_compatible("apple,j604"))
		return 0;
	if (dptx_retained)
		j604_retain_domain("dptx_phy");
	if (gfx_retained)
		j604_retain_domain("gfx");
	j604_input();
	j604_external_display();
	if (poweroff_enabled)
		j604_poweroff();
	if (aop_enabled) {
		node = of_find_compatible_node(NULL, NULL, "apple,t8132-aop");
		j604_coproc(node, NULL);
		child = of_get_child_by_name(node, "audio");
		j604_suppliers(child, "dmas", "#dma-cells", NULL);
		of_node_put(child);
		j604_status(node, true);
		of_node_put(node);
	}
	if (jack_enabled) {
		node = of_find_compatible_node(NULL, NULL, "cirrus,cs42l84");
		if (node) {
			j604_status(node->parent, true);
			j604_status(node, true);
		}
		of_node_put(node);
	}
	if (panel_enabled) {
		node = of_find_node_by_path("dcp");
		j604_coproc(node, "gravity,keep-boot-translation");
		of_node_put(node);
	}
	for (i = 0; i < reported_count; i++)
		of_node_put(reported[i]);
	return 0;
}
/* The default OF platform population runs at arch_initcall_sync. */
arch_initcall(j604_boot_policy);

static int __init j604_bootloader_log(void)
{
	struct device_node *chosen;
	u64 range[2], end;
	void *mapping;
	char *copy, *cursor, *line;
	int ret;

	if (!of_machine_is_compatible("apple,j604"))
		return 0;
	chosen = of_find_node_by_path("/chosen");
	if (!chosen)
		return 0;
	ret = of_property_read_u64_array(chosen, "gravity,u-boot-log", range, 2);
	of_node_put(chosen);
	if (ret)
		return 0;
	if (!range[1] || range[1] > SZ_1M ||
	    check_add_overflow(range[0], range[1], &end)) {
		pr_warn("invalid bootloader log range\n");
		return 0;
	}
	mapping = memremap(range[0], range[1], MEMREMAP_WB);
	if (!mapping)
		return 0;
	copy = kmemdup_nul(mapping, strnlen(mapping, range[1]), GFP_KERNEL);
	memunmap(mapping);
	if (!copy)
		return 0;
	cursor = copy;
	while ((line = strsep(&cursor, "\n")))
		if (*line)
			printk(KERN_INFO "u-boot: %s\n", line);
	kfree(copy);
	return 0;
}
late_initcall(j604_bootloader_log);
