// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple T6030 internal display enable gate, and its T8122 counterpart
 *
 * The T6030 device tree describes the internal display (the DCP, its
 * mailbox, the two display DARTs and the display subsystem) with every node
 * disabled. Kernels that cannot take the display over from iBoot therefore
 * keep scanning out through the simple framebuffer, and all of them can
 * share one device tree.
 *
 * This gate enables those five nodes before platform devices are created.
 * It acts only if the boot loader has set apple,t6030-handoff = <1> on the
 * DCP and on the display subsystem, which it does after it has checked and
 * locked the inherited DART mappings. apple_t6030_display.enable=0 on the
 * kernel command line keeps the display on the boot framebuffer instead;
 * apple_t6030_display.dcpext=0 and apple_t6030_display.scanout=0 do the same
 * for the external display processors and their display DARTs (see
 * gate_dcpext() below). The targets are found through the DCP's and the
 * display subsystem's phandles, and each must still be disabled. Otherwise
 * nothing is changed.
 *
 * The DCP firmware needs the power management processor (PMP) running,
 * which iBoot leaves loaded but halted. Once the display nodes are enabled,
 * the gate also adds the PMP from a built-in overlay: its node, DART and
 * mailbox (disabled), and a PMP report node whose driver sets the display
 * and storage requests and starts the PMP when the display driver asks for
 * it. The PMP node needs this Mac's board and DRAM vendor ids and its PMP
 * tunables. A boot loader that copies them from the ADT into
 * /chosen/asahi,t6030-pmp gets the T6030 overlay with those values, on any
 * T6030 board; without them, only a J516S gets the PMP, from an overlay with
 * one J516S's values. Before any driver probes, the gate then
 *  - raises the minimum power state of the display domains (display
 *    subsystem, front end and DCP CPU) to "active", so that the PMP can
 *    never power the running DCP down;
 *  - keeps the PMP and PMS_SRAM domains on, as iBoot left them: the PTD the
 *    DCP firmware and the PMP share lives in that SRAM;
 *  - gives the PMP DART and mailbox the interrupt parent of the DCP mailbox.
 * Everything the PMP needs is checked before the display nodes are enabled.
 * Without PMP values for this Mac, or if a check fails or the PMP cannot be
 * added, the display nodes stay (or are put back) disabled and the display
 * stays on the boot framebuffer.
 *
 * What differs between SoCs is in struct gate_soc. On T8122 (M3) the gate
 * serves the same handoff, marked apple,t8122-handoff by the boot loader on
 * apple,t8122-dcp and apple,t8122-display-subsystem nodes, and adds the T8122
 * PMP the same way. There is no T8122 fallback: the boot loader must pass
 * this Mac's PMP values in /chosen/asahi,t8122-pmp, or nothing is enabled. A
 * T8122 device tree without display nodes is normal and the gate says
 * nothing. apple_t8122_display.enable=0 has the same effect as the T6030
 * option.
 */

#define pr_fmt(fmt) fmt

#include <linux/errno.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kstrtox.h>
#include <linux/memblock.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_graph.h>
#include <linux/of_reserved_mem.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/sizes.h>
#include <linux/soc/apple/j613-display.h>
#include <linux/string.h>
#include <linux/types.h>

enum {
	GATE_DCP_DART,
	GATE_DISP0_DART,
	GATE_DCP_MBOX,
	GATE_DCP,
	GATE_DISPLAY,
	GATE_NR_NODES,
};

enum {
	PMP_PS_DISP_SYS,
	PMP_PS_DISP_FE,
	PMP_PS_DISP_CPU,
	PMP_PS_PMP,
	PMP_PS_PMS_SRAM,
	PMP_PS_NR,
};

static const char *const pmp_ps_labels[PMP_PS_NR] __initconst = {
	"disp_sys", "disp_fe", "disp_cpu", "pmp", "pms_sram",
};

#define PMGR_PS_ACTIVE	15

/*
 * Built-in overlays with the PMP, its DART and mailbox, and its report. The
 * T6030 one has no per-Mac values: the gate copies them from the node the
 * boot loader fills from this Mac's ADT. The J516S one carries the values
 * of one J516S, for a boot loader that passes none.
 */
extern const u8 __dtbo_t6030_pmp_begin[];
extern const u8 __dtbo_t6030_pmp_end[];
extern const u8 __dtbo_t6030_j516s_pmp_begin[];
extern const u8 __dtbo_t6030_j516s_pmp_end[];
extern const u8 __dtbo_t8122_pmp_begin[];
extern const u8 __dtbo_t8122_pmp_end[];

/* This Mac's PMP values, from the boot loader, or NULL. */
static struct device_node *gate_pmp_values __initdata;

/*
 * The PMP firmware image the T6030 PMP support was brought up with, as the
 * J516S overlay names it. The report driver starts only the image the PMP
 * node names; values from the boot loader must name this one too.
 */
static const char gate_pmp_uuid[] __initconst = "2F4EB4C4-001B-3ACF-A9A0-68D8E42FC3A7";

/* The T8122 PMP image of the 14.x system firmware. */
static const char gate_t8122_pmp_uuid[] __initconst = "3B18C886-4C70-349B-B1C4-70F35DE2C5CD";

/* One SoC's internal display handoff, as the gate checks and completes it */
struct gate_soc {
	const char *machine;		/* root compatible */
	const char *prefix;		/* log prefix */
	const char *name;
	const char *dcp_compat;
	const char *display_compat;
	const char *marker;		/* set to <1> by the boot loader */
	const char *pwrstate_compat;	/* the PMGR power states */
	const char *dcp_full_name;	/* the DCP node the PMP overlay expects, under /soc */
	const char *pmp_values;		/* the boot loader's PMP values for this Mac */
	const char *pmp_uuid;		/* the PMP image the PMP support is written for */
	const char *report_compat;	/* the PMP report the overlay adds */
	const u8 *pmp_dtbo, *pmp_dtbo_end;	/* NULL: no PMP description built in */
	const char *fallback_board;	/* gets the fallback overlay without boot loader values */
	const u8 *fallback_dtbo, *fallback_dtbo_end;
	bool quiet_without_nodes;	/* no display nodes in the device tree is normal */
	bool dcpext;			/* the external display processors and their DARTs */
};

static const struct gate_soc gate_t6030 __initconst = {
	.machine = "apple,t6030",
	.prefix = "apple-t6030-display: ",
	.name = "T6030",
	.dcp_compat = "apple,t6030-dcp",
	.display_compat = "apple,t6030-display-subsystem",
	.marker = "apple,t6030-handoff",
	.pwrstate_compat = "apple,t6030-pmgr-pwrstate",
	.dcp_full_name = "dcp@28ec00000",
	.pmp_values = "/chosen/asahi,t6030-pmp",
	.pmp_uuid = gate_pmp_uuid,
	.report_compat = "apple,t6030-pmp-v2-report",
	.pmp_dtbo = __dtbo_t6030_pmp_begin,
	.pmp_dtbo_end = __dtbo_t6030_pmp_end,
	.fallback_board = "apple,j516s",
	.fallback_dtbo = __dtbo_t6030_j516s_pmp_begin,
	.fallback_dtbo_end = __dtbo_t6030_j516s_pmp_end,
	.dcpext = true,
};

/*
 * T8122 (M3 MacBook Air): the same handoff and power states, and a PMP
 * overlay at the T8122 addresses. The PMP values must come from the boot
 * loader: there is no built-in fallback.
 */
static const struct gate_soc gate_t8122 __initconst = {
	.machine = "apple,t8122",
	.prefix = "apple-t8122-display: ",
	.name = "T8122",
	.dcp_compat = "apple,t8122-dcp",
	.display_compat = "apple,t8122-display-subsystem",
	.marker = "apple,t8122-handoff",
	.pwrstate_compat = "apple,t8122-pmgr-pwrstate",
	.dcp_full_name = "dcp@28ec00000",
	.pmp_values = "/chosen/asahi,t8122-pmp",
	.pmp_uuid = gate_t8122_pmp_uuid,
	.report_compat = "apple,t8122-pmp-v2-report",
	.pmp_dtbo = __dtbo_t8122_pmp_begin,
	.pmp_dtbo_end = __dtbo_t8122_pmp_end,
	.quiet_without_nodes = true,
};

static const struct gate_soc *const gate_socs[] __initconst = {
	&gate_t6030,
	&gate_t8122,
};

/* The SoC this boot runs on, if the gate serves it. */
static const struct gate_soc *gate_soc __initdata;

#define gate_info(fmt, ...) pr_info("%s" fmt, gate_soc->prefix, ##__VA_ARGS__)
#define gate_warn(fmt, ...) pr_warn("%s" fmt, gate_soc->prefix, ##__VA_ARGS__)
#define gate_err(fmt, ...) pr_err("%s" fmt, gate_soc->prefix, ##__VA_ARGS__)

/* The boot loader's values the gate copies onto the PMP node. */
static bool __init gate_pmp_value_wanted(const struct property *prop)
{
	if (!strcmp(prop->name, "apple,board-id") ||
	    !strcmp(prop->name, "apple,dram-vendor-id") ||
	    !strcmp(prop->name, "apple,dram-capacity"))
		return prop->length == sizeof(u32);
	return strstarts(prop->name, "apple,tunable-");
}

static bool gate_requested __initdata = true;

