// SPDX-License-Identifier: GPL-2.0-only
/*
 * gpu_rebar_layout - give selected GPUs their largest Resizable BAR from Linux when the BIOS
 * cannot (for example an AM4 board with a PCIe switch and several 32 GB GPUs).
 *
 * At load time, for every PCI device whose vendor:device is listed in `ids`:
 *   1. check that nothing below the root ports above those devices is in use: no driver bound
 *      (other than pcieport), no boot display, and no endpoints other than GPUs, their audio
 *      functions and the switch's own management endpoints (unless allow_other_devices=1),
 *   2. set BAR0's Resizable BAR size to the largest supported size (capped by max_log2_mb),
 *   3. remove those root ports and rescan. Before resources are assigned, with drop_gpu_audio=1,
 *      remove the GPUs' HDMI/DP audio functions in that subtree so they take no address space
 *      (they come back only if something triggers a PCI rescan),
 *   4. unless pad=0, pad every bus segment in the subtree: each smaller memory BAR is aligned to
 *      the largest BAR of the same kind (64-bit prefetchable / other) on its segment. Linux sizes
 *      bridge windows by summing child sizes, which undercounts when a large BAR is followed by
 *      small ones (the next card needs a boundary aligned to the large BAR). With the padding
 *      every window is a multiple of its alignment, so the kernel's normal allocator can place
 *      them,
 *   5. check that every device came back with its memory BARs assigned and every resized BAR0
 *      has the size asked for, and log what didn't.
 *
 * Must load before any GPU driver binds (from the initramfs). Does nothing unless `ids` is set,
 * e.g. on the kernel command line: gpu_rebar_layout.ids=1002:7551
 *
 * The changes are made once, at load time. Unloading the module does not undo them.
 *
 * Documentation: README.md
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/hex.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/ioport.h>
#include <linux/vgaarb.h>

#define MAX_TARGET_IDS 8
#define MAX_TARGETS 16
#define MAX_DROPPED_AUDIO 32

static char *ids = "";
module_param(ids, charp, 0444);
MODULE_PARM_DESC(ids, "Comma-separated vendor:device list to enlarge, e.g. 1002:7551 (at most 8)");

static unsigned int max_log2_mb = 15;
module_param(max_log2_mb, uint, 0444);
MODULE_PARM_DESC(max_log2_mb, "Largest BAR0 size as log2(MB); 15 = 32 GB (default), values above 27 are capped");

static bool drop_gpu_audio;
module_param(drop_gpu_audio, bool, 0444);
MODULE_PARM_DESC(drop_gpu_audio, "Remove the HDMI/DP audio functions of GPUs below the re-enumerated root ports (default: false)");

static bool pad = true;
module_param(pad, bool, 0444);
MODULE_PARM_DESC(pad, "Pad smaller BARs to the largest BAR on their bus segment (default: true; not needed on kernels with the upstream bridge window sizing fix)");

static bool allow_other_devices;
module_param(allow_other_devices, bool, 0444);
MODULE_PARM_DESC(allow_other_devices, "Re-enumerate even when other endpoints (disks, NICs, ...) share a root port with the GPUs (default: false)");

static u16 target_vendors[MAX_TARGET_IDS], target_devices[MAX_TARGET_IDS];
static int target_id_count;

/* Parse one to four hex digits. */
static int parse_hex16(const char **cursor, u16 *value)
{
	unsigned int result = 0;
	int digits = 0, digit;

	while ((digit = hex_to_bin(**cursor)) >= 0) {
		if (++digits > 4)
			return -EINVAL;
		result = result * 16 + digit;
		(*cursor)++;
	}
	if (!digits)
		return -EINVAL;
	*value = result;
	return 0;
}

/* `ids` must be exactly "vvvv:dddd[,vvvv:dddd...]", at most MAX_TARGET_IDS entries. */
static int parse_ids(void)
{
	const char *cursor = ids;

	for (;;) {
		u16 vendor, device;

		if (target_id_count == MAX_TARGET_IDS) {
			pr_err("ids: more than %d entries\n", MAX_TARGET_IDS);
			return -EINVAL;
		}
		if (parse_hex16(&cursor, &vendor) || *cursor++ != ':' ||
		    parse_hex16(&cursor, &device)) {
			pr_err("ids: cannot parse '%s', expected vendor:device in hex, e.g. 1002:7551\n",
			       ids);
			return -EINVAL;
		}
		target_vendors[target_id_count] = vendor;
		target_devices[target_id_count] = device;
		target_id_count++;
		if (!*cursor)
			return 0;
		if (*cursor++ != ',') {
			pr_err("ids: cannot parse '%s', entries are separated by commas\n", ids);
			return -EINVAL;
		}
	}
}

