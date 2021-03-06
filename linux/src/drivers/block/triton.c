/*
 *  linux/drivers/block/triton.c	Version 1.13  Aug 12, 1996
 *					Version 1.13a June 1998 - new chipsets
 *					Version 1.13b July 1998 - DMA blacklist
 *					Version 1.14  June 22, 1999
 *
 *  Copyright (c) 1998-1999  Andre Hedrick
 *  Copyright (c) 1995-1996  Mark Lord
 *  May be copied or modified under the terms of the GNU General Public License
 */

/*
 * This module provides support for Bus Master IDE DMA functions in various
 * motherboard chipsets and PCI controller cards.
 * Please check /Documentation/ide.txt and /Documentation/udma.txt for details.
 */

#include <linux/config.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/timer.h>
#include <linux/mm.h>
#include <linux/ioport.h>
#include <linux/interrupt.h>
#include <linux/blkdev.h>
#include <linux/hdreg.h>
#include <linux/pci.h>
#include <linux/bios32.h>

#include <asm/io.h>
#include <asm/dma.h>
#include <asm/irq.h>

#include "ide.h"

#undef DISPLAY_TRITON_TIMINGS	/* define this to display timings */
#undef DISPLAY_APOLLO_TIMINGS	/* define this for extensive debugging information */
#undef DISPLAY_ALI15X3_TIMINGS	/* define this for extensive debugging information */

#if defined(CONFIG_PROC_FS)
#include <linux/stat.h>
#include <linux/proc_fs.h>
#ifdef DISPLAY_APOLLO_TIMINGS
#include <linux/via_ide_dma.h>
#endif
#ifdef DISPLAY_ALI15X3_TIMINGS
#include <linux/ali_ide_dma.h>
#endif
#endif

/*
 * good_dma_drives() lists the model names (from "hdparm -i")
 * of drives which do not support mword2 DMA but which are
 * known to work fine with this interface under Linux.
 */
const char *good_dma_drives[] = {"Micropolis 2112A",
				 "CONNER CTMA 4000",
				 "CONNER CTT8000-A",
				 "QEMU HARDDISK",
				 NULL};

/*
 * bad_dma_drives() lists the model names (from "hdparm -i")
 * of drives which supposedly support (U)DMA but which are
 * known to corrupt data with this interface under Linux.
 *
 * Note: the list was generated by statistical analysis of problem
 * reports. It's not clear if there are problems with the drives,
 * or with some combination of drive/controller or what. 
 *
 * You can forcibly override this if you wish. This is the kernel
 * 'Tread carefully' list.
 *
 * Finally see http://www.wdc.com/quality/err-rec.html if you have
 * one of the listed drives. 
 */
const char *bad_dma_drives[] = {"WDC AC11000H",
				"WDC AC22100H",
				"WDC AC32500H",
				"WDC AC33100H",
				 NULL};

/*
 * Our Physical Region Descriptor (PRD) table should be large enough
 * to handle the biggest I/O request we are likely to see.  Since requests
 * can have no more than 256 sectors, and since the typical blocksize is
 * two sectors, we could get by with a limit of 128 entries here for the
 * usual worst case.  Most requests seem to include some contiguous blocks,
 * further reducing the number of table entries required.
 *
 * The driver reverts to PIO mode for individual requests that exceed
 * this limit (possible with 512 byte blocksizes, eg. MSDOS f/s), so handling
 * 100% of all crazy scenarios here is not necessary.
 *
 * As it turns out though, we must allocate a full 4KB page for this,
 * so the two PRD tables (ide0 & ide1) will each get half of that,
 * allowing each to have about 256 entries (8 bytes each) from this.
 */
#define PRD_BYTES	8
#define PRD_ENTRIES	(PAGE_SIZE / (2 * PRD_BYTES))
#define DEFAULT_BMIBA	0xe800	/* in case BIOS did not init it */
#define DEFAULT_BMCRBA  0xcc00  /* VIA's default value */
#define DEFAULT_BMALIBA	0xd400	/* ALI's default value */

/*
 * dma_intr() is the handler for disk read/write DMA interrupts
 */
static void dma_intr (ide_drive_t *drive)
{
	byte stat, dma_stat;
	int i;
	struct request *rq = HWGROUP(drive)->rq;
	unsigned short dma_base = HWIF(drive)->dma_base;

	dma_stat = inb(dma_base+2);		/* get DMA status */
	outb(inb(dma_base)&~1, dma_base);	/* stop DMA operation */
	stat = GET_STAT();			/* get drive status */
	if (OK_STAT(stat,DRIVE_READY,drive->bad_wstat|DRQ_STAT)) {
		if ((dma_stat & 7) == 4) {	/* verify good DMA status */
			rq = HWGROUP(drive)->rq;
			for (i = rq->nr_sectors; i > 0;) {
				i -= rq->current_nr_sectors;
				ide_end_request(1, HWGROUP(drive));
			}
			return;
		}
		printk("%s: bad DMA status: 0x%02x\n", drive->name, dma_stat);
	}
	sti();
	ide_error(drive, "dma_intr", stat);
}

/*
 * build_dmatable() prepares a dma request.
 * Returns 0 if all went okay, returns 1 otherwise.
 */