bool __init apple_t6030_display_gate_enabled(void)
{
	return gate_requested;
}

/* Kept after a successful apply: the live tree now holds its properties. */
static struct of_changeset gate_cs;
static struct of_changeset gate_pmp_cs;

static int __init gate_setup(char *arg)
{
	return kstrtobool(arg, &gate_requested);
}
early_param("apple_t6030_display.enable", gate_setup);

static int __init gate_setup_t8122(char *arg)
{
	return gate_setup(arg);
}
early_param("apple_t8122_display.enable", gate_setup_t8122);

/* Returns the only node compatible with @compat, or NULL if there are none or several. */
static struct device_node *__init gate_find_one(const char *compat)
{
	struct device_node *np, *found = NULL;

	for_each_compatible_node(np, NULL, compat) {
		if (found) {
			of_node_put(np);
			of_node_put(found);
			return NULL;
		}
		found = of_node_get(np);
	}

	return found;
}

static bool __init gate_marked(const struct device_node *np)
{
	u32 val;

	return !of_property_read_u32(np, gate_soc->marker, &val) && val == 1;
}

static bool __init gate_disabled(const struct device_node *np)
{
	const char *status;

	return !of_property_read_string(np, "status", &status) &&
	       !strcmp(status, "disabled");
}

/*
 * Returns the node named by the single entry of @list in @np, if that entry
 * has @nargs cells (the first equal to @arg0) and the node is compatible
 * with @compat. Returns NULL otherwise.
 */
static struct device_node *__init gate_target(const struct device_node *np,
					      const char *list,
					      const char *cells_name,
					      int nargs, u32 arg0,
					      const char *compat)
{
	struct of_phandle_args args;

	if (of_count_phandle_with_args(np, list, cells_name) != 1)
		return NULL;
	if (of_parse_phandle_with_args(np, list, cells_name, 0, &args))
		return NULL;

	if (args.args_count != nargs || (nargs && args.args[0] != arg0) ||
	    !of_device_is_compatible(args.np, compat)) {
		of_node_put(args.np);
		return NULL;
	}

	return args.np;
}

static int __init gate_resolve(struct device_node **np)
{
	int i;

	struct device_node *any;

	if (gate_soc->quiet_without_nodes) {
		any = of_find_compatible_node(NULL, NULL, gate_soc->dcp_compat);
		if (!any)
			return -ENODEV;
		of_node_put(any);
	}

	np[GATE_DCP] = gate_find_one(gate_soc->dcp_compat);
	if (!np[GATE_DCP]) {
		gate_warn("not enabling: need exactly one %s node\n", gate_soc->dcp_compat);
		return -ENODEV;
	}

	np[GATE_DISPLAY] = gate_find_one(gate_soc->display_compat);
	if (!np[GATE_DISPLAY]) {
		gate_warn("not enabling: need exactly one %s node\n", gate_soc->display_compat);
		return -ENODEV;
	}

	if (!gate_marked(np[GATE_DCP]) || !gate_marked(np[GATE_DISPLAY])) {
		gate_warn("not enabling: the boot loader did not set %s\n", gate_soc->marker);
		return -EPERM;
	}

	np[GATE_DCP_DART] = gate_target(np[GATE_DCP], "iommus", "#iommu-cells",
					1, 5, "apple,t8110-dart");
	if (!np[GATE_DCP_DART]) {
		gate_warn("not enabling: %pOF iommus is not one apple,t8110-dart stream 5\n",
			np[GATE_DCP]);
		return -EINVAL;
	}

	np[GATE_DISP0_DART] = gate_target(np[GATE_DISPLAY], "iommus",
					  "#iommu-cells", 1, 0,
					  "apple,t8110-dart");
	if (!np[GATE_DISP0_DART]) {
		gate_warn("not enabling: %pOF iommus is not one apple,t8110-dart stream 0\n",
			np[GATE_DISPLAY]);
		return -EINVAL;
	}

	np[GATE_DCP_MBOX] = gate_target(np[GATE_DCP], "mboxes", "#mbox-cells",
					0, 0, "apple,asc-mailbox-v4");
	if (!np[GATE_DCP_MBOX]) {
		gate_warn("not enabling: %pOF mboxes is not one apple,asc-mailbox-v4\n",
			np[GATE_DCP]);
		return -EINVAL;
	}

	if (np[GATE_DCP_DART] == np[GATE_DISP0_DART]) {
		gate_warn("not enabling: the DCP and the display subsystem share %pOF\n",
			np[GATE_DCP_DART]);
		return -EINVAL;
	}

	for (i = 0; i < GATE_NR_NODES; i++) {
		if (!gate_disabled(np[i])) {
			gate_warn("not enabling: %pOF is not disabled\n", np[i]);
			return -EBUSY;
		}
	}

	return 0;
}

static int __init gate_apply(struct device_node **np)
{
	int i, ret = 0;

	of_changeset_init(&gate_cs);

	for (i = 0; i < GATE_NR_NODES; i++) {
		ret = of_changeset_update_prop_string(&gate_cs, np[i], "status",
						      "okay");
		if (ret)
			break;
	}
	if (!ret)
		ret = of_changeset_apply(&gate_cs);

	if (ret) {
		of_changeset_destroy(&gate_cs);
		gate_err("not enabling: changeset failed: %d\n", ret);
		return ret;
	}

	gate_info("enabled %pOF, %pOF, %pOF, %pOF and %pOF\n",
		np[GATE_DCP_DART], np[GATE_DISP0_DART], np[GATE_DCP_MBOX],
		np[GATE_DCP], np[GATE_DISPLAY]);

	return 0;
}

/* Disables the display nodes again when the PMP could not be added. */
static void __init gate_revert(void)
{
	int ret = of_changeset_revert(&gate_cs);

	if (ret) {
		gate_err("could not disable the display nodes again: %d\n", ret);
		return;
	}
	of_changeset_destroy(&gate_cs);
	gate_info("display nodes disabled again, display stays on the boot framebuffer\n");
}

/* The single power domain of @np, if it is one of the SoC's power states. */
static struct device_node *__init gate_ps_parent(struct device_node *np)
{
	return gate_target(np, "power-domains", "#power-domain-cells", 0, 0,
			   gate_soc->pwrstate_compat);
}

static bool __init gate_ps_is(const struct device_node *np, const char *label)
{
	const char *name;

	return np && of_device_is_compatible(np, gate_soc->pwrstate_compat) &&
	       !of_property_read_string(np, "label", &name) && !strcmp(name, label);
}

/* The only power state below @pmgr labelled @label. */
static struct device_node *__init gate_ps_child(struct device_node *pmgr, const char *label)
{
	struct device_node *child, *found = NULL;

	for_each_child_of_node(pmgr, child) {
		if (!gate_ps_is(child, label))
			continue;
		if (found) {
			of_node_put(child);
			of_node_put(found);
			return NULL;
		}
		found = of_node_get(child);
	}

	return found;
}

/*
 * The display domains are the DCP's power domain and its parents; the PMP
 * and PMS_SRAM domains are found by label beside them.
 */
static int __init gate_pmp_resolve(struct device_node **np, struct device_node **ps,
				   struct device_node **aic)
{
	struct device_node *pmgr, *other;
	int i;

	if (!gate_soc->pmp_dtbo) {
		gate_warn("PMP not added: no %s PMP description is built in\n", gate_soc->name);
		return -ENODEV;
	}

	gate_pmp_values = of_find_node_by_path(gate_soc->pmp_values);
	if (gate_pmp_values) {
		const char *uuid = NULL;
		u32 v;

		if (of_property_read_u32(gate_pmp_values, "apple,board-id", &v) ||
		    of_property_read_u32(gate_pmp_values, "apple,dram-vendor-id", &v) ||
		    of_property_read_string(gate_pmp_values, "apple,tunable-uuid", &uuid)) {
			gate_warn("PMP not added: %pOF is incomplete\n", gate_pmp_values);
			of_node_put(gate_pmp_values);
			gate_pmp_values = NULL;
			return -EINVAL;
		}
		if (strcmp(uuid, gate_soc->pmp_uuid)) {
			gate_warn("PMP not added: this Mac's PMP firmware image is %s, not %s\n",
				uuid, gate_soc->pmp_uuid);
			of_node_put(gate_pmp_values);
			gate_pmp_values = NULL;
			return -EINVAL;
		}
	}
	if (!gate_pmp_values && (!gate_soc->fallback_board ||
				 !of_machine_is_compatible(gate_soc->fallback_board))) {
		gate_warn("PMP not added: the boot loader passed no PMP values for this Mac\n");
		return -ENODEV;
	}

	other = of_find_compatible_node(NULL, NULL, "apple,t6000-pmp-v2");
	if (!other)
		other = of_find_compatible_node(NULL, NULL, gate_soc->report_compat);
	if (other) {
		gate_warn("PMP not added: %pOF already exists\n", other);
		of_node_put(other);
		return -EEXIST;
	}

	/* The overlay's DCP fragment names this path. */
	if (strcmp(of_node_full_name(np[GATE_DCP]), gate_soc->dcp_full_name) ||
	    !of_node_name_eq(np[GATE_DCP]->parent, "soc") ||
	    !of_node_is_root(np[GATE_DCP]->parent->parent)) {
		gate_warn("PMP not added: the DCP is %pOF, not /soc/%s\n",
			np[GATE_DCP], gate_soc->dcp_full_name);
		return -EINVAL;
	}