static bool is_target(struct pci_dev *dev)
{
	int index;

	for (index = 0; index < target_id_count; index++)
		if (dev->vendor == target_vendors[index] && dev->device == target_devices[index])
			return true;
	return false;
}

static bool is_bridge(struct pci_dev *dev)
{
	return dev->subordinate || (dev->class >> 8) == PCI_CLASS_BRIDGE_PCI;
}

static bool is_display(struct pci_dev *dev)
{
	return (dev->class >> 16) == PCI_BASE_CLASS_DISPLAY;
}

/*
 * HDMI/DP audio function that sits next to a display controller (function 0 of the same slot).
 * Called from pci_walk_bus(), which already holds pci_bus_sem, so walk the bus list directly
 * instead of pci_get_slot() (which would take the semaphore again).
 */
static bool is_gpu_audio_function(struct pci_dev *dev)
{
	struct pci_dev *sibling;

	if ((dev->class >> 8) != PCI_CLASS_MULTIMEDIA_HD_AUDIO || PCI_FUNC(dev->devfn) == 0)
		return false;
	list_for_each_entry(sibling, &dev->bus->devices, bus_list)
		if (sibling->devfn == PCI_DEVFN(PCI_SLOT(dev->devfn), 0))
			return is_display(sibling);
	return false;
}

/*
 * A switch's own management endpoint (a Broadcom PEX880xx shows one as a storage-class device)
 * sits behind a port of the same switch, so it has the same vendor as the bridge above it.
 */
static bool is_switch_endpoint(struct pci_dev *dev)
{
	struct pci_dev *upstream = dev->bus->self;

	return upstream && pci_is_pcie(upstream) &&
	       pci_pcie_type(upstream) == PCI_EXP_TYPE_DOWNSTREAM &&
	       upstream->vendor == dev->vendor;
}

struct subtree_check {
	struct pci_dev *blocker;
	const char *reason;
};

/* pci_walk_bus callback: is anything below the root port in use or not ours to move? */
static int check_subtree_device(struct pci_dev *dev, void *check_pointer)
{
	struct subtree_check *check = check_pointer;

	if (dev->driver && strcmp(dev->driver->name, "pcieport") != 0)
		check->reason = "has a driver bound";
	else if (dev == vga_default_device())
		check->reason = "is the boot display";
	else if (!allow_other_devices && !is_bridge(dev) && !is_display(dev) &&
		 !is_gpu_audio_function(dev) && !is_switch_endpoint(dev))
		check->reason = "is not a GPU (set allow_other_devices=1 to move it anyway)";
	else
		return 0;
	check->blocker = pci_dev_get(dev);
	return 1;
}

/* pci_walk_bus callback: count endpoints that should come back after re-enumeration. */
static int count_endpoint(struct pci_dev *dev, void *count_pointer)
{
	if (!is_bridge(dev) && !(drop_gpu_audio && is_gpu_audio_function(dev)))
		(*(int *)count_pointer)++;
	return 0;
}

static bool has_unassigned_bar(struct pci_dev *dev)
{
	int bar;

	for (bar = 0; bar < PCI_STD_NUM_BARS; bar++) {
		struct resource *resource = &dev->resource[bar];

		if ((resource->flags & IORESOURCE_MEM) && resource_size(resource) &&
		    !resource->parent)
			return true;
	}
	return false;
}

/* pci_walk_bus callback: log memory BARs that did not get an address. */
static int check_assigned(struct pci_dev *dev, void *unassigned_pointer)
{
	int bar;

	if (is_bridge(dev))
		return 0;
	for (bar = 0; bar < PCI_STD_NUM_BARS; bar++) {
		struct resource *resource = &dev->resource[bar];

		if (!(resource->flags & IORESOURCE_MEM) || !resource_size(resource) ||
		    resource->parent)
			continue;
		pci_err(dev, "BAR%d (%llu MB) was not assigned an address\n", bar,
			(unsigned long long)resource_size(resource) >> 20);
		(*(int *)unassigned_pointer)++;
	}
	return 0;
}

static int read_config(struct pci_dev *dev, int where, u32 *value)
{
	if (pci_read_config_dword(dev, where, value) != PCIBIOS_SUCCESSFUL || PCI_POSSIBLE_ERROR(*value))
		return -EIO;
	return 0;
}