static int build_dmatable (ide_drive_t *drive)
{
	struct request *rq = HWGROUP(drive)->rq;
	struct buffer_head *bh = rq->bh;
	unsigned long size, addr, *table = HWIF(drive)->dmatable;
	unsigned int count = 0;

	do {
		/*
		 * Determine addr and size of next buffer area.  We assume that
		 * individual virtual buffers are always composed linearly in
		 * physical memory.  For example, we assume that any 8kB buffer
		 * is always composed of two adjacent physical 4kB pages rather
		 * than two possibly non-adjacent physical 4kB pages.
		 */
		if (bh == NULL) {  /* paging and tape requests have (rq->bh == NULL) */
			addr = virt_to_bus (rq->buffer);
#ifdef CONFIG_BLK_DEV_IDETAPE
			if (drive->media == ide_tape)
				size = drive->tape.pc->request_transfer;
			else
#endif /* CONFIG_BLK_DEV_IDETAPE */	
			size = rq->nr_sectors << 9;
		} else {
			/* group sequential buffers into one large buffer */
			addr = virt_to_bus (bh->b_data);
			size = bh->b_size;
			while ((bh = bh->b_reqnext) != NULL) {
				if ((addr + size) != virt_to_bus (bh->b_data))
					break;
				size += bh->b_size;
			}
		}

		/*
		 * Fill in the dma table, without crossing any 64kB boundaries.
		 * We assume 16-bit alignment of all blocks.
		 */
		while (size) {
			if (++count >= PRD_ENTRIES) {
				printk("%s: DMA table too small\n", drive->name);
				return 1; /* revert to PIO for this request */
			} else {
				unsigned long bcount = 0x10000 - (addr & 0xffff);
				if (bcount > size)
					bcount = size;
				*table++ = addr;
				*table++ = bcount & 0xffff;
				addr += bcount;
				size -= bcount;
			}
		}
	} while (bh != NULL);
	if (count) {
		*--table |= 0x80000000;	/* set End-Of-Table (EOT) bit */
		return 0;
	}
	printk("%s: empty DMA table?\n", drive->name);
	return 1;	/* let the PIO routines handle this weirdness */
}

/*
 * We will only enable drives with multi-word (mode2) (U)DMA capabilities,
 * and ignore the very rare cases of drives that can only do single-word
 * (modes 0 & 1) (U)DMA transfers. We also discard "blacklisted" hard disks.
 */
static int config_drive_for_dma (ide_drive_t *drive)
{
#ifndef CONFIG_BLK_DEV_FORCE_DMA
	const char **list;
	struct hd_driveid *id = drive->id;
#endif

#ifdef CONFIG_BLK_DEV_FORCE_DMA
	drive->using_dma = 1;
	return 0;
#else
	if (HWIF(drive)->chipset == ide_hpt343) {
		drive->using_dma = 0;	/* no DMA */
		return 1;	/* DMA disabled */
	}

	if (id && (id->capability & 1)) {
		/* Consult the list of known "bad" drives */
		list = bad_dma_drives;
		while (*list) {
			if (!strcmp(*list++,id->model)) {
				drive->using_dma = 0;   /* no DMA */
				printk("ide: Disabling DMA modes on %s drive (%s).\n", drive->name, id->model);
				return 1;	/* DMA disabled */
			}
		}

		if (!strcmp("QEMU HARDDISK", id->model)) {
			/* Virtual disks don't have issues with DMA :) */
			drive->using_dma = 1;
			/* And keep enabled even if some requests time out due to emulation lag. */
			drive->keep_settings = 1;
			return 1;		/* DMA enabled */
		}
		/* Enable DMA on any drive that has mode 4 or 2 UltraDMA enabled */
		if (id->field_valid & 4) {	/* UltraDMA */
			/* Enable DMA on any drive that has mode 4 UltraDMA enabled */
			if (((id->dma_ultra & 0x1010) == 0x1010) &&
			    (id->word93 & 0x2000) &&
			    (HWIF(drive)->chipset == ide_ultra66)) {
				drive->using_dma = 1;
				return 0;	/* DMA enabled */
			} else
			/* Enable DMA on any drive that has mode 2 UltraDMA enabled */
				if ((id->dma_ultra & 0x404) == 0x404) {
				drive->using_dma = 1;
				return 0;	/* DMA enabled */
			}
		}
		/* Enable DMA on any drive that has mode2 DMA enabled */
		if (id->field_valid & 2)	/* regular DMA */
			if ((id->dma_mword & 0x404) == 0x404) {
				drive->using_dma = 1;
				return 0;	/* DMA enabled */
			}
		/* Consult the list of known "good" drives */
		list = good_dma_drives;
		while (*list) {
			if (!strcmp(*list++,id->model)) {
				drive->using_dma = 1;
				return 0;	/* DMA enabled */
			}
		}
	}
	return 1;	/* DMA not enabled */
#endif
}

/*
 * triton_dmaproc() initiates/aborts DMA read/write operations on a drive.
 *
 * The caller is assumed to have selected the drive and programmed the drive's
 * sector address using CHS or LBA.  All that remains is to prepare for DMA
 * and then issue the actual read/write DMA/PIO command to the drive.
 *
 * For ATAPI devices, we just prepare for DMA and return. The caller should
 * then issue the packet command to the drive and call us again with
 * ide_dma_begin afterwards.
 *
 * Returns 0 if all went well.
 * Returns 1 if DMA read/write could not be started, in which case
 * the caller should revert to PIO for the current request.
 */