	ps[PMP_PS_DISP_CPU] = gate_ps_parent(np[GATE_DCP]);
	if (ps[PMP_PS_DISP_CPU])
		ps[PMP_PS_DISP_FE] = gate_ps_parent(ps[PMP_PS_DISP_CPU]);
	if (ps[PMP_PS_DISP_FE])
		ps[PMP_PS_DISP_SYS] = gate_ps_parent(ps[PMP_PS_DISP_FE]);
	pmgr = of_get_parent(ps[PMP_PS_DISP_CPU]);
	if (pmgr) {
		ps[PMP_PS_PMP] = gate_ps_child(pmgr, "pmp");
		ps[PMP_PS_PMS_SRAM] = gate_ps_child(pmgr, "pms_sram");
		of_node_put(pmgr);
	}
	for (i = 0; i < PMP_PS_NR; i++) {
		if (!gate_ps_is(ps[i], pmp_ps_labels[i])) {
			gate_warn("PMP not added: no %s power state where expected\n",
				pmp_ps_labels[i]);
			return -ENODEV;
		}
	}
	for (i = PMP_PS_DISP_SYS; i <= PMP_PS_DISP_CPU; i++) {
		if (!of_property_read_bool(ps[i], "apple,inherited-on")) {
			gate_warn("PMP not added: %pOF is not apple,inherited-on\n", ps[i]);
			return -EINVAL;
		}
	}

	*aic = of_parse_phandle(np[GATE_DCP_MBOX], "interrupt-parent", 0);
	if (!*aic || !of_property_read_bool(*aic, "interrupt-controller")) {
		gate_warn("PMP not added: %pOF has no interrupt parent\n", np[GATE_DCP_MBOX]);
		return -EINVAL;
	}

	return 0;
}

/* Sets the u32 property @name of @np, adding it if it is missing. */
static int __init gate_set_u32(struct of_changeset *cs, struct device_node *np,
			       const char *name, u32 val)
{
	struct property *prop;
	__be32 *value;

	/* Kept for good: the live tree refers to it once applied. */
	prop = kzalloc_obj(*prop);
	value = kmalloc_obj(*value);
	if (!prop || !value) {
		kfree(prop);
		kfree(value);
		return -ENOMEM;
	}
	*value = cpu_to_be32(val);
	prop->name = (char *)name;
	prop->length = sizeof(*value);
	prop->value = value;

	return of_changeset_update_property(cs, np, prop);
}

/*
 * Adds the boot loader's PMP values (gate_pmp_value_wanted()) to @pmp, which
 * the T6030 overlay leaves without them. Returns the number added.
 */
static int __init gate_pmp_copy_values(struct of_changeset *cs, struct device_node *pmp)
{
	struct property *src, *prop;
	int n = 0, ret;

	for_each_property_of_node(gate_pmp_values, src) {
		if (!gate_pmp_value_wanted(src))
			continue;
		/* Kept for good: the live tree refers to it once applied. */
		prop = kzalloc_obj(*prop);
		if (!prop)
			return -ENOMEM;
		prop->name = kstrdup(src->name, GFP_KERNEL);
		prop->value = kmemdup(src->value, src->length, GFP_KERNEL);
		prop->length = src->length;
		if (!prop->name || (src->length && !prop->value)) {
			kfree(prop->name);
			kfree(prop->value);
			kfree(prop);
			return -ENOMEM;
		}
		ret = of_changeset_add_property(cs, pmp, prop);
		if (ret) {
			kfree(prop->name);
			kfree(prop->value);
			kfree(prop);
			return ret;
		}
		n++;
	}
	return n;
}

static int __init gate_t8122_gpu_pmp_link(struct of_changeset *cs, struct device_node *pmp)
{
	struct device_node *gpu, *linked;
	int ret = 0;

	if (gate_soc != &gate_t8122)
		return 0;
	gpu = of_find_compatible_node(NULL, NULL, "apple,agx-t8122");
	if (!gpu || !of_device_is_available(gpu))
		goto out;
	linked = of_parse_phandle(gpu, "apple,pmp", 0);
	if (!pmp || !pmp->phandle || (linked && linked != pmp))
		ret = -EINVAL;
	else if (!linked)
		ret = of_changeset_add_prop_u32(cs, gpu, "apple,pmp", pmp->phandle);
	of_node_put(linked);
out:
	of_node_put(gpu);
	return ret;
}

static int __init gate_pmp_apply(struct device_node *dcp, struct device_node **ps,
				 struct device_node *aic)
{
	const u8 *dtbo = gate_pmp_values ? gate_soc->pmp_dtbo : gate_soc->fallback_dtbo;
	const size_t size = gate_pmp_values ?
		gate_soc->pmp_dtbo_end - gate_soc->pmp_dtbo :
		gate_soc->fallback_dtbo_end - gate_soc->fallback_dtbo;
	struct device_node *report, *disp = NULL, *pmp = NULL, *dart = NULL, *mbox = NULL;
	int ovcs_id = 0, ret, i, values = 0;

	ret = of_overlay_fdt_apply(dtbo, size, &ovcs_id, NULL);
	if (ret) {
		gate_err("PMP not added: overlay failed: %d\n", ret);
		return ret;
	}

	report = of_find_compatible_node(NULL, NULL, gate_soc->report_compat);
	if (report) {
		struct device_node *child;

		pmp = of_parse_phandle(report, "apple,pmp", 0);
		/* The display request entry: DISP is PMP device 7. */
		for_each_child_of_node(report, child) {
			if (!strcmp(of_node_full_name(child), "report@7")) {
				disp = child;
				break;
			}
		}
	}
	if (pmp) {
		dart = of_parse_phandle(pmp, "iommus", 0);
		mbox = of_parse_phandle(pmp, "mboxes", 0);
	}

	of_changeset_init(&gate_pmp_cs);
	ret = dart && mbox && disp && disp->phandle ? 0 : -ENODEV;
	/* The DCP driver starts once the PMP acknowledges this request. */
	if (!ret)
		ret = of_changeset_add_prop_u32(&gate_pmp_cs, dcp, "apple,pmp-report",
						disp->phandle);
	/* The current14 T8122 GPU needs an inner AGX vote from this Mac's PMP. */
	if (!ret)
		ret = gate_t8122_gpu_pmp_link(&gate_pmp_cs, pmp);
	/* The DART and mailbox interrupt parent lives in the base tree. */
	if (!ret)
		ret = of_changeset_add_prop_u32(&gate_pmp_cs, dart, "interrupt-parent",
						aic->phandle);
	if (!ret)
		ret = of_changeset_add_prop_u32(&gate_pmp_cs, mbox, "interrupt-parent",
						aic->phandle);
	/* The display domains' floor, applied by the PMGR driver when it probes. */
	for (i = PMP_PS_DISP_SYS; i <= PMP_PS_DISP_CPU && !ret; i++)
		ret = gate_set_u32(&gate_pmp_cs, ps[i], "apple,min-state", PMGR_PS_ACTIVE);
	/* Linux would otherwise power these off as unused. */
	for (i = PMP_PS_PMP; i <= PMP_PS_PMS_SRAM && !ret; i++) {
		if (!of_property_read_bool(ps[i], "apple,always-on"))
			ret = of_changeset_add_prop_bool(&gate_pmp_cs, ps[i], "apple,always-on");
	}
	if (!ret && gate_pmp_values) {
		values = gate_pmp_copy_values(&gate_pmp_cs, pmp);
		ret = values < 0 ? values : 0;
	}
	if (!ret)
		ret = of_changeset_apply(&gate_pmp_cs);

	of_node_put(mbox);
	of_node_put(dart);
	of_node_put(pmp);
	of_node_put(disp);
	of_node_put(report);

	if (ret) {
		of_changeset_destroy(&gate_pmp_cs);
		of_overlay_remove(&ovcs_id);
		gate_err("PMP not added: changeset failed: %d\n", ret);
		return ret;
	}

	if (gate_pmp_values)
		gate_info("PMP: this Mac's %d values from %pOF\n", values, gate_pmp_values);
	else
		gate_info("PMP: the built-in J516S values; the boot loader passed none\n");
	gate_info("added the PMP and its display and storage report; minimum power state of %s, %s and %s raised to active; %s and %s kept on\n",
		pmp_ps_labels[PMP_PS_DISP_SYS], pmp_ps_labels[PMP_PS_DISP_FE],
		pmp_ps_labels[PMP_PS_DISP_CPU], pmp_ps_labels[PMP_PS_PMP],
		pmp_ps_labels[PMP_PS_PMS_SRAM]);
	return 0;
}

/*
 * External display processors (dcpext0, dcpext1, ...). The boot loader hands
 * each one over separately: its memory (apple,t6030-dcpext-memory-ready), and
 * its display DART with a scanout child that names it
 * (apple,t6030-dispext-handoff). Nothing here touches a processor's CPU: the
 * display driver starts the firmware when a display is first attached.
 *
 * Each processor ends in one of three states:
 *  - native (the default): the processor, its DART, mailbox, PMP request and
 *    Type-C routes are enabled; its display DART is added to the display
 *    subsystem's iommus, so every framebuffer is mapped for this pipe as on
 *    M1/M2, and a piodma child is added for the DART's PIODMA stream 4. The
 *    processor then joins the main DRM device.
 *  - manual (apple_t6030_display.dcpext_manual=1): the earlier diagnostic
 *    path, with explicit start and a separately mapped scanout buffer.
 *  - refused: everything stays disabled, and so do its Type-C routes, so the
 *    USB-C ports never wait for a display mode switch nobody registers.
 * apple_t6030_display.dcpext=0 refuses every processor;
 * apple_t6030_display.scanout=0 refuses the display DART, which the native
 * path needs.
 */
