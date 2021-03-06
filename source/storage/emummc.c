/*
 * Copyright (C) 2019 CTCaer
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>
#include <stdlib.h>

#include "emummc.h"
#include "sdmmc.h"
#include "../config/config.h"
#include "../config/ini.h"
#include "../gfx/gfx.h"
#include "../libs/fatfs/ff.h"
#include "../mem/heap.h"
#include "../utils/list.h"
#include "../utils/types.h"

extern sdmmc_t sd_sdmmc;
extern sdmmc_storage_t sd_storage;
extern FATFS sd_fs;

extern hekate_config h_cfg;

extern bool sd_mount();
extern void sd_unmount();

bool emummc_load_cfg()
{
	sd_mount();
	emu_cfg.enabled = 0;
	emu_cfg.path = NULL;
	emu_cfg.nintendo_path = NULL;
	emu_cfg.sector = 0;
	emu_cfg.id = 0;
	emu_cfg.file_based_part_size = 0;
	emu_cfg.active_part = 0;
	emu_cfg.fs_ver = 0;
	free(emu_cfg.emummc_file_based_path);
	emu_cfg.emummc_file_based_path = (char *)malloc(0x80);

	LIST_INIT(ini_sections);
	if (ini_parse(&ini_sections, "emuMMC/emummc.ini", false))
	{
		LIST_FOREACH_ENTRY(ini_sec_t, ini_sec, &ini_sections, link)
		{
			if (ini_sec->type == INI_CHOICE)
			{
				if (strcmp(ini_sec->name, "emummc"))
					continue;

				LIST_FOREACH_ENTRY(ini_kv_t, kv, &ini_sec->kvs, link)
				{
					if (!strcmp("enabled", kv->key))
						emu_cfg.enabled = atoi(kv->val);
					else if (!strcmp("sector", kv->key))
						emu_cfg.sector = strtol(kv->val, NULL, 16);
					else if (!strcmp("id", kv->key))
						emu_cfg.id = strtol(kv->val, NULL, 16);
					else if (!strcmp("path", kv->key))
						emu_cfg.path = kv->val;
					else if (!strcmp("nintendo_path", kv->key))
						emu_cfg.nintendo_path = kv->val;
				}
				break;
			}
		}
		return 0;
	}
	return 1;
}

static int emummc_raw_get_part_off(int part_idx)
{
	switch (part_idx)
	{
	case 0:
		return 2;
	case 1:
		return 0;
	case 2:
		return 1;
	}
	return 2;
}


int emummc_storage_init_mmc(sdmmc_storage_t *storage, sdmmc_t *sdmmc)
{
	FILINFO fno;
	if (!sdmmc_storage_init_mmc(storage, sdmmc, SDMMC_4, SDMMC_BUS_WIDTH_8, 4))
	{
		EPRINTF("Failed to init eMMC.");

		goto out;
	}
	if (h_cfg.emummc_force_disable)
		return 1;

	emu_cfg.active_part = 0;
	if (!sd_mount())
		goto out;

	if (emu_cfg.enabled && !emu_cfg.sector)
	{
		strcpy(emu_cfg.emummc_file_based_path, emu_cfg.path);
		strcat(emu_cfg.emummc_file_based_path, "/eMMC");

		if (f_stat(emu_cfg.emummc_file_based_path, &fno))
		{
			EPRINTF("Failed to open eMMC folder.");
			goto out;
		}
		f_chmod(emu_cfg.emummc_file_based_path, AM_ARC, AM_ARC);

		strcat(emu_cfg.emummc_file_based_path, "/00");
		if (f_stat(emu_cfg.emummc_file_based_path, &fno))
		{
			EPRINTF("Failed to open emuMMC rawnand.");
			goto out;
		}
		emu_cfg.file_based_part_size = fno.fsize >> 9;
	}
	return 1;

out:
	return 0;
}

int emummc_storage_end(sdmmc_storage_t *storage)
{
	sd_unmount();
	sdmmc_storage_end(storage);

	return 1;
}

int emummc_storage_read(sdmmc_storage_t *storage, u32 sector, u32 num_sectors, void *buf)
{
	FIL fp;
	if (!emu_cfg.enabled || h_cfg.emummc_force_disable)
		return sdmmc_storage_read(storage, sector, num_sectors, buf);
	else if (emu_cfg.sector)
	{
		sector += emu_cfg.sector;
		sector += emummc_raw_get_part_off(emu_cfg.active_part) * 0x2000;
		return sdmmc_storage_read(&sd_storage, sector, num_sectors, buf);
	}
	else
	{
		if (!emu_cfg.active_part)
		{
			u32 file_part = sector / emu_cfg.file_based_part_size;
			sector = sector % emu_cfg.file_based_part_size;
			if (file_part >= 10)
				itoa(file_part, emu_cfg.emummc_file_based_path + strlen(emu_cfg.emummc_file_based_path) - 2, 10);
			else
			{
				emu_cfg.emummc_file_based_path[strlen(emu_cfg.emummc_file_based_path) - 2] = '0';
				itoa(file_part, emu_cfg.emummc_file_based_path + strlen(emu_cfg.emummc_file_based_path) - 1, 10);
			}
		}
		if (f_open(&fp, emu_cfg.emummc_file_based_path, FA_READ))
		{
			EPRINTF("Failed to open emuMMC image.");
			return 0;
		}
		f_lseek(&fp, (u64)sector << 9);
		if (f_read(&fp, buf, (u64)num_sectors << 9, NULL))
		{
			EPRINTF("Failed to read emuMMC image.");
			f_close(&fp);
			return 0;
		}

		f_close(&fp);
		return 1;
	}

	return 1;
}

int emummc_storage_write(sdmmc_storage_t *storage, u32 sector, u32 num_sectors, void *buf)
{
	FIL fp;
	if (!emu_cfg.enabled || h_cfg.emummc_force_disable)
		return sdmmc_storage_write(storage, sector, num_sectors, buf);
	else if (emu_cfg.sector)
	{
		sector += emu_cfg.sector;
		sector += emummc_raw_get_part_off(emu_cfg.active_part) * 0x2000;
		return sdmmc_storage_write(&sd_storage, sector, num_sectors, buf);
	}
	else
	{
		if (!emu_cfg.active_part)
		{
			u32 file_part = sector / emu_cfg.file_based_part_size;
			sector = sector % emu_cfg.file_based_part_size;
			if (file_part >= 10)
				itoa(file_part, emu_cfg.emummc_file_based_path + strlen(emu_cfg.emummc_file_based_path) - 2, 10);
			else
			{
				emu_cfg.emummc_file_based_path[strlen(emu_cfg.emummc_file_based_path) - 2] = '0';
				itoa(file_part, emu_cfg.emummc_file_based_path + strlen(emu_cfg.emummc_file_based_path) - 1, 10);
			}
		}
		if (f_open(&fp, emu_cfg.emummc_file_based_path, FA_WRITE))
		{
			gfx_printf("e5\n");
			return 0;
		}
		f_lseek(&fp, (u64)sector << 9);
		if (f_write(&fp, buf, (u64)num_sectors << 9, NULL))
		{
			gfx_printf("e6\n");
			f_close(&fp);
			return 0;
		}

		f_close(&fp);
		return 1;
	}
}

int emummc_storage_set_mmc_partition(sdmmc_storage_t *storage, u32 partition)
{
	emu_cfg.active_part = partition;

	if (!emu_cfg.enabled || h_cfg.emummc_force_disable)
		sdmmc_storage_set_mmc_partition(storage, partition);
	else if (emu_cfg.sector)
		return 1;
	else
	{
		strcpy(emu_cfg.emummc_file_based_path, emu_cfg.path);
		strcat(emu_cfg.emummc_file_based_path, "/eMMC");

		switch (partition)
		{
		case 0:
			strcat(emu_cfg.emummc_file_based_path, "/00");
			break;
		case 1:
			strcat(emu_cfg.emummc_file_based_path, "/BOOT0");
			break;
		case 2:
			strcat(emu_cfg.emummc_file_based_path, "/BOOT1");
			break;
		}

		return 1;
	}

	return 1;
}