static int triton_dmaproc (ide_dma_action_t func, ide_drive_t *drive)
{
	unsigned long dma_base = HWIF(drive)->dma_base;
	unsigned int reading = (1 << 3);

	switch (func) {
		case ide_dma_abort:
			outb(inb(dma_base)&~1, dma_base);	/* stop DMA */
			return 0;
		case ide_dma_check:
			return config_drive_for_dma (drive);
		case ide_dma_write:
			reading = 0;
		case ide_dma_read:
			break;
		case ide_dma_status_bad:
			return ((inb(dma_base+2) & 7) != 4);	/* verify good DMA status */
		case ide_dma_transferred:
#if 0
			return (number of bytes actually transferred);
#else
			return (0);
#endif
		case ide_dma_begin:
			outb(inb(dma_base)|1, dma_base);	/* begin DMA */
			return 0;
		default:
			printk("triton_dmaproc: unsupported func: %d\n", func);
			return 1;
	}
	if (build_dmatable (drive))
		return 1;
	outl(virt_to_bus (HWIF(drive)->dmatable), dma_base + 4); /* PRD table */
	outb(reading, dma_base);			/* specify r/w */
	outb(inb(dma_base+2)|0x06, dma_base+2);		/* clear status bits */
#ifdef CONFIG_BLK_DEV_IDEATAPI
	if (drive->media != ide_disk)
		return 0;
#endif /* CONFIG_BLK_DEV_IDEATAPI */	
	ide_set_handler(drive, &dma_intr, WAIT_CMD);	/* issue cmd to drive */
	OUT_BYTE(reading ? WIN_READDMA : WIN_WRITEDMA, IDE_COMMAND_REG);
	outb(inb(dma_base)|1, dma_base);		/* begin DMA */
	return 0;
}

#ifdef DISPLAY_TRITON_TIMINGS
/*
 * print_triton_drive_flags() displays the currently programmed options
 * in the i82371 (Triton) for a given drive.
 *
 *	If fastDMA  is "no", then slow ISA timings are used for DMA data xfers.
 *	If fastPIO  is "no", then slow ISA timings are used for PIO data xfers.
 *	If IORDY    is "no", then IORDY is assumed to always be asserted.
 *	If PreFetch is "no", then data pre-fetch/post are not used.
 *
 * When "fastPIO" and/or "fastDMA" are "yes", then faster PCI timings and
 * back-to-back 16-bit data transfers are enabled, using the sample_CLKs
 * and recovery_CLKs (PCI clock cycles) timing parameters for that interface.
 */
static void print_triton_drive_flags (unsigned int unit, byte flags)
{
	printk("         %s ", unit ? "slave :" : "master:");
	printk( "fastDMA=%s",	(flags&9)	? "on " : "off");
	printk(" PreFetch=%s",	(flags&4)	? "on " : "off");
	printk(" IORDY=%s",	(flags&2)	? "on " : "off");
	printk(" fastPIO=%s\n",	((flags&9)==1)	? "on " : "off");
}
#endif /* DISPLAY_TRITON_TIMINGS */

static void init_triton_dma (ide_hwif_t *hwif, unsigned short base)
{
	static unsigned long dmatable = 0;

	printk("    %s: BM-DMA at 0x%04x-0x%04x", hwif->name, base, base+7);
	if (check_region(base, 8)) {
		printk(" -- ERROR, PORTS ALREADY IN USE");
	} else {
		request_region(base, 8, "IDE DMA");
		hwif->dma_base = base;
		if (!dmatable) {
			/*
			 * The BM-DMA uses a full 32-bits, so we can
			 * safely use __get_free_page() here instead
			 * of __get_dma_pages() -- no ISA limitations.
			 */
			dmatable = __get_free_pages(GFP_KERNEL, 1, 0);
		}
		if (dmatable) {
			hwif->dmatable = (unsigned long *) dmatable;
			dmatable += (PRD_ENTRIES * PRD_BYTES);
			outl(virt_to_bus(hwif->dmatable), base + 4);
			hwif->dmaproc  = &triton_dmaproc;
		}
	}
	printk("\n");
}

/*
 *  Set VIA Chipset Timings for (U)DMA modes enabled.
 */
static int set_via_timings (byte bus, byte fn, byte post, byte flush)
{
	byte via_config = 0;
	int rc = 0;

	/* setting IDE read prefetch buffer and IDE post write buffer */
	if ((rc = pcibios_read_config_byte(bus, fn, 0x41, &via_config)))
		return (1);
	if ((rc = pcibios_write_config_byte(bus, fn, 0x41, via_config | post)))
		return (1);

	/* setting Channel read and End-of-sector FIFO flush: */
	if ((rc = pcibios_read_config_byte(bus, fn, 0x46, &via_config)))
		return (1);
	if ((rc = pcibios_write_config_byte(bus, fn, 0x46, via_config | flush)))
		return (1);

	return (0);
}

