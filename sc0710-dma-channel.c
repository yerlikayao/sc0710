/*
 *  Driver for the Elgato 4k60 Pro mk.2 HDMI capture card.
 *
 *  Copyright (c) 2021 Steven Toth <stoth@kernellabs.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/* Each FPGA descriptor is 8xDWORD.
 * We're going to have 8 descriptors per channel, where
 * each descriptor is either a frame of video or a 'chunk' (TBD)
 * of audio.
 * The entire descriptor pagetable for a single channel will fit
 * inside a single page of memory.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include "sc0710.h"

void sc0710_dma_channel_descriptors_dump(struct sc0710_dma_channel *ch)
{
	int i;
	struct sc0710_dma_descriptor *desc = (struct sc0710_dma_descriptor *)ch->pt_cpu;

	printk("%s  pt_cpu %p  pt_dma %llx  pt_size %d\n",
		ch->dev->name,
		ch->pt_cpu, ch->pt_dma, ch->pt_size);

	/* Create a set of linked scatter gather descriptors and allocate
	 * supporting dma buffers for the PCIe endpoint to burst into.
	 */
	for (i = 0; i < ch->numDescriptors; i++) {
		printk("%s buf_cpu %p buf_dma %llx buf_size %d\n",
			ch->dev->name,
			ch->buf_cpu[i], ch->buf_dma[i], ch->buf_size);
	}
	for (i = 0; i < ch->numDescriptors; i++) {
		printk("%s         [%02d] %08x %08x %08x %08x %08x %08x %08x %08x\n",
			ch->dev->name,
			i,
			desc->control,
			desc->lengthBytes,
			desc->src_l,
			desc->src_h,
			desc->dst_l,
			desc->dst_h,
			desc->next_l,
			desc->next_h);
		desc++;
	}
}

static void sc0710_dma_channel_descriptors_free(struct sc0710_dma_channel *ch)
{
	int i;

	pci_free_consistent(ch->dev->pci, ch->pt_size, ch->pt_cpu, ch->pt_dma);

	for (i = 0; i < ch->numDescriptors; i++) {
		pci_free_consistent(ch->dev->pci, ch->buf_size, ch->buf_cpu[i], ch->buf_dma[i]);
	}
}

static int sc0710_dma_channel_descriptors_alloc(struct sc0710_dma_channel *ch)
{
	struct sc0710_dma_descriptor *desc;
	int i;

	/* allocate the descriptor table ram, its contigious. */
	ch->pt_cpu = pci_alloc_consistent(ch->dev->pci, ch->pt_size, &ch->pt_dma);
	if (ch->pt_cpu == 0)
		return -1;

	memset(ch->pt_cpu, 0, ch->pt_size);

	desc = (struct sc0710_dma_descriptor *)ch->pt_cpu;

	/* Create a set of linked scatter gather descriptors and allocate
	 * supporting dma buffers for the PCIe endpoint to burst into.
	 */
	for (i = 0; i < ch->numDescriptors; i++) {
		ch->buf_cpu[i] = pci_alloc_consistent(ch->dev->pci, ch->buf_size, &ch->buf_dma[i]);
		if (!ch->buf_cpu[i]) {
			return -1;
		}

		desc->control = 0xAD4B0000;
		desc->lengthBytes = ch->buf_size;
		desc->src_l = 0;
		desc->src_h = 0;
		desc->dst_l = (u64)ch->buf_dma[i];
		desc->dst_h = (u64)ch->buf_dma[i] >> 32;
		desc->next_l = (u64)ch->pt_dma;
		desc->next_h = (u64)ch->pt_dma >> 32;
		desc++;
	}

	return 0;
}

int sc0710_dma_channel_alloc(struct sc0710_dev *dev, u32 nr, enum sc0710_channel_dir_e direction,
	u32 baseaddr,
	enum sc0710_channel_type_e mediatype)
{
	struct sc0710_dma_channel *ch = &dev->channel[nr];
	if (nr >= SC0710_MAX_CHANNELS)
		return -1;

	memset(ch, 0, sizeof(*ch));
	mutex_init(&ch->lock);

	ch->dev = dev;
	ch->nr = nr;
	ch->enabled = 1;
	ch->direction = direction;
	ch->mediatype = mediatype;

	if (ch->mediatype == CHTYPE_VIDEO) {
		ch->numDescriptors = 6;
		ch->buf_size = 0x1fa400;
	} else
	if (ch->mediatype == CHTYPE_AUDIO) {
		ch->numDescriptors = 4;
		ch->buf_size = 0x4000;
	} else {
		ch->numDescriptors = 0;
	}

	/* Page table defaults. */
	ch->pt_size = PAGE_SIZE;

	/* register offsets use by the channel and dma descriptor register writes/reads. */
	ch->register_dma_base = baseaddr;

	ch->register_sg_base = baseaddr + 0x4000;
        ch->reg_sg_start_l = ch->register_sg_base + 0x80;
        ch->reg_sg_start_h = ch->register_sg_base + 0x84;
        ch->reg_sg_adj = ch->register_sg_base + 0x88;

	sc0710_dma_channel_descriptors_alloc(ch);

	printk(KERN_INFO "%s channel %d allocated\n", dev->name, nr);

	sc0710_dma_channel_descriptors_dump(ch);

	return 0; /* Success */
};

void sc0710_dma_channel_free(struct sc0710_dev *dev, u32 nr)
{
	struct sc0710_dma_channel *ch = &dev->channel[nr];
	if (nr >= SC0710_MAX_CHANNELS)
		return;

	if (ch->enabled == 0)
		return;

	ch->enabled = 0;
	sc0710_dma_channel_descriptors_free(ch);

	printk(KERN_INFO "%s channel %d deallocated\n", dev->name, nr);
}