/*
 * REBAR_FAILED: nothing changed. REBAR_UNCERTAIN: the card may be left with memory decoding off or
 * a size we couldn't confirm, so it has to be re-enumerated, and it still counts as a failure.
 */
enum rebar_result { REBAR_FAILED, REBAR_ALREADY_SET, REBAR_CHANGED, REBAR_UNCERTAIN };

/* Program BAR0's Resizable BAR control register to the largest supported size. */
static enum rebar_result set_bar0_rebar_size(struct pci_dev *dev, int *requested_log2)
{
	int capability = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_REBAR);
	u32 first_control, control, original_control, supported_sizes, written;
	int bar_count, entry, target_log2, current_log2;
	u16 command, command_now;

	if (!capability) {
		pci_warn(dev, "no Resizable BAR capability\n");
		return REBAR_FAILED;
	}
	if (read_config(dev, capability + PCI_REBAR_CTRL, &first_control))
		goto config_error;
	bar_count = (first_control & PCI_REBAR_CTRL_NBAR_MASK) >> PCI_REBAR_CTRL_NBAR_SHIFT;

	for (entry = 0; entry < bar_count; entry++) {
		int control_offset = capability + PCI_REBAR_CTRL + entry * 8;

		if (read_config(dev, control_offset, &control))
			goto config_error;
		if ((control & PCI_REBAR_CTRL_BAR_IDX) != 0)
			continue;
		if (read_config(dev, capability + PCI_REBAR_CAP + entry * 8, &supported_sizes))
			goto config_error;
		for (target_log2 = min_t(unsigned int, max_log2_mb, 27); target_log2 >= 0; target_log2--)
			if (supported_sizes & BIT(target_log2 + 4))
				break;
		if (target_log2 < 0) {
			pci_warn(dev, "no usable BAR0 size\n");
			return REBAR_FAILED;
		}
		current_log2 = (control >> 8) & 0x3f;
		if (current_log2 == target_log2) {
			pci_info(dev, "BAR0 is already %llu MB\n", 1ULL << target_log2);
			return REBAR_ALREADY_SET;
		}

		*requested_log2 = target_log2;

		/* Stop memory decoding while the BAR changes size; re-enumeration re-enables it. */
		if (pci_read_config_word(dev, PCI_COMMAND, &command) != PCIBIOS_SUCCESSFUL) {
			pci_warn(dev, "cannot read the command register, leaving BAR0 alone\n");
			return REBAR_FAILED;
		}
		if (pci_write_config_word(dev, PCI_COMMAND, command & ~PCI_COMMAND_MEMORY) !=
			    PCIBIOS_SUCCESSFUL ||
		    pci_read_config_word(dev, PCI_COMMAND, &command_now) != PCIBIOS_SUCCESSFUL ||
		    (command_now & PCI_COMMAND_MEMORY)) {
			pci_warn(dev, "cannot turn off memory decoding, leaving BAR0 alone\n");
			if (pci_write_config_word(dev, PCI_COMMAND, command) != PCIBIOS_SUCCESSFUL)
				return REBAR_UNCERTAIN;
			return REBAR_FAILED;
		}
		original_control = control;
		control = (control & ~(0x3fu << 8)) | ((u32)target_log2 << 8);
		if (pci_write_config_dword(dev, control_offset, control) != PCIBIOS_SUCCESSFUL ||
		    read_config(dev, control_offset, &written)) {
			/* Unknown size: keep decoding off and let re-enumeration size the BAR. */
			pci_warn(dev, "cannot confirm BAR0's new size, re-enumerating to recover\n");
			return REBAR_UNCERTAIN;
		}
		if (((written >> 8) & 0x3f) != target_log2) {
			pci_warn(dev, "BAR0 size did not change to %llu MB\n", 1ULL << target_log2);
			if (pci_write_config_dword(dev, control_offset, original_control) ==
				    PCIBIOS_SUCCESSFUL &&
			    !read_config(dev, control_offset, &written) && written == original_control &&
			    pci_write_config_word(dev, PCI_COMMAND, command) == PCIBIOS_SUCCESSFUL)
				return REBAR_FAILED;
			return REBAR_UNCERTAIN;
		}
		pci_info(dev, "BAR0 size %llu MB -> %llu MB\n", 1ULL << current_log2,
			 1ULL << target_log2);
		return REBAR_CHANGED;
	}
	pci_warn(dev, "BAR0 is not resizable\n");
	return REBAR_FAILED;