static bool gate_dcpext_requested __initdata = true;

static int __init gate_dcpext_setup(char *arg)
{
	return kstrtobool(arg, &gate_dcpext_requested);
}
early_param("apple_t6030_display.dcpext", gate_dcpext_setup);

static bool gate_scanout_requested __initdata = true;

static int __init gate_scanout_setup(char *arg)
{
	return kstrtobool(arg, &gate_scanout_requested);
}
early_param("apple_t6030_display.scanout", gate_scanout_setup);

static bool gate_dcpext_manual __initdata;

static int __init gate_dcpext_manual_setup(char *arg)
{
	return kstrtobool(arg, &gate_dcpext_manual);
}
early_param("apple_t6030_display.dcpext_manual", gate_dcpext_manual_setup);

#define GATE_DCPEXT_MAX		4

/* One external display processor and what the gate found for it. */
struct gate_ext {
	struct device_node *dcp, *dart, *mbox;
	struct device_node *cpu, *fe, *sys;
	struct device_node *entry;	/* its PMP request, pmp-dispextN */
	struct device_node *scanout;	/* the boot loader's scanout child */
	struct device_node *disp_dart;	/* the display DART that child names */
	unsigned int index;		/* N of dispextN */
	bool memory;			/* the memory handoff is present */
	bool scanout_ok;		/* the display DART handoff checked out */
};

static void __init gate_ext_put(struct gate_ext *ext)
{
	of_node_put(ext->disp_dart);
	of_node_put(ext->scanout);
	of_node_put(ext->entry);
	of_node_put(ext->sys);
	of_node_put(ext->fe);
	of_node_put(ext->cpu);
	of_node_put(ext->mbox);
	of_node_put(ext->dart);
	of_node_put(ext->dcp);
}

/*
 * True if @ps is the power state labelled "dispext<N>_<suffix>". The first
 * call sets *@index (UINT_MAX on entry); later calls must name the same N.
 */
static bool __init gate_ps_dispext(const struct device_node *ps, const char *suffix,
				   unsigned int *index)
{
	const char *label, *p;
	unsigned int n = 0;

	if (!ps || !of_device_is_compatible(ps, gate_soc->pwrstate_compat) ||
	    of_property_read_string(ps, "label", &label) || !strstarts(label, "dispext"))
		return false;
	p = label + strlen("dispext");
	if (*p < '0' || *p > '9')
		return false;
	while (*p >= '0' && *p <= '9' && n < GATE_DCPEXT_MAX)
		n = n * 10 + *p++ - '0';
	if (n >= GATE_DCPEXT_MAX || *p++ != '_' || strcmp(p, suffix))
		return false;
	if (*index != UINT_MAX && *index != n)
		return false;
	*index = n;
	return true;
}

/* The PMP request entry labelled pmp-dispext<index>. */
static struct device_node *__init gate_pmp_dispext_entry(unsigned int index)
{
	struct device_node *report, *child, *found = NULL;
	char want[16];
	const char *label;

	report = gate_find_one(gate_soc->report_compat);
	if (!report || !of_device_is_available(report)) {
		of_node_put(report);
		return NULL;
	}
	snprintf(want, sizeof(want), "pmp-dispext%u", index);
	for_each_child_of_node(report, child) {
		if (!of_property_read_string(child, "label", &label) && !strcmp(label, want)) {
			found = child;
			break;
		}
	}
	of_node_put(report);
	return found;
}

/*
 * A no-map DT reservation is recorded by the reserved-memory parser, but
 * need not appear in memblock.reserved: no-map marks memblock.memory, and
 * the bootloader's carveouts can be outside the usable-memory list entirely.
 * memblock_is_region_reserved() only checks intersection, not full coverage.
 */
static bool __init gate_scanout_table_reserved(struct device_node *region,
					     const struct resource *table)
{
	struct reserved_mem *rmem;
	struct memblock_region *m;
	u64 size;

	if (!region || !region->parent ||
	    !of_node_name_eq(region->parent, "reserved-memory") ||
	    !of_node_is_root(region->parent->parent) ||
	    !of_device_is_available(region) ||
	    !of_property_read_bool(region, "no-map") ||
	    of_property_read_bool(region, "reusable") || table->end < table->start)
		return false;
	size = resource_size(table);
	if (size < SZ_16K || size > SZ_1M || !IS_ALIGNED(table->start, SZ_16K) ||
	    !IS_ALIGNED(size, SZ_16K))
		return false;
	rmem = of_reserved_mem_lookup(region);
	if (!rmem || rmem->base != table->start || rmem->size != size)
		return false;
	/* Reject even partially allocatable or linearly mapped RAM. */
	for_each_mem_region(m) {
		if (m->size && m->base <= table->end &&
		    (table->start < m->base || table->start - m->base < m->size) &&
		    !memblock_is_nomap(m))
			return false;
	}
	return true;
}

/*
 * Read-only check of the display DART the boot loader handed over with
 * @ext's scanout child, before Linux may attach to it: locked, translating,
 * with empty stream 0 and stream 4 roots in reserved tables, and powered by
 * the processor's own CPU domain.
 */
static int __init gate_dispext_check(struct gate_ext *ext)
{
	struct device_node *dart = NULL, *region = NULL;
	struct resource regs, table, first_table = {};
	struct of_phandle_args spec;
	void __iomem *mmio = NULL;
	void *root;
	u32 state[6], marker;
	u64 phys;
	int i, ret = -EINVAL;

	ext->scanout = of_get_child_by_name(ext->dcp, "scanout");
	if (!ext->scanout || !gate_disabled(ext->scanout) ||
	    !of_device_is_compatible(ext->scanout, "apple,t6030-dispext-scanout") ||
	    of_property_read_u32(ext->scanout, "apple,t6030-dispext-handoff", &marker) ||
	    marker != 1)
		return -ENODEV;
	if (of_count_phandle_with_args(ext->scanout, "iommus", "#iommu-cells") != 1 ||
	    of_parse_phandle_with_args(ext->scanout, "iommus", "#iommu-cells", 0, &spec))
		return -EINVAL;
	dart = spec.np;
	if (spec.args_count != 1 || spec.args[0] != 0 || !gate_disabled(dart) ||
	    !dart->phandle || dart == ext->dart ||
	    !of_device_is_compatible(dart, "apple,t8110-dart") ||
	    of_property_read_u32(dart, "apple,t6030-dispext-handoff", &marker) || marker != 1 ||
	    of_property_count_u32_elems(dart, "apple,inherited-dart-state") != 6 ||
	    of_property_read_u32_array(dart, "apple,inherited-dart-state", state, 6) ||
	    of_count_phandle_with_args(dart, "memory-region", NULL) != 2 ||
	    of_property_match_string(dart, "memory-region-names", "sid0-page-tables") != 0 ||
	    of_property_match_string(dart, "memory-region-names", "sid4-page-tables") != 1 ||
	    state[0] != 0 || state[3] != 4 ||
	    of_address_to_resource(dart, 0, &regs) || resource_size(&regs) != SZ_16K)
		goto out;
	/* The DART belongs to this processor's pipe: it shares its CPU domain. */
	region = gate_ps_parent(dart);
	if (region != ext->cpu)
		goto out;
	of_node_put(region);
	region = NULL;
	mmio = ioremap(regs.start, resource_size(&regs));
	if (!mmio || !(readl(mmio + 0x200) & BIT(0)))
		goto out;
	for (i = 0; i < 2; i++) {
		u32 sid = state[3 * i], tcr = state[3 * i + 1], ttbr = state[3 * i + 2];

		/* A valid SID0 root must not turn a later SID4 refusal into success. */
		ret = -EINVAL;
		if ((tcr & (BIT(0) | BIT(1) | BIT(3))) != BIT(0) || !(ttbr & BIT(0)) ||
		    (ttbr & ~(GENMASK(29, 2) | BIT(0))) ||
		    readl(mmio + 0x1000 + 4 * sid) != tcr ||
		    readl(mmio + 0x1400 + 4 * sid) != ttbr)
			goto out;
		/* T8110: address bits29:2, 16KiB page shift (42-bit output PA). */
		phys = (u64)(ttbr & GENMASK(29, 2)) << 12;
		region = of_parse_phandle(dart, "memory-region", i);
		if (!region ||
		    of_address_to_resource(region, 0, &table) ||
		    !gate_scanout_table_reserved(region, &table) ||
		    phys < table.start || phys > table.end - SZ_16K + 1 ||
		    (i && table.start <= first_table.end && first_table.start <= table.end))
			goto out;
		if (!i)
			first_table = table;
		/* arch_initcall runs after vmalloc initialization. On arm64,
		 * MEMREMAP_WB uses ioremap_cache for no-map/outside-usable RAM;
		 * the validation above excludes an allocatable/direct-map alias.
		 */
		root = memremap(phys, SZ_16K, MEMREMAP_WB);
		if (!root)
			goto out;
		ret = memchr_inv(root, 0, SZ_16K) ? -EBUSY : 0;
		memunmap(root);
		if (ret)
			goto out;
		of_node_put(region);
		region = NULL;
	}
	ext->disp_dart = of_node_get(dart);
	ext->scanout_ok = true;
out:
	if (mmio)
		iounmap(mmio);
	of_node_put(region);
	of_node_put(dart);
	return ret;
}