static int setup_aladdin (byte bus, byte fn)
{
	byte confreg0 = 0, confreg1 = 0, progif = 0;
	int errors = 0;

	if (pcibios_read_config_byte(bus, fn, 0x50, &confreg1))
		goto veryspecialsettingserror;
	if (!(confreg1 & 0x02))
		if (pcibios_write_config_byte(bus, fn, 0x50, confreg1 | 0x02))
			goto veryspecialsettingserror;

	if (pcibios_read_config_byte(bus, fn, 0x09, &progif))
		goto veryspecialsettingserror;
	if (!(progif & 0x40)) {
		/*
		 * The way to enable them is to set progif
		 * writable at 0x4Dh register, and set bit 6
		 * of progif to 1:
		 */
		if (pcibios_read_config_byte(bus, fn, 0x4d, &confreg0))
			goto veryspecialsettingserror;
		if (confreg0 & 0x80)
			if (pcibios_write_config_byte(bus, fn, 0x4d, confreg0 & ~0x80))
				goto veryspecialsettingserror;
		if (pcibios_write_config_byte(bus, fn, 0x09, progif | 0x40))
			goto veryspecialsettingserror;
		if (confreg0 & 0x80)
			if (pcibios_write_config_byte(bus, fn, 0x4d, confreg0))
				errors++;
	}

	if ((pcibios_read_config_byte(bus, fn, 0x09, &progif)) || (!(progif & 0x40)))
		goto veryspecialsettingserror;

	printk("ide: ALI15X3: enabled read of IDE channels state (en/dis-abled) %s.\n",
		errors ? "with Error(s)" : "Succeeded" );
	return 1;
veryspecialsettingserror:
	printk("ide: ALI15X3: impossible to enable read of IDE channels state (en/dis-abled)!\n");
	return 0;
}

void set_promise_hpt343_extra (unsigned short device, unsigned int bmiba)
{
	switch(device) {
		case PCI_DEVICE_ID_PROMISE_20246:
			if(!check_region((bmiba+16), 16))
				request_region((bmiba+16), 16, "PDC20246");
			break;
		case PCI_DEVICE_ID_PROMISE_20262:
			if (!check_region((bmiba+48), 48))
				request_region((bmiba+48), 48, "PDC20262");
			break;
		case PCI_DEVICE_ID_TTI_HPT343:
			if(!check_region((bmiba+16), 16))
				request_region((bmiba+16), 16, "HPT343");
			break;
		default:
			break;
	}
}

#define HPT343_PCI_INIT_REG		0x80

/*
 * ide_init_triton() prepares the IDE driver for DMA operation.
 * This routine is called once, from ide.c during driver initialization,
 * for each BM-DMA chipset which is found (rarely more than one).
 */