config_error:
	pci_warn(dev, "cannot read the Resizable BAR capability\n");
	return REBAR_FAILED;
}

/*
 * Pad the BARs on one bus segment so each bridge window is a multiple of its alignment.
 *
 * Linux sizes a bridge window as the sum of its children's sizes. When a large BAR shares a
 * window with smaller ones (R9700: 32 GB + 2 MB doorbell; RTX 3090: 256 MB + 32 MB, and 16 MB
 * registers + 16 KB audio), the sum is smaller than the space the cards really need once the next
 * card is aligned, and assignment fails. Raising the alignment of every smaller BAR to the largest
 * BAR of the same kind on the segment makes the sums exact. Same as
 * pci_request_resource_alignment(..., resize=false): the BAR keeps its size, only its start
 * alignment grows.
 */
static bool is_prefetchable_64bit(const struct resource *resource)
{
	return (resource->flags & IORESOURCE_PREFETCH) && (resource->flags & IORESOURCE_MEM_64);
}

static void pad_bars_on_bus(struct pci_bus *bus)
{
	resource_size_t largest_prefetchable = 0, largest_other = 0;
	struct pci_dev *dev;
	int bar;

	list_for_each_entry(dev, &bus->devices, bus_list) {
		for (bar = 0; bar <= PCI_ROM_RESOURCE; bar++) {  /* standard BARs + expansion ROM */
			struct resource *resource = &dev->resource[bar];

			if (!(resource->flags & IORESOURCE_MEM) || !resource_size(resource))
				continue;
			if (is_prefetchable_64bit(resource))
				largest_prefetchable = max(largest_prefetchable, resource_size(resource));
			else
				largest_other = max(largest_other, resource_size(resource));
		}
	}

	list_for_each_entry(dev, &bus->devices, bus_list) {
		for (bar = 0; bar <= PCI_ROM_RESOURCE; bar++) {  /* standard BARs + expansion ROM */
			struct resource *resource = &dev->resource[bar];
			resource_size_t size = resource_size(resource), alignment;

			if (!(resource->flags & IORESOURCE_MEM) || !size ||
			    (resource->flags & IORESOURCE_PCI_FIXED))
				continue;
			alignment = is_prefetchable_64bit(resource) ? largest_prefetchable : largest_other;
			if (size >= alignment)
				continue;
			resource->flags &= ~IORESOURCE_SIZEALIGN;
			resource->flags |= IORESOURCE_STARTALIGN | IORESOURCE_UNSET;
			resource->start = alignment;
			resource->end = alignment + size - 1;
			pci_info(dev, "BAR%d (%llu KB) aligned to %llu KB\n", bar,
				 (unsigned long long)size >> 10, (unsigned long long)alignment >> 10);
		}
	}
}

/* pci_walk_bus callback: pad the bus segment below every bridge in the re-enumerated subtree. */
static int pad_bars_below_bridge(struct pci_dev *dev, void *unused)
{
	if (dev->subordinate)
		pad_bars_on_bus(dev->subordinate);
	return 0;
}

struct dropped_audio_list {
	struct pci_dev *devices[MAX_DROPPED_AUDIO];
	int count;
};

/*
 * pci_walk_bus callback, after scan and before assignment: clear the BARs of GPU audio functions
 * so they take no address space, and remember them so they can be removed before any driver sees
 * them.
 */
static int drop_gpu_audio_resources(struct pci_dev *dev, void *list_pointer)
{
	struct dropped_audio_list *dropped = list_pointer;
	int bar;

	if (!is_gpu_audio_function(dev))
		return 0;
	if (dropped->count == MAX_DROPPED_AUDIO) {
		pci_warn(dev, "more than %d GPU audio functions, keeping this one\n",
			 MAX_DROPPED_AUDIO);
		return 0;
	}
	for (bar = 0; bar <= PCI_ROM_RESOURCE; bar++) {
		dev->resource[bar].flags = 0;
		dev->resource[bar].start = 0;
		dev->resource[bar].end = 0;
	}
	dropped->devices[dropped->count++] = pci_dev_get(dev);
	return 0;
}