/* Everything @ext's processor needs, checked without changing anything. */
static int __init gate_ext_resolve(struct gate_ext *ext)
{
	struct device_node *domain;
	u32 ready, id;
	int i;

	ext->index = UINT_MAX;
	ext->memory = !of_property_read_u32(ext->dcp, "apple,t6030-dcpext-memory-ready", &ready) &&
		      ready == 1 && of_property_present(ext->dcp, "memory-region");
	if (!ext->memory)
		return -ENODEV;
	ext->dart = gate_target(ext->dcp, "iommus", "#iommu-cells", 1, 5, "apple,t8110-dart");
	ext->mbox = gate_target(ext->dcp, "mboxes", "#mbox-cells", 0, 0, "apple,asc-mailbox-v4");
	if (!ext->dart || !ext->mbox || !gate_disabled(ext->dcp) ||
	    !gate_disabled(ext->dart) || !gate_disabled(ext->mbox))
		return -EINVAL;

	/* Do not enable a CPU whose power ownership is not fully described. */
	ext->cpu = gate_ps_parent(ext->dcp);
	if (!gate_ps_dispext(ext->cpu, "cpu", &ext->index))
		return -EINVAL;
	ext->fe = gate_ps_parent(ext->cpu);
	if (!gate_ps_dispext(ext->fe, "fe", &ext->index))
		return -EINVAL;
	ext->sys = gate_ps_parent(ext->fe);
	if (!gate_ps_dispext(ext->sys, "sys", &ext->index))
		return -EINVAL;
	for (i = 0; i < 2; i++) {
		domain = gate_ps_parent(i ? ext->mbox : ext->dart);
		of_node_put(domain);
		if (domain != ext->cpu)
			return -EINVAL;
	}

	/* The driver waits for this request's acknowledgement before RUN. */
	ext->entry = gate_pmp_dispext_entry(ext->index);
	if (!ext->entry || !gate_disabled(ext->entry) || !ext->entry->phandle ||
	    !of_device_is_compatible(ext->entry, "apple,t6000-pmp-v2-report-entry") ||
	    of_property_read_u32(ext->entry, "reg", &id) ||
	    !of_property_read_bool(ext->entry, "apple,always-on") ||
	    of_property_read_bool(ext->entry, "apple,no-ack"))
		return -ENOENT;
	return 0;
}

/* Kept for good once applied: the live tree refers to it. */
static struct of_changeset *__init gate_cs_alloc(void)
{
	struct of_changeset *cs = kzalloc_obj(*cs);

	if (cs)
		of_changeset_init(cs);
	return cs;
}

static void __init gate_cs_free(struct of_changeset *cs)
{
	of_changeset_destroy(cs);
	kfree(cs);
}

/* Sets "status" of every Type-C route of @dcp that does not already have it. */
static int __init gate_ext_routes(struct of_changeset *cs, struct device_node *dcp,
				  const char *status)
{
	struct device_node *routes, *route;
	const char *now;
	int ret = 0;

	routes = of_get_child_by_name(dcp, "typec-routes");
	for_each_child_of_node(routes, route) {
		if (!of_property_read_string(route, "status", &now) && !strcmp(now, status))
			continue;
		ret = of_changeset_update_prop_string(cs, route, "status", status);
		if (ret) {
			of_node_put(route);
			break;
		}
	}
	of_node_put(routes);
	return ret;
}

/* The display subsystem's iommus with <@dart 0> appended. */
static int __init gate_display_add_dart(struct of_changeset *cs, struct device_node *dart)
{
	struct device_node *display = gate_find_one(gate_soc->display_compat);
	struct of_phandle_args args;
	struct property *old, *prop;
	__be32 *value;
	int i, n, ret = -EINVAL;

	if (!display)
		return -ENODEV;
	old = of_find_property(display, "iommus", NULL);
	n = of_count_phandle_with_args(display, "iommus", "#iommu-cells");
	if (!old || n < 1 || old->length != n * 2 * sizeof(__be32))
		goto out;
	for (i = 0; i < n; i++) {
		if (of_parse_phandle_with_args(display, "iommus", "#iommu-cells", i, &args))
			goto out;
		of_node_put(args.np);
		if (args.np == dart || args.args_count != 1)
			goto out;
	}
	ret = -ENOMEM;
	prop = kzalloc_obj(*prop);
	value = kmalloc(old->length + 2 * sizeof(*value), GFP_KERNEL);
	if (!prop || !value) {
		kfree(prop);
		kfree(value);
		goto out;
	}
	memcpy(value, old->value, old->length);
	value[old->length / sizeof(*value)] = cpu_to_be32(dart->phandle);
	value[old->length / sizeof(*value) + 1] = cpu_to_be32(0);
	prop->name = "iommus";
	prop->length = old->length + 2 * sizeof(*value);
	prop->value = value;
	ret = of_changeset_update_property(cs, display, prop);
out:
	of_node_put(display);
	return ret;
}

/*
 * The processor's PIODMA stream on its display DART, as the internal DCP's
 * piodma child: the driver maps firmware buffers there on request.
 */
static int __init gate_ext_piodma(struct of_changeset *cs, struct gate_ext *ext)
{
	u32 iommus[2] = { ext->disp_dart->phandle, 4 };
	struct device_node *piodma;
	int ret;

	piodma = of_get_child_by_name(ext->dcp, "piodma");
	if (piodma) {
		ret = of_changeset_update_prop_string(cs, piodma, "status", "okay");
		if (!ret) {
			struct property *prop = kzalloc_obj(*prop);
			__be32 *value = kmalloc(sizeof(iommus), GFP_KERNEL);

			if (!prop || !value) {
				kfree(prop);
				kfree(value);
				ret = -ENOMEM;
			} else {
				value[0] = cpu_to_be32(iommus[0]);
				value[1] = cpu_to_be32(iommus[1]);
				prop->name = "iommus";
				prop->length = sizeof(iommus);
				prop->value = value;
				ret = of_changeset_update_property(cs, piodma, prop);
			}
		}
		if (!ret)
			ret = gate_set_u32(cs, piodma, "apple,t6030-dispext-handoff", 1);
		of_node_put(piodma);
		return ret;
	}
	piodma = of_changeset_create_node(cs, ext->dcp, "piodma");
	if (!piodma)
		return -ENOMEM;
	/* The new node is kept for good, like the changeset that attaches it. */
	ret = of_changeset_add_prop_u32_array(cs, piodma, "iommus", iommus, ARRAY_SIZE(iommus));
	if (!ret)
		ret = of_changeset_add_prop_u32(cs, piodma, "apple,t6030-dispext-handoff", 1);
	return ret;
}

/* Enables @ext's processor, in the native or the manual configuration. */
static int __init gate_ext_apply(struct gate_ext *ext, bool native)
{
	struct of_changeset *cs = gate_cs_alloc();
	int ret;

	if (!cs)
		return -ENOMEM;
	/* PMGR applies the floor at probe, before the PMP can manage this CPU. */
	ret = gate_set_u32(cs, ext->cpu, "apple,min-state", PMGR_PS_ACTIVE);
	/* The driver must wait for this request's acknowledgement before RUN. */
	if (!ret)
		ret = gate_set_u32(cs, ext->dcp, "apple,pmp-report", ext->entry->phandle);
	if (!ret)
		ret = of_changeset_update_prop_string(cs, ext->entry, "status", "okay");
	if (!ret)
		ret = of_changeset_update_prop_string(cs, ext->dart, "status", "okay");
	if (!ret)
		ret = of_changeset_update_prop_string(cs, ext->mbox, "status", "okay");
	if (!ret)
		ret = of_changeset_update_prop_string(cs, ext->dcp, "status", "okay");
	/* A processor's routes are usable exactly when the processor is. */
	if (!ret)
		ret = gate_ext_routes(cs, ext->dcp, "okay");
	if (!ret && ext->scanout_ok)
		ret = of_changeset_update_prop_string(cs, ext->disp_dart, "status", "okay");
	if (!ret && native) {
		ret = gate_ext_piodma(cs, ext);
		if (!ret)
			ret = gate_display_add_dart(cs, ext->disp_dart);
	} else if (!ret && ext->scanout_ok) {
		ret = gate_set_u32(cs, ext->scanout, "apple,t6030-scanout-verified", 1);
		if (!ret)
			ret = of_changeset_update_prop_string(cs, ext->scanout, "status", "okay");
	}
	if (!ret)
		ret = of_changeset_apply(cs);
	if (ret)
		gate_cs_free(cs);
	return ret;
}

/* Leaves @dcp disabled, and its routes with it. */
static void __init gate_ext_refuse(struct device_node *dcp, const char *why)
{
	struct of_changeset *cs;
	int ret = -ENOMEM;

	cs = gate_cs_alloc();
	if (cs) {
		ret = gate_ext_routes(cs, dcp, "disabled");
		if (!ret)
			ret = of_changeset_apply(cs);
		if (ret)
			gate_cs_free(cs);
	}
	gate_info("%pOF left disabled: %s%s\n", dcp, why,
		  ret ? "; its Type-C routes could not be disabled" : "");
}