void ide_init_triton (byte bus, byte fn)
{
	byte bridgebus, bridgefn, bridgeset = 0, hpt34x_flag = 0;
	unsigned char irq = 0;
	int dma_enabled = 0, rc = 0, h;
	unsigned short io[6], count = 0, step_count = 0, pass_count = 0;
	unsigned short pcicmd, vendor, device, class;
	unsigned int bmiba, timings, reg, tmp;
	unsigned int addressbios = 0;
	unsigned long flags;
	unsigned index;

#if defined(DISPLAY_APOLLO_TIMINGS) || defined(DISPLAY_ALI15X3_TIMINGS)
	bmide_bus = bus;
	bmide_fn = fn;
#endif /* DISPLAY_APOLLO_TIMINGS || DISPLAY_ALI15X3_TIMINGS */

/*
 *  We pick up the vendor, device, and class info for selecting the correct
 *  controller that is supported.  Since we can access this routine more than
 *  once with the use of onboard and off-board EIDE controllers, a method
 *  of determining "who is who for what" is needed.
 */

	pcibios_read_config_word (bus, fn, PCI_VENDOR_ID, &vendor);
	pcibios_read_config_word (bus, fn, PCI_DEVICE_ID, &device);
	pcibios_read_config_word (bus, fn, PCI_CLASS_DEVICE, &class);
	pcibios_read_config_byte (bus, fn, PCI_INTERRUPT_LINE, &irq);

	switch(vendor) {
		case PCI_VENDOR_ID_INTEL:
			printk("ide: Intel 82371 ");
			switch(device) {
				case PCI_DEVICE_ID_INTEL_82371_0:
					printk("PIIX (single FIFO) ");
					break;
				case PCI_DEVICE_ID_INTEL_82371SB_1:
					printk("PIIX3 (dual FIFO) ");
					break;
				case PCI_DEVICE_ID_INTEL_82371AB:
					printk("PIIX4 (dual FIFO) ");
					break;
				default:
					printk(" (unknown) 0x%04x ", device);
					break;
			}
			printk("DMA Bus Mastering IDE ");
			break;
		case PCI_VENDOR_ID_SI:
			printk("ide: SiS 5513 (dual FIFO) DMA Bus Mastering IDE ");
			break;
                case PCI_VENDOR_ID_VIA:
			printk("ide: VIA VT82C586B (split FIFO) UDMA Bus Mastering IDE ");
			break;
		case PCI_VENDOR_ID_TTI:
			/*PCI_CLASS_STORAGE_UNKNOWN == class */
			if (device == PCI_DEVICE_ID_TTI_HPT343) {
				pcibios_write_config_byte(bus, fn, HPT343_PCI_INIT_REG, 0x00);
				pcibios_read_config_word(bus, fn, PCI_COMMAND, &pcicmd);
				hpt34x_flag = (pcicmd & PCI_COMMAND_MEMORY) ? 1 : 0;
#if 1
				if (!hpt34x_flag) {
					save_flags(flags);
					cli();
					pcibios_write_config_word(bus, fn, PCI_COMMAND, pcicmd & ~PCI_COMMAND_IO);
					pcibios_read_config_dword(bus, fn, PCI_BASE_ADDRESS_4, &bmiba);
					pcibios_write_config_dword(bus, fn, PCI_BASE_ADDRESS_0, bmiba | 0x20);
					pcibios_write_config_dword(bus, fn, PCI_BASE_ADDRESS_1, bmiba | 0x34);
					pcibios_write_config_dword(bus, fn, PCI_BASE_ADDRESS_2, bmiba | 0x28);
					pcibios_write_config_dword(bus, fn, PCI_BASE_ADDRESS_3, bmiba | 0x3c);
					pcibios_write_config_word(bus, fn, PCI_COMMAND, pcicmd);
					bmiba = 0;
					restore_flags(flags);
				}
#endif
				pcibios_write_config_byte(bus, fn, PCI_LATENCY_TIMER, 0x20); 
				goto hpt343_jump_in;
			} else {
				printk("ide: HPTXXX did == 0x%04X unsupport chipset error.\n", device);
				return;
			}
		case PCI_VENDOR_ID_PROMISE:
			/*
			 *  I have been able to make my Promise Ultra33 UDMA card change class.
			 *  It has reported as both PCI_CLASS_STORAGE_RAID and PCI_CLASS_STORAGE_IDE.
			 *  Since the PCI_CLASS_STORAGE_RAID mode should automatically mirror the
			 *  two halves of the PCI_CONFIG register data, but sometimes it forgets.
			 *  Thus we guarantee that they are identical, with a quick check and
			 *  correction if needed.
			 *  PDC20246 (primary) PDC20247 (secondary) IDE hwif's.
			 *
			 *  PDC20262 Promise Ultra66 UDMA.
			 *
			 *  Note that Promise "stories,fibs,..." about this device not being
			 *  capable of ATAPI and AT devices.
			 */
			if (class != PCI_CLASS_STORAGE_IDE) {
				unsigned char irq_mirror = 0;

				pcibios_read_config_byte(bus, fn, (PCI_INTERRUPT_LINE)|0x80, &irq_mirror);
				if (irq != irq_mirror) {
					pcibios_write_config_byte(bus, fn, (PCI_INTERRUPT_LINE)|0x80, irq);
				}
			}
		case PCI_VENDOR_ID_ARTOP:
			/*	PCI_CLASS_STORAGE_SCSI == class	*/
			/*
			 *  I have found that by stroking rom_enable_bit on both the AEC6210U/UF and
			 *  PDC20246 controller cards, the features desired are almost guaranteed
			 *  to be enabled and compatible.  This ROM may not be registered in the
			 *  config data, but it can be turned on.  Registration failure has only
			 *  been observed if and only if Linux sets up the pci_io_address in the
			 *  0x6000 range.  If they are setup in the 0xef00 range it is reported.
			 *  WHY??? got me.........
			 */
hpt343_jump_in:
			printk("ide: %s UDMA Bus Mastering ",
				(device == PCI_DEVICE_ID_ARTOP_ATP850UF) 		? "AEC6210" :
				(device == PCI_DEVICE_ID_PROMISE_20246)  		? "PDC20246" :
				(device == PCI_DEVICE_ID_PROMISE_20262)  		? "PDC20262" :
				(hpt34x_flag && (device == PCI_DEVICE_ID_TTI_HPT343))	? "HPT345" :
				(device == PCI_DEVICE_ID_TTI_HPT343)     		? "HPT343" : "UNKNOWN");
			pcibios_read_config_dword(bus, fn, PCI_ROM_ADDRESS, &addressbios);
			if (addressbios) {
				pcibios_write_config_byte(bus, fn, PCI_ROM_ADDRESS, addressbios | PCI_ROM_ADDRESS_ENABLE);
				printk("with ROM enabled at 0x%08x", addressbios);
			}
			/*
			 *  This was stripped out of 2.1.XXX kernel code and parts from a patch called
			 *  promise_update.  This finds the PCI_BASE_ADDRESS spaces and makes them
			 *  available for configuration later.
			 *  PCI_BASE_ADDRESS_0  hwif0->io_base
			 *  PCI_BASE_ADDRESS_1  hwif0->ctl_port
			 *  PCI_BASE_ADDRESS_2  hwif1->io_base
			 *  PCI_BASE_ADDRESS_3  hwif1->ctl_port
			 *  PCI_BASE_ADDRESS_4  bmiba
			 */
			memset(io, 0, 6 * sizeof(unsigned short));
			for (reg = PCI_BASE_ADDRESS_0; reg <= PCI_BASE_ADDRESS_5; reg += 4) {
				pcibios_read_config_dword(bus, fn, reg, &tmp);
				if (tmp & PCI_BASE_ADDRESS_SPACE_IO)
					io[count++] = tmp & PCI_BASE_ADDRESS_IO_MASK;
			}
			break;
		case PCI_VENDOR_ID_AL:
			save_flags(flags);
			cli();
			for (index = 0; !pcibios_find_device (PCI_VENDOR_ID_AL, PCI_DEVICE_ID_AL_M1533, index, &bridgebus, &bridgefn); ++index) {
				bridgeset = setup_aladdin(bus, fn);
			}
			restore_flags(flags);
			printk("ide: ALI15X3 (dual FIFO) DMA Bus Mastering IDE ");
			break;
		default:
			return;
	}

	printk("\n    Controller on PCI bus %d function %d\n", bus, fn);

	/*
	 * See if IDE and BM-DMA features are enabled:
	 */
	if ((rc = pcibios_read_config_word(bus, fn, PCI_COMMAND, &pcicmd)))
		goto quit;
	if ((pcicmd & 1) == 0)  {
		printk("ide: ports are not enabled (BIOS)\n");
		goto quit;
	}
	if ((pcicmd & 4) == 0) {
		printk("ide: BM-DMA feature is not enabled (BIOS), enabling\n");
		pcicmd |= 4;
		pcibios_write_config_word(bus, fn, 0x04, pcicmd);
		if ((rc = pcibios_read_config_word(bus, fn, 0x04, &pcicmd))) {
			printk("ide: Couldn't read back PCI command\n");
			goto quit;
		}
	}

	if ((pcicmd & 4) == 0) {
		printk("ide: BM-DMA feature couldn't be enabled\n");
	} else {
		/*
		 * Get the bmiba base address
		 */
		int try_again = 1;
		do {
			if ((rc = pcibios_read_config_dword(bus, fn, PCI_BASE_ADDRESS_4, &bmiba)))
				goto quit;
			bmiba &= 0xfff0;	/* extract port base address */
			if (bmiba) {
				dma_enabled = 1;
				break;
			} else {
                                printk("ide: BM-DMA base register is invalid (0x%04x, PnP BIOS problem)\n", bmiba);
                                if (inb(((vendor == PCI_VENDOR_ID_AL) ? DEFAULT_BMALIBA :
					 (vendor == PCI_VENDOR_ID_VIA) ? DEFAULT_BMCRBA :
									DEFAULT_BMIBA)) != 0xff || !try_again)
					break;
				printk("ide: setting BM-DMA base register to 0x%04x\n",
					((vendor == PCI_VENDOR_ID_AL) ? DEFAULT_BMALIBA :
					 (vendor == PCI_VENDOR_ID_VIA) ? DEFAULT_BMCRBA :
									DEFAULT_BMIBA));
				if ((rc = pcibios_write_config_word(bus, fn, PCI_COMMAND, pcicmd&~1)))
					goto quit;
				rc = pcibios_write_config_dword(bus, fn, 0x20,
					((vendor == PCI_VENDOR_ID_AL) ? DEFAULT_BMALIBA :
					 (vendor == PCI_VENDOR_ID_VIA) ? DEFAULT_BMCRBA :
									DEFAULT_BMIBA)|1);
				if (pcibios_write_config_word(bus, fn, PCI_COMMAND, pcicmd|5) || rc)
					goto quit;
			}
		} while (try_again--);
	}

	/*
	 * See if ide port(s) are enabled
	 */
	if ((rc = pcibios_read_config_dword(bus, fn,
		(vendor == PCI_VENDOR_ID_PROMISE) ? 0x50 : 
		(vendor == PCI_VENDOR_ID_ARTOP) ? 0x54 :
		(vendor == PCI_VENDOR_ID_SI) ? 0x48 :
		(vendor == PCI_VENDOR_ID_AL) ? 0x08 :
		0x40, &timings)))
		goto quit;
	/*
	 * We do a vendor check since the Ultra33/66 and AEC6210
	 * holds their timings in a different location.
	 */
#if 0
	printk("ide: timings == %08x\n", timings);
#endif
	/*
	 *  The switch preserves some stuff that was original.
	 */
	switch(vendor) {
		case PCI_VENDOR_ID_INTEL:
			if (!(timings & 0x80008000)) {
				printk("ide: INTEL: neither port is enabled\n");
				goto quit;
			}
			break;
		case PCI_VENDOR_ID_VIA:
			if(!(timings & 0x03)) {
				printk("ide: VIA: neither port is enabled\n");
				goto quit;
			}
			break;
		case PCI_VENDOR_ID_AL:
			timings <<= 16;
			timings >>= 24;
			if (!(timings & 0x30)) {
				printk("ide: ALI15X3: neither port is enabled\n");
				goto quit;
			}
			break;
		case PCI_VENDOR_ID_SI:
			timings <<= 8;
			timings >>= 24;
			if (!(timings & 0x06)) {
				printk("ide: SIS5513: neither port is enabled\n");
				goto quit;
			}
			break;
		case PCI_VENDOR_ID_PROMISE:
			printk("    (U)DMA Burst Bit %sABLED " \
				"Primary %s Mode " \
				"Secondary %s Mode.\n",
				(inb(bmiba + 0x001f) & 1) ? "EN" : "DIS",
				(inb(bmiba + 0x001a) & 1) ? "MASTER" : "PCI",
				(inb(bmiba + 0x001b) & 1) ? "MASTER" : "PCI" );
#if 0
			if (!(inb(bmiba + 0x001f) & 1)) {
				outb(inb(bmiba + 0x001f)|0x01, (bmiba + 0x001f));
				printk("    (U)DMA Burst Bit Forced %sABLED.\n",
					(inb(bmiba + 0x001f) & 1) ? "EN" : "DIS");
			}
#endif
			break;
		case PCI_VENDOR_ID_ARTOP:
		case PCI_VENDOR_ID_TTI:
                default:
                        break;
        }

	/*
	 * Save the dma_base port addr for each interface
	 */
	for (h = 0; h < MAX_HWIFS; ++h) {
		ide_hwif_t *hwif = &ide_hwifs[h];
		byte channel = ((h == 1) || (h == 3) || (h == 5)) ? 1 : 0;

		/*
		 *  This prevents the first contoller from accidentally
		 *  initalizing the hwif's that it does not use and block
		 *  an off-board ide-pci from getting in the game.
		 */
		if ((step_count >= 2) || (pass_count >= 2)) {
			goto quit;
		}

#if 0
		if (hwif->chipset == ide_unknown)
			printk("ide: index == %d channel(%d)\n", h, channel);
#endif

#ifdef CONFIG_BLK_DEV_OFFBOARD
		/*
		 *  This is a forced override for the onboard ide controller
		 *  to be enabled, if one chooses to have an offboard ide-pci
		 *  card as the primary booting device.  This beasty is
		 *  for offboard UDMA upgrades with hard disks, but saving
		 *  the onboard DMA2 controllers for CDROMS, TAPES, ZIPS, etc...
		 */
		if (((vendor == PCI_VENDOR_ID_INTEL) ||
		     (vendor == PCI_VENDOR_ID_SI) ||
		     (vendor == PCI_VENDOR_ID_VIA) ||
		     (vendor == PCI_VENDOR_ID_AL)) && (h >= 2)) {
			hwif->io_base	= channel ? 0x170 : 0x1f0;
			hwif->ctl_port	= channel ? 0x376 : 0x3f6;
			hwif->irq	= channel ? 15 : 14;
			hwif->noprobe	= 0;
		}
#endif /* CONFIG_BLK_DEV_OFFBOARD */
		/*
		 *  If the chipset is listed as "ide_unknown", lets get a
		 *  hwif while they last.  This does the first check on
		 *  the current availability of the ide_hwifs[h] in question.
		 */
		if (hwif->chipset != ide_unknown) {
			continue;
		} else if (vendor == PCI_VENDOR_ID_INTEL) {
			unsigned short time;
#ifdef DISPLAY_TRITON_TIMINGS
			byte s_clks, r_clks;
			unsigned short devid;
#endif /* DISPLAY_TRITON_TIMINGS */
			pass_count++;
			if (hwif->io_base == 0x1f0) {
				time = timings & 0xffff;
				if ((time & 0x8000) == 0)	/* interface enabled? */
					continue;
				hwif->chipset = ide_triton;
				if (dma_enabled)
					init_triton_dma(hwif, bmiba);
				step_count++;
			} else if (hwif->io_base == 0x170) {
				time = timings >> 16;
				if ((time & 0x8000) == 0)	/* interface enabled? */
					continue;
				hwif->chipset = ide_triton;
				if (dma_enabled)
					init_triton_dma(hwif, bmiba + 8);
				step_count++;
			} else {
				continue;
			}
#ifdef DISPLAY_TRITON_TIMINGS
			s_clks = ((~time >> 12) & 3) + 2;
			r_clks = ((~time >>  8) & 3) + 1;
			printk("    %s timing: (0x%04x) sample_CLKs=%d, recovery_CLKs=%d\n",
				hwif->name, time, s_clks, r_clks);
			if ((time & 0x40) && !pcibios_read_config_word(bus, fn, PCI_DEVICE_ID, &devid)
				&& devid == PCI_DEVICE_ID_INTEL_82371SB_1) {
				byte stime;
				if (pcibios_read_config_byte(bus, fn, 0x44, &stime)) {
					if (hwif->io_base == 0x1f0) {
						s_clks = ~stime >> 6;
						r_clks = ~stime >> 4;
					} else {
						s_clks = ~stime >> 2;
						r_clks = ~stime;
					}
					s_clks = (s_clks & 3) + 2;
					r_clks = (r_clks & 3) + 1;
					printk("                   slave: sample_CLKs=%d, recovery_CLKs=%d\n",
						s_clks, r_clks);
				}
			}
			print_triton_drive_flags (0, time & 0xf);
			print_triton_drive_flags (1, (time >> 4) & 0xf);
#endif /* DISPLAY_TRITON_TIMINGS */
		} else if (vendor == PCI_VENDOR_ID_SI) {
			pass_count++;
			if (hwif->io_base == 0x1f0) {
				if ((timings & 0x02) == 0)
					continue;
				hwif->chipset = ide_triton;
				if (dma_enabled)
					init_triton_dma(hwif, bmiba);
				step_count++;
			} else if (hwif->io_base == 0x170) {
				if ((timings & 0x04) == 0)
					continue;
				hwif->chipset = ide_triton;
				if (dma_enabled)
					init_triton_dma(hwif, bmiba + 8);
				step_count++;
			} else {
				continue;
			}
		} else if (vendor == PCI_VENDOR_ID_VIA) {
			pass_count++;
			if (hwif->io_base == 0x1f0) {
				if ((timings & 0x02) == 0)
					continue;
				hwif->chipset = ide_triton;
				if (dma_enabled)
					init_triton_dma(hwif, bmiba);
				if (set_via_timings(bus, fn, 0xc0, 0xa0))
					goto quit;
#ifdef DISPLAY_APOLLO_TIMINGS
				proc_register_dynamic(&proc_root, &via_proc_entry);
#endif /* DISPLAY_APOLLO_TIMINGS */
				step_count++;
			} else if (hwif->io_base == 0x170) {
				if ((timings & 0x01) == 0)
					continue;
				hwif->chipset = ide_triton;
				if (dma_enabled)
					init_triton_dma(hwif, bmiba + 8);
				if (set_via_timings(bus, fn, 0x30, 0x50))
					goto quit;
				step_count++;
			} else {
				continue;
			}
		} else if (vendor == PCI_VENDOR_ID_AL) {
			byte ideic, inmir;
			byte irq_routing_table[] = { -1,  9, 3, 10, 4,  5, 7,  6,
						      1, 11, 0, 12, 0, 14, 0, 15 };

			if (bridgeset) {
				pcibios_read_config_byte(bridgebus, bridgefn, 0x58, &ideic);
				ideic = ideic & 0x03;
				if ((channel && ideic == 0x03) || (!channel && !ideic)) {
					pcibios_read_config_byte(bridgebus, bridgefn, 0x44, &inmir);
					inmir = inmir & 0x0f;
					hwif->irq = irq_routing_table[inmir];
				} else if (channel && !(ideic & 0x01)) {
					pcibios_read_config_byte(bridgebus, bridgefn, 0x75, &inmir);
					inmir = inmir & 0x0f;
					hwif->irq = irq_routing_table[inmir];
				}
			}
			pass_count++;
			if (hwif->io_base == 0x1f0) {
				if ((timings & 0x20) == 0)
					continue;
				hwif->chipset = ide_triton;
				if (dma_enabled)
					init_triton_dma(hwif, bmiba);
				outb(inb(bmiba+2) & 0x60, bmiba+2);
				if (inb(bmiba+2) & 0x80)
					printk("ALI15X3: simplex device: DMA forced\n");
#ifdef DISPLAY_ALI15X3_TIMINGS
				proc_register_dynamic(&proc_root, &ali_proc_entry);
#endif /* DISPLAY_ALI15X3_TIMINGS */
				step_count++;
			} else if (hwif->io_base == 0x170) {
				if ((timings & 0x10) == 0)
					continue;
				hwif->chipset = ide_triton;
				if (dma_enabled)
					init_triton_dma(hwif, bmiba + 8);
				outb(inb(bmiba+10) & 0x60, bmiba+10);
				if (inb(bmiba+10) & 0x80)
					printk("ALI15X3: simplex device: DMA forced\n");
				step_count++;
			} else {
				continue;
			}
		} else if ((vendor == PCI_VENDOR_ID_PROMISE) ||
			   (vendor == PCI_VENDOR_ID_ARTOP) ||
			   (vendor == PCI_VENDOR_ID_TTI)) {
			pass_count++;
			if (vendor == PCI_VENDOR_ID_TTI) {
				if ((!hpt34x_flag) && (h < 2)) {
					goto quit;
				} else if (hpt34x_flag) {
					hwif->io_base	= channel ? (bmiba + 0x28) : (bmiba + 0x20);
					hwif->ctl_port	= channel ? (bmiba + 0x3e) : (bmiba + 0x36);
				} else {
					goto io_temps;
				}
			} else {
io_temps:
				tmp		= channel ? 2 : 0;
				hwif->io_base	= io[tmp];
				hwif->ctl_port	= io[tmp + 1] + 2;
			}
			hwif->irq = irq;
			hwif->noprobe = 0;

			if (device == PCI_DEVICE_ID_ARTOP_ATP850UF) {
				hwif->serialized = 1;
			}

			if ((vendor == PCI_VENDOR_ID_PROMISE) ||
			    (vendor == PCI_VENDOR_ID_TTI)) {
				set_promise_hpt343_extra(device, bmiba);
			}

			if (dma_enabled) {
				if ((!check_region(bmiba, 8)) && (!channel)) {
					hwif->chipset = ((vendor == PCI_VENDOR_ID_TTI) && !hpt34x_flag) ? ide_hpt343 :
							 (device == PCI_DEVICE_ID_PROMISE_20262) ? ide_ultra66 : ide_udma;
					init_triton_dma(hwif, bmiba);
					step_count++;
				} else if ((!check_region((bmiba + 0x08), 8)) && (channel)) {
					hwif->chipset = ((vendor == PCI_VENDOR_ID_TTI) && !hpt34x_flag) ? ide_hpt343 :
							 (device == PCI_DEVICE_ID_PROMISE_20262) ? ide_ultra66 : ide_udma;
					init_triton_dma(hwif, bmiba + 8);
					step_count++;
				} else {
					continue;
				}
			}
		}
	}

	quit: if (rc) printk("ide: pcibios access failed - %s\n", pcibios_strerror(rc));
}