static int __init gpu_rebar_layout_init(void)
{
	struct pci_dev *targets[MAX_TARGETS];
	struct pci_dev *root_ports[MAX_TARGETS];
	struct pci_bus *root_buses[MAX_TARGETS];
	struct pci_bus *root_port_buses[MAX_TARGETS];
	unsigned int root_port_devfns[MAX_TARGETS];
	static struct dropped_audio_list dropped_audio;
	int target_count = 0, root_port_count = 0, root_bus_count = 0;
	int changed_count = 0, needs_layout_count = 0, missing_root_ports = 0, failed_count = 0;
	int requested_log2[MAX_TARGETS], target_domains[MAX_TARGETS];
	unsigned int target_bus_numbers[MAX_TARGETS], target_devfns[MAX_TARGETS];
	int wrong_size_count = 0;
	int endpoints_before = 0, endpoints_after = 0, unassigned_bars = 0;
	int index, inner, result = 0;
	struct pci_dev *dev = NULL;

	if (!ids || !*ids)
		return 0;
	result = parse_ids();
	if (result)
		return result;

	/* Hold the rescan/remove lock for the whole run so the topology can't change under us. */
	pci_lock_rescan_remove();

	/* Pass 1: collect target devices and the root ports above them. Change nothing yet. */
	while ((dev = pci_get_device(PCI_ANY_ID, PCI_ANY_ID, dev))) {
		struct pci_dev *root_port;
		bool already_listed = false;

		if (!is_target(dev))
			continue;
		if (target_count == MAX_TARGETS) {
			pci_warn(dev, "more than %d matching devices, leaving this one alone\n",
				 MAX_TARGETS);
			continue;
		}
		root_port = pcie_find_root_port(dev);
		if (!root_port) {
			pci_warn(dev, "no root port found, skipping\n");
			continue;
		}
		targets[target_count++] = pci_dev_get(dev);
		for (index = 0; index < root_port_count; index++)
			if (root_ports[index] == root_port)
				already_listed = true;
		if (!already_listed)
			root_ports[root_port_count++] = pci_dev_get(root_port);
	}

	if (!target_count) {
		pr_info("no matching devices\n");
		goto release_references;
	}

	/* Pass 2: refuse to touch a subtree that has anything in use, or anything that isn't ours. */
	for (index = 0; index < root_port_count; index++) {
		struct subtree_check check = { };

		if (!root_ports[index]->subordinate)
			continue;
		pci_walk_bus(root_ports[index]->subordinate, check_subtree_device, &check);
		if (check.blocker) {
			pci_err(check.blocker, "%s, below root port %s. Nothing changed.\n",
				check.reason, pci_name(root_ports[index]));
			pci_dev_put(check.blocker);
			result = -EBUSY;
			goto release_references;
		}
		pci_walk_bus(root_ports[index]->subordinate, count_endpoint, &endpoints_before);
	}

	/* Pass 3: program the Resizable BAR sizes. */
	for (index = 0; index < target_count; index++) {
		requested_log2[index] = -1;
		target_domains[index] = pci_domain_nr(targets[index]->bus);
		target_bus_numbers[index] = targets[index]->bus->number;
		target_devfns[index] = targets[index]->devfn;
		switch (set_bar0_rebar_size(targets[index], &requested_log2[index])) {
		case REBAR_CHANGED:
			changed_count++;
			needs_layout_count++;
			break;
		case REBAR_UNCERTAIN:
			failed_count++;
			needs_layout_count++;
			break;
		case REBAR_ALREADY_SET:
			/* Right size but a BAR without an address: the windows still need laying out. */
			if (has_unassigned_bar(targets[index]))
				needs_layout_count++;
			break;
		case REBAR_FAILED:
			requested_log2[index] = -1;
			failed_count++;
			break;
		}
	}
	if (!needs_layout_count) {
		if (failed_count) {
			pr_err("%d card(s) could not be resized, nothing changed\n", failed_count);
			result = -EIO;
		} else {
			pr_info("nothing to do\n");
		}
		goto release_references;
	}

	/* Pass 4: re-enumerate below each root port with the small BARs aligned. */
	for (index = 0; index < root_port_count; index++) {
		struct pci_bus *root_bus = root_ports[index]->bus;
		bool bus_listed = false;

		for (inner = 0; inner < root_bus_count; inner++)
			if (root_buses[inner] == root_bus)
				bus_listed = true;
		if (!bus_listed)
			root_buses[root_bus_count++] = root_bus;

		root_port_devfns[index] = root_ports[index]->devfn;
		root_port_buses[index] = root_bus;
		pci_info(root_ports[index], "removing for re-enumeration\n");
		pci_stop_and_remove_bus_device(root_ports[index]);
	}
	for (index = 0; index < root_bus_count; index++)
		pci_scan_child_bus(root_buses[index]);

	/* Re-acquire the root ports; the old pci_dev structs were removed. */
	for (index = 0; index < root_port_count; index++) {
		pci_dev_put(root_ports[index]);
		root_ports[index] = pci_get_slot(root_port_buses[index], root_port_devfns[index]);
		if (!root_ports[index]) {
			pr_err("root port %02x.%d did not come back after rescan\n",
			       PCI_SLOT(root_port_devfns[index]), PCI_FUNC(root_port_devfns[index]));
			missing_root_ports++;
		}
	}

	/* Strip GPU audio functions before anything can bind to them, then pad. */
	for (index = 0; index < root_port_count; index++)
		if (drop_gpu_audio && root_ports[index] && root_ports[index]->subordinate)
			pci_walk_bus(root_ports[index]->subordinate, drop_gpu_audio_resources,
				     &dropped_audio);
	for (index = 0; index < dropped_audio.count; index++) {
		pci_info(dropped_audio.devices[index], "removing GPU audio function\n");
		pci_stop_and_remove_bus_device(dropped_audio.devices[index]);
		pci_dev_put(dropped_audio.devices[index]);
	}
	for (index = 0; index < root_port_count; index++) {
		if (!pad || !root_ports[index] || !root_ports[index]->subordinate)
			continue;
		pad_bars_on_bus(root_ports[index]->subordinate);
		pci_walk_bus(root_ports[index]->subordinate, pad_bars_below_bridge, NULL);
	}

	for (index = 0; index < root_bus_count; index++) {
		pci_assign_unassigned_bus_resources(root_buses[index]);
		pci_bus_add_devices(root_buses[index]);
	}

	/* Pass 5: check that everything came back with its memory BARs assigned. */
	for (index = 0; index < root_port_count; index++) {
		if (!root_ports[index] || !root_ports[index]->subordinate)
			continue;
		pci_walk_bus(root_ports[index]->subordinate, count_endpoint, &endpoints_after);
		pci_walk_bus(root_ports[index]->subordinate, check_assigned, &unassigned_bars);
	}

	/* Check that every card we resized came back with the size we asked for. */
	for (index = 0; index < target_count; index++) {
		struct pci_dev *rescanned;
		resource_size_t expected;

		if (requested_log2[index] < 0)
			continue;
		expected = (resource_size_t)1 << (requested_log2[index] + 20);
		rescanned = pci_get_domain_bus_and_slot(target_domains[index],
							target_bus_numbers[index],
							target_devfns[index]);
		if (!rescanned || resource_size(&rescanned->resource[0]) != expected) {
			pr_err("%04x:%02x:%02x.%d: BAR0 is not %llu MB after the rescan\n",
			       target_domains[index], target_bus_numbers[index],
			       PCI_SLOT(target_devfns[index]), PCI_FUNC(target_devfns[index]),
			       (unsigned long long)expected >> 20);
			wrong_size_count++;
		}
		pci_dev_put(rescanned);
	}

	if (missing_root_ports || endpoints_after != endpoints_before || unassigned_bars ||
	    wrong_size_count) {
		pr_err("re-enumeration finished with problems: %d missing root port(s), %d of %d devices back, %d memory BAR(s) unassigned, %d BAR0(s) the wrong size. Reboot without gpu_rebar_layout.ids= to undo.\n",
		       missing_root_ports, endpoints_after, endpoints_before, unassigned_bars,
		       wrong_size_count);
		result = -EIO;
	} else if (failed_count) {
		pr_err("re-enumeration done, but %d card(s) could not be resized\n", failed_count);
		result = -EIO;
	} else {
		pr_info("re-enumeration done: %d BAR(s) resized, %d device(s) back, all memory BARs assigned\n",
			changed_count, endpoints_after);
	}

release_references:
	pci_unlock_rescan_remove();
	for (index = 0; index < target_count; index++)
		pci_dev_put(targets[index]);
	for (index = 0; index < root_port_count; index++)
		pci_dev_put(root_ports[index]);
	return result;
}

/* Nothing to undo: the module only acts once, at load time. */
static void __exit gpu_rebar_layout_exit(void)
{
}

module_init(gpu_rebar_layout_init);
module_exit(gpu_rebar_layout_exit);
MODULE_AUTHOR("Doug Calobrisi");
MODULE_VERSION("1.1");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Enlarge Resizable BARs of selected GPUs and re-lay out PCI windows");