/* The firmware the display driver runs external processors with. */
static bool __init gate_ext_firmware_ok(struct device_node *np)
{
	u32 ver[3];

	return of_property_count_u32_elems(np, "apple,firmware-compat") == 3 &&
	       !of_property_read_u32_array(np, "apple,firmware-compat", ver, 3) &&
	       ver[0] == 14 && ver[1] == 7 && ver[2] == 0;
}

static void __init gate_dcpext_one(struct device_node *np, const char *refuse)
{
	struct gate_ext ext = { .dcp = of_node_get(np) };
	bool native = !gate_dcpext_manual;
	int ret;

	if (!gate_disabled(np)) {
		gate_warn("%pOF is not disabled; external display gate skipped\n", np);
		goto put;
	}
	if (refuse) {
		gate_ext_refuse(np, refuse);
		goto put;
	}
	ret = gate_ext_resolve(&ext);
	if (ret == -ENODEV) {
		gate_ext_refuse(np, "the boot loader did not hand it over");
		goto put;
	}
	if (ret) {
		gate_ext_refuse(np, "power, memory or PMP prerequisites are not described");
		goto put;
	}
	/*
	 * The display driver refuses any other firmware at probe. Its Type-C
	 * routes would then never register, and every port that lists them
	 * would wait for them.
	 */
	if (!gate_ext_firmware_ok(np)) {
		gate_ext_refuse(np, "its firmware-compat is not 14.7.0");
		goto put;
	}
	if (gate_scanout_requested) {
		ret = gate_dispext_check(&ext);
		if (ret && ret != -ENODEV)
			gate_warn("%pOF: display DART handoff failed validation (%d)\n", np, ret);
	}
	if (native && !ext.scanout_ok) {
		gate_ext_refuse(np, "no usable display DART handoff");
		goto put;
	}
	ret = gate_ext_apply(&ext, native);
	if (ret) {
		gate_warn("%pOF: changeset failed: %d\n", np, ret);
		gate_ext_refuse(np, "the gate could not enable it");
		goto put;
	}
	if (native)
		gate_info("enabled dcpext%u (%pOF) with PMP DISPEXT%u request, CPU power floor, display DART %pOF and its Type-C routes; started by the display driver on first use\n",
			  ext.index, np, ext.index, ext.disp_dart);
	else
		gate_info("enabled dcpext%u (%pOF) for manual start%s\n", ext.index, np,
			  ext.scanout_ok ? " with the verified scanout DART" : "");
put:
	gate_ext_put(&ext);
}

/* Every external processor; @refuse, if set, says why none is enabled. */
static void __init gate_dcpext(const char *refuse)
{
	struct device_node *np;

	if (!refuse && !gate_dcpext_requested)
		refuse = "disabled on the command line";
	for_each_compatible_node(np, NULL, "apple,t6030-dcpext")
		gate_dcpext_one(np, refuse);
}

/*
 * The T8122 (M3) external display processor, dcpext. Unlike T6030's, the
 * boot loader does not hand it over: the display driver starts its firmware
 * itself, as on M1/M2 (DCP_FIRMWARE_V_14_7_TEMPLATE). The boot loader only
 * reserves that firmware (memory-region and iommu-addresses), sets its
 * firmware version and marks the node apple,t8122-dcpext-firmware-ready =
 * <1>. The node stays disabled in the device tree, so a kernel that cannot
 * drive it never sees it, and neither does a USB-C port controller waiting
 * for its Type-C display route.
 *
 * Once the internal display was handed over, this enables, in one
 * changeset:
 *  - the dcpext node and its Type-C routes (a processor's routes are usable
 *    exactly when the processor is);
 *  - <&dispextN_dart 0> in the display subsystem's iommus, so that every
 *    framebuffer is mapped for the external pipe too, as on M1/M2 (the
 *    display DART is the one the dcpext's piodma child names);
 *  - an active floor on the dcpext's CPU power state, as on T6030, so that
 *    the running PMP cannot power the processor down under its firmware;
 *  - if the PMP was added, its DISPEXT request (pmp-dispext0), as on
 *    T6030: once the PMP runs it owns the SoC power and performance states
 *    and keeps a block up only if that block's request bit was set when it
 *    started. The dcpext points at the entry (apple,pmp-report) and the
 *    display driver waits for its acknowledgement before it runs the dcpext
 *    CPU.
 * Its DARTs and mailbox must already be enabled by the device tree. Without
 * the marker, with other firmware, or if anything does not check out, the
 * node and its routes stay disabled. apple_t8122_display.dcpext=0 keeps them
 * disabled too.
 */
static bool gate_t8122_dcpext_requested __initdata = true;

static int __init gate_t8122_dcpext_setup(char *arg)
{
	return kstrtobool(arg, &gate_t8122_dcpext_requested);
}
early_param("apple_t8122_display.dcpext", gate_t8122_dcpext_setup);

static bool __init gate_available(struct device_node *np)
{
	return np && of_device_is_available(np);
}

/*
 * J613: also give the dcpext a Type-C route to the second USB-C port
 * (left-front, ATC1). The device tree only wires the left-back port
 * (route@0: ATC0's DP PHY and crossbar, crossbar state 2 = dispext1,0,
 * DPTX target 0) and leaves ATC1's crossbar disabled, so on the front port
 * the USB-C controller finds no display mode switch, and only USB comes
 * up. The gate adds, in the same changeset that enables the dcpext:
 *  - typec-routes/route@1 (mode-switch) with an endpoint, and port@3 with
 *    the matching endpoint on the front connector, as the device tree does
 *    for route@0 and typec0;
 *  - ATC1's DP PHY and crossbar to the dcpext's phys / mux-controls
 *    ("typec1"), the same crossbar state as route 0 (one display pipeline,
 *    dispext1,0, behind either port's crossbar), and DPTX target 1 (the
 *    firmware's DFP index of atc1, as in the M1/M2 dcpext routes);
 *  - status = "okay" on ATC1's crossbar, and apple,always-on on ATC1's
 *    common power state if ATC0's has it (as the device tree does for ATC0).
 * The dcpext has one DPTX link, so one external display at a time: the
 * display fabric gives the pipeline to the first port that asks; the other
 * port's display mode switch then reports busy, and its USB keeps working.
 * Everything is found through the device tree (the route@0 graph, the
 * connector whose SuperSpeed lanes go to the other ATC PHY, the crossbar at
 * that PHY's base + 0x4c000) and checked first; if anything does not match,
 * or the device tree already describes a second route, the front port is
 * left as it is and the dcpext is enabled with the routes it has.
 */

#define GATE_ATC_XBAR_OFFSET	0x4c000

struct gate_front {
	struct device_node *routes;	/* the dcpext's typec-routes */
	struct device_node *phy0, *phy1;	/* ATC0 / ATC1 PHY */
	struct device_node *xbar0, *xbar1;	/* their display crossbars */
	struct device_node *conn1;	/* the front connector */
	struct device_node *ports1;	/* its ports node */
	struct device_node *common1;	/* ATC1 common power state, if to be kept on */
	u32 phy_cell, mux_index, dptx;
	phandle route_ph, conn_ph, xbar_ph;
	bool xbar_new_ph;	/* nothing in the DTB refers to ATC1's crossbar */
	struct device_node *route_ep, *conn_ep;	/* created by the changeset */
};

static void __init gate_front_put(struct gate_front *f)
{
	of_node_put(f->routes);
	of_node_put(f->phy0);
	of_node_put(f->phy1);
	of_node_put(f->xbar0);
	of_node_put(f->xbar1);
	of_node_put(f->conn1);
	of_node_put(f->ports1);
	of_node_put(f->common1);
}

/* The remote port parent of @node's first endpoint in port @port_id. */
static struct device_node *__init gate_graph_remote(const struct device_node *node,
						    int port_id)
{
	struct device_node *ep, *remote;

	ep = of_graph_get_endpoint_by_regs(node, port_id, -1);
	if (!ep)
		return NULL;
	remote = of_graph_get_remote_port_parent(ep);
	of_node_put(ep);
	return remote;
}

static bool __init gate_reg_start(struct device_node *np, resource_size_t *start)
{
	struct resource res;

	if (of_address_to_resource(np, 0, &res))
		return false;
	*start = res.start;
	return true;
}

/* @xbar is the display crossbar of the ATC whose PHY is @phy. */
static bool __init gate_xbar_of_phy(struct device_node *xbar, struct device_node *phy)
{
	resource_size_t x, p;

	return of_device_is_compatible(xbar, "apple,t8122-display-crossbar") &&
	       gate_reg_start(xbar, &x) && gate_reg_start(phy, &p) &&
	       x == p + GATE_ATC_XBAR_OFFSET;
}

/* The "atcN_common" power state that @phy's power domain depends on. */
static struct device_node *__init gate_atc_common(struct device_node *phy)
{
	struct device_node *ps, *parent, *found = NULL;
	const char *label;
	int i;

	ps = of_parse_phandle(phy, "power-domains", 0);
	if (!ps)
		return NULL;
	for (i = 0; !found && (parent = of_parse_phandle(ps, "power-domains", i)); i++) {
		if (!of_property_read_string(parent, "label", &label) &&
		    strlen(label) > 7 && !strcmp(label + strlen(label) - 7, "_common"))
			found = parent;
		else
			of_node_put(parent);
	}
	of_node_put(ps);
	return found;
}

static bool __init gate_one_string(struct device_node *np, const char *list, const char *want)
{
	const char *s;

	return of_property_count_strings(np, list) == 1 &&
	       !of_property_read_string_index(np, list, 0, &s) && !strcmp(s, want);
}

/* The largest phandle in the live tree. */
static phandle __init gate_max_phandle(void)
{
	struct device_node *np;
	phandle max = 0;

	for (np = of_find_all_nodes(NULL); np; np = of_find_all_nodes(np))
		if (np->phandle != 0xdeadbeef && np->phandle > max)
			max = np->phandle;
	return max;
}

/* Checks everything the front-port route needs; returns why not, or NULL. */
static const char *__init gate_front_resolve(struct device_node *dcp, struct gate_front *f)
{
	struct device_node *child, *conn0 = NULL, *c, *remote, *common0, *port;
	struct of_phandle_args args;
	u32 val, reg, cells;
	int n = 0;

	if (!of_machine_is_compatible("apple,j613"))
		return "not a J613";
	f->routes = of_get_child_by_name(dcp, "typec-routes");
	if (!f->routes)
		return "the dcpext has no typec-routes";
	for_each_child_of_node(f->routes, child) {
		if (of_property_read_u32(child, "reg", &reg) || reg != 0 || n++) {
			of_node_put(child);
			of_node_put(conn0);
			return "the dcpext has a Type-C route other than route@0";
		}
		conn0 = gate_graph_remote(child, -1);
	}
	if (!conn0)
		return "route@0 has no Type-C connector";

	/* route 0: exactly the DTB's single-port wiring */
	if (of_count_phandle_with_args(dcp, "phys", "#phy-cells") != 1 ||
	    of_parse_phandle_with_args(dcp, "phys", "#phy-cells", 0, &args))
		goto wiring;
	f->phy0 = args.np;
	if (args.args_count != 1 || !of_device_is_compatible(f->phy0, "apple,t8122-atcphy"))
		goto wiring;
	f->phy_cell = args.args[0];
	if (of_count_phandle_with_args(dcp, "mux-controls", "#mux-control-cells") != 1 ||
	    of_parse_phandle_with_args(dcp, "mux-controls", "#mux-control-cells", 0, &args))
		goto wiring;
	f->xbar0 = args.np;
	if (args.args_count != 1 || args.args[0] != 0 || !gate_xbar_of_phy(f->xbar0, f->phy0))
		goto wiring;
	if (!gate_one_string(dcp, "phy-names", "typec0") ||
	    !gate_one_string(dcp, "mux-control-names", "typec0") ||
	    of_property_count_u32_elems(dcp, "apple,typec-mux-indices") != 1 ||
	    of_property_read_u32(dcp, "apple,typec-mux-indices", &f->mux_index) ||
	    of_property_count_u32_elems(dcp, "apple,typec-dptx-phys") != 1 ||
	    of_property_read_u32(dcp, "apple,typec-dptx-phys", &val) || val != 0)
		goto wiring;
	remote = gate_graph_remote(conn0, 1);
	of_node_put(remote);
	if (remote != f->phy0)
		goto wiring;
	of_node_put(conn0);
	conn0 = NULL;

	/* the one other connector whose SuperSpeed lanes go to another ATC PHY */
	for_each_compatible_node(c, NULL, "usb-c-connector") {
		if (!of_device_is_available(c->parent))
			continue;
		remote = gate_graph_remote(c, 1);
		if (remote && remote != f->phy0 &&
		    of_device_is_compatible(remote, "apple,t8122-atcphy")) {
			if (f->conn1) {
				of_node_put(remote);
				of_node_put(c);
				return "more than one other USB-C connector";
			}
			f->conn1 = of_node_get(c);
			f->phy1 = remote;
		} else {
			of_node_put(remote);
		}
	}
	if (!f->conn1)
		return "no other USB-C connector on an ATC PHY";
	port = of_graph_get_port_by_id(f->conn1, 3);
	of_node_put(port);
	if (port)
		return "the front connector already has a display port (port@3)";
	f->ports1 = of_get_child_by_name(f->conn1, "ports");
	if (!f->ports1 || of_property_read_u32(f->ports1, "#address-cells", &cells) ||
	    cells != 1 || of_property_read_u32(f->ports1, "#size-cells", &cells) || cells)
		return "the front connector has no usable ports node";
	if (!of_device_is_available(f->phy1))
		return "the front ATC PHY is disabled";
	if (of_property_count_u32_elems(f->phy1, "apple,tunable-common") <= 0)
		return "the front ATC PHY has no DisplayPort tunables";

	for_each_compatible_node(c, NULL, "apple,t8122-display-crossbar") {
		if (gate_xbar_of_phy(c, f->phy1)) {
			f->xbar1 = of_node_get(c);
			of_node_put(c);
			break;
		}
	}
	if (!f->xbar1)
		return "the front ATC has no display crossbar";
	if (f->xbar1 == f->xbar0)
		return "both ports share one crossbar";
	{
		struct device_node *a = of_parse_phandle(f->xbar1, "power-domains", 0);
		struct device_node *b = of_parse_phandle(f->phy1, "power-domains", 0);

		of_node_put(a);
		of_node_put(b);
		if (!a || a != b)
			return "the front crossbar is not in its ATC's power domain";
	}

	/* keep ATC1 common on exactly when ATC0 common is (DP alt mode) */
	common0 = gate_atc_common(f->phy0);
	if (common0 && of_property_read_bool(common0, "apple,always-on")) {
		f->common1 = gate_atc_common(f->phy1);
		if (f->common1 && (f->common1 == common0 ||
				   of_property_read_bool(f->common1, "apple,always-on"))) {
			of_node_put(f->common1);
			f->common1 = NULL;
		}
	}
	of_node_put(common0);

	if (!f->phy0->phandle || !f->phy1->phandle || !f->xbar0->phandle)
		return "an ATC PHY or crossbar has no phandle";

	f->dptx = 1;
	f->route_ph = gate_max_phandle() + 1;
	f->conn_ph = f->route_ph + 1;
	/* dtc gives a node a phandle only if something refers to it */
	f->xbar_new_ph = !f->xbar1->phandle;
	f->xbar_ph = f->xbar_new_ph ? f->route_ph + 2 : f->xbar1->phandle;
	if (f->route_ph < 2 || f->route_ph + 2 >= 0xdeadbeef)
		return "no free phandles";
	return NULL;

wiring:
	of_node_put(conn0);
	return "the dcpext's Type-C wiring is not the DTB's single left-back route";
}

/* Sets @np's property @name to a copy of @len bytes at @val. */
static int __init gate_set_raw(struct of_changeset *cs, struct device_node *np,
			       const char *name, const void *val, int len)
{
	struct property *prop = kzalloc_obj(*prop);
	void *value = kmemdup(val, len, GFP_KERNEL);

	if (!prop || !value) {
		kfree(prop);
		kfree(value);
		return -ENOMEM;
	}
	prop->name = (char *)name;
	prop->length = len;
	prop->value = value;
	/* adds the property if @np does not have it yet */
	return of_changeset_update_property(cs, np, prop);
}

static int __init gate_set_u32s(struct of_changeset *cs, struct device_node *np,
				const char *name, const u32 *vals, int n)
{
	__be32 be[4];
	int i;

	for (i = 0; i < n && i < ARRAY_SIZE(be); i++)
		be[i] = cpu_to_be32(vals[i]);
	return gate_set_raw(cs, np, name, be, i * sizeof(*be));
}

/* An endpoint node under @parent/@port_name, pointing at @remote_ph. */
static struct device_node *__init gate_front_endpoint(struct of_changeset *cs,
						      struct device_node *port,
						      phandle ph, phandle remote_ph)
{
	struct device_node *ep = of_changeset_create_node(cs, port, "endpoint");

	if (!ep)
		return NULL;
	if (of_changeset_add_prop_u32(cs, ep, "phandle", ph) ||
	    of_changeset_add_prop_u32(cs, ep, "remote-endpoint", remote_ph))
		return NULL;
	return ep;
}

/* Adds the front-port route (see above) to @cs. */
static int __init gate_front_add(struct of_changeset *cs, struct device_node *dcp,
				 struct gate_front *f)
{
	static const char names[] = "typec0\0typec1";
	struct device_node *route, *port;
	u32 v[4];
	int ret;

	v[0] = f->phy0->phandle;
	v[1] = f->phy_cell;
	v[2] = f->phy1->phandle;
	v[3] = f->phy_cell;
	ret = gate_set_u32s(cs, dcp, "phys", v, 4);
	if (!ret)
		ret = gate_set_raw(cs, dcp, "phy-names", names, sizeof(names));
	v[0] = f->xbar0->phandle;
	v[1] = 0;
	v[2] = f->xbar_ph;
	v[3] = 0;
	if (!ret)
		ret = gate_set_u32s(cs, dcp, "mux-controls", v, 4);
	if (!ret)
		ret = gate_set_raw(cs, dcp, "mux-control-names", names, sizeof(names));
	v[0] = f->mux_index;
	v[1] = f->mux_index;
	if (!ret)
		ret = gate_set_u32s(cs, dcp, "apple,typec-mux-indices", v, 2);
	v[0] = 0;
	v[1] = f->dptx;
	if (!ret)
		ret = gate_set_u32s(cs, dcp, "apple,typec-dptx-phys", v, 2);
	if (!ret && f->xbar_new_ph)
		ret = of_changeset_add_prop_u32(cs, f->xbar1, "phandle", f->xbar_ph);
	if (!ret && gate_disabled(f->xbar1))
		ret = of_changeset_update_prop_string(cs, f->xbar1, "status", "okay");
	if (!ret && f->common1)
		ret = of_changeset_add_prop_bool(cs, f->common1, "apple,always-on");
	if (ret)
		return ret;

	/* The new nodes are kept for good, like the changeset that attaches them. */
	route = of_changeset_create_node(cs, f->routes, "route@1");
	if (!route)
		return -ENOMEM;
	ret = of_changeset_add_prop_u32(cs, route, "reg", 1);
	if (!ret)
		ret = of_changeset_add_prop_bool(cs, route, "mode-switch");
	/* usable exactly when the dcpext is, which this changeset enables */
	if (!ret)
		ret = of_changeset_add_prop_string(cs, route, "status", "okay");
	if (ret)
		return ret;
	port = of_changeset_create_node(cs, route, "port");
	if (!port)
		return -ENOMEM;
	f->route_ep = gate_front_endpoint(cs, port, f->route_ph, f->conn_ph);
	if (!f->route_ep)
		return -ENOMEM;

	port = of_changeset_create_node(cs, f->ports1, "port@3");
	if (!port || of_changeset_add_prop_u32(cs, port, "reg", 3))
		return -ENOMEM;
	f->conn_ep = gate_front_endpoint(cs, port, f->conn_ph, f->route_ph);
	if (!f->conn_ep)
		return -ENOMEM;
	return 0;
}

/*
 * After the changeset is applied: a node takes its phandle from its
 * "phandle" property only when it is attached (from the FDT, or by a
 * changeset, which adds properties after the node), so the new endpoints
 * and the crossbar, which had none, do not have them yet. Set them now,
 * before anything can look them up (no devices exist yet).
 */
static void __init gate_front_applied(struct gate_front *f)
{
	f->route_ep->phandle = f->route_ph;
	f->conn_ep->phandle = f->conn_ph;
	if (f->xbar_new_ph)
		f->xbar1->phandle = f->xbar_ph;
}

static void __init gate_t8122_dcpext_one(struct device_node *np)
{
	struct device_node *dart = NULL, *mbox = NULL, *piodma = NULL;
	struct device_node *disp_dart = NULL, *ps = NULL, *entry = NULL;
	struct gate_front front = {};
	struct of_changeset *cs;
	const char *label, *pmp_note, *front_why;
	bool pmp_request = false, with_front = false;
	u32 ready;
	int ret;

	if (!gate_disabled(np)) {
		gate_warn("%pOF is not disabled; external display gate skipped\n", np);
		return;
	}
	if (of_property_read_u32(np, "apple,t8122-dcpext-firmware-ready", &ready) ||
	    ready != 1) {
		gate_ext_refuse(np, "the boot loader did not reserve its firmware");
		return;
	}
	if (!gate_ext_firmware_ok(np)) {
		gate_ext_refuse(np, "its firmware-compat is not 14.7.0");
		return;
	}
	if (!of_property_present(np, "memory-region")) {
		gate_ext_refuse(np, "it has no firmware memory");
		goto put;
	}
	dart = gate_target(np, "iommus", "#iommu-cells", 1, 5, "apple,t8110-dart");
	mbox = gate_target(np, "mboxes", "#mbox-cells", 0, 0, "apple,asc-mailbox-v4");
	piodma = of_get_child_by_name(np, "piodma");
	if (piodma)
		disp_dart = gate_target(piodma, "iommus", "#iommu-cells", 1, 4,
					"apple,t8110-dart");
	if (!gate_available(dart) || !gate_available(mbox) || !gate_available(disp_dart) ||
	    dart == disp_dart) {
		gate_ext_refuse(np, "its DARTs or mailbox are not described or not enabled");
		goto put;
	}
	ps = of_parse_phandle(np, "power-domains", 0);
	if (!ps || !of_device_is_compatible(ps, gate_soc->pwrstate_compat) ||
	    of_property_read_string(ps, "label", &label) || strcmp(label, "dispext_cpu")) {
		gate_ext_refuse(np, "its power state is not dispext_cpu");
		goto put;
	}

	/* The PMP DISPEXT request, if the PMP was added (see above). */
	entry = gate_pmp_dispext_entry(0);
	if (!entry) {
		pmp_note = "no PMP DISPEXT request (the PMP is not described)";
	} else if (!entry->phandle ||
		   !of_device_is_compatible(entry, "apple,t6000-pmp-v2-report-entry") ||
		   !gate_disabled(entry)) {
		pmp_note = "no PMP DISPEXT request (its report entry is not usable)";
	} else {
		pmp_request = true;
		pmp_note = "PMP DISPEXT request";
	}

	/* The front-port route, if everything it needs checks out. */
	front_why = gate_front_resolve(np, &front);
	if (front_why)
		gate_info("%pOF: no added route to the front USB-C port: %s\n", np, front_why);
	else
		with_front = true;

	cs = gate_cs_alloc();
	if (!cs) {
		gate_ext_refuse(np, "out of memory");
		goto put;
	}
	ret = gate_set_u32(cs, ps, "apple,min-state", PMGR_PS_ACTIVE);
	/* The driver waits for this request's acknowledgement before RUN. */
	if (!ret && pmp_request) {
		ret = gate_set_u32(cs, np, "apple,pmp-report", entry->phandle);
		if (!ret)
			ret = of_changeset_update_prop_string(cs, entry, "status", "okay");
	}
	if (!ret)
		ret = of_changeset_update_prop_string(cs, np, "status", "okay");
	if (!ret)
		ret = gate_ext_routes(cs, np, "okay");
	if (!ret && with_front)
		ret = gate_front_add(cs, np, &front);
	if (!ret)
		ret = gate_display_add_dart(cs, disp_dart);
	if (!ret)
		ret = of_changeset_apply(cs);
	if (ret) {
		gate_cs_free(cs);
		gate_warn("%pOF: changeset failed: %d\n", np, ret);
		gate_ext_refuse(np, "the gate could not enable it");
		goto put;
	}
	if (with_front)
		gate_front_applied(&front);
	gate_info("enabled %pOF with its Type-C routes, display DART %pOF, CPU power floor and %s; started by the display driver\n",
		  np, disp_dart, pmp_note);
	if (with_front)
		gate_info("%pOF: Type-C route 1 to the front port %pOF: DP PHY %pOF, crossbar %pOF (state %u), DPTX target %u%s\n",
			  np, front.conn1, front.phy1, front.xbar1, front.mux_index, front.dptx,
			  front.common1 ? ", ATC1 common power kept on" : "");
put:
	gate_front_put(&front);
	of_node_put(entry);
	of_node_put(ps);
	of_node_put(disp_dart);
	of_node_put(piodma);
	of_node_put(mbox);
	of_node_put(dart);
}

static void __init gate_t8122_dcpext(bool panel)
{
	struct device_node *np;

	for_each_compatible_node(np, NULL, "apple,t8122-dcpext") {
		if (!panel)
			gate_ext_refuse(np, "the internal display was not handed over");
		else if (!gate_t8122_dcpext_requested)
			gate_ext_refuse(np, "disabled on the command line");
		else
			gate_t8122_dcpext_one(np);
	}
}

/* Runs before of_platform_default_populate_init() at arch_initcall_sync. */
static int __init apple_t6030_display_gate(void)
{
	struct device_node *np[GATE_NR_NODES] = {};
	struct device_node *ps[PMP_PS_NR] = {};
	struct device_node *aic = NULL;
	bool panel = false;
	int i;

	for (i = 0; i < ARRAY_SIZE(gate_socs) && !gate_soc; i++)
		if (of_machine_is_compatible(gate_socs[i]->machine))
			gate_soc = gate_socs[i];
	if (!gate_soc)
		return 0;

	/* The exact 25G83 tree (J613; J615 experimental) has its own firmware and PMP admission gate. */
	if (of_machine_is_compatible("apple,j613") || of_machine_is_compatible("apple,j615")) {
		struct device_node *dcp = of_find_node_by_path("/soc/dcp@28ec00000");
		bool native = dcp && of_property_present(dcp, "apple,j613-25g83-profile");

		of_node_put(dcp);
		if (native)
			return 0;
	}

	if (!gate_requested) {
		gate_info("disabled on the command line, display stays on the boot framebuffer\n");
		if (gate_soc->dcpext)
			gate_dcpext("the display gate is disabled");
		return 0;
	}

	if (!gate_resolve(np) && !gate_pmp_resolve(np, ps, &aic) && !gate_apply(np)) {
		if (gate_pmp_apply(np[GATE_DCP], ps, aic))
			gate_revert();
		else
			panel = true;
	}
	/* External processors need the panel's PMP; without it they stay off. */
	if (gate_soc->dcpext)
		gate_dcpext(panel ? NULL : "the internal display was not handed over");
	if (gate_soc == &gate_t8122)
		gate_t8122_dcpext(panel);
	of_node_put(aic);
	for (i = 0; i < PMP_PS_NR; i++)
		of_node_put(ps[i]);
	for (i = 0; i < GATE_NR_NODES; i++)
		of_node_put(np[i]);

	return 0;
}
arch_initcall(apple_t6030_display_gate);
