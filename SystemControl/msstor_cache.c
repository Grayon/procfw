/*
 * This file is part of PRO CFW.

 * PRO CFW is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * PRO CFW is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with PRO CFW. If not, see <http://www.gnu.org/licenses/ .
 */

#include <pspsdk.h>
#include <pspsysmem_kernel.h>
#include <pspkernel.h>
#include <psputilsforkernel.h>
#include <pspsysevent.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <string.h>
#include "printk.h"
#include "utils.h"
#include "main.h"
#include "systemctrl_patch_offset.h"

#define CACHE_BUFSIZE (16 * 1024)
#define CACHE_BUFSIZE_GO (8 * 1024)

static int (*msstor_read)(PspIoDrvFileArg *arg, char *data, int len) = NULL;
static int (*msstor_write)(PspIoDrvFileArg *arg, const char *data, int len) = NULL;
static SceOff (*msstor_lseek)(PspIoDrvFileArg *arg, SceOff ofs, int whence) = NULL;

static u32 read_call = 0;
static u32 read_hit = 0;
static u32 read_missed = 0;
static u32 read_uncacheable = 0;

struct MsCache {
	char *buf;
	int bufsize;
	SceOff pos; /* -1 = invalid */
	int age;
};

static struct MsCache g_cache;

static inline int is_within_range(int pos, int start, int len)
{
	if(pos >= start && pos < start + len) {
		return 1;
	}

	return 0;
}

static struct MsCache *get_hit_cache(SceOff pos, int len)
{
	if(g_cache.pos != -1) {
		if(is_within_range(pos, g_cache.pos, g_cache.bufsize) && is_within_range(pos+len, g_cache.pos, g_cache.bufsize)) {
			return &g_cache;
		}
	}

	return NULL;
}

static void disable_cache(struct MsCache *cache)
{
	cache->pos = -1;
}

static void disable_cache_within_range(SceOff pos, int len)
{
	if(g_cache.pos != -1) {
		if(is_within_range(pos, g_cache.pos, g_cache.bufsize)) {
			disable_cache(&g_cache);
		}

		if(is_within_range(pos+len, g_cache.pos, g_cache.bufsize)) {
			disable_cache(&g_cache);
		}

		if(pos <= g_cache.pos && pos + len >= g_cache.pos + g_cache.bufsize) {
			disable_cache(&g_cache);
		}
	}
}

static int msstor_cache_read(PspIoDrvFileArg *arg, char *data, int len)
{
	int ret, read_len;
	SceOff pos;
	struct MsCache *cache;
	
	pos = (*msstor_lseek)(arg, 0, PSP_SEEK_CUR);
	cache = get_hit_cache(pos, len);
	
	// cache hit?
	if(cache != NULL) {
		read_len = MIN(len, cache->pos + cache->bufsize - pos);
		memcpy(data, cache->buf + pos - cache->pos, read_len);
		ret = read_len;
		(*msstor_lseek)(arg, pos + ret, PSP_SEEK_SET);
		read_hit += len;
	} else {
		if( 1 ) {
			char buf[256];

			sprintf(buf, "%s: 0x%08X <%d>\n", __func__, (uint)pos, (int)len);
			sceIoWrite(1, buf, strlen(buf));
		}
		
		cache = &g_cache;

		if(len <= cache->bufsize) {
			disable_cache(cache);
			ret = (*msstor_read)(arg, cache->buf, cache->bufsize);

			if(ret >= 0) {
				read_len = MIN(len, ret);
				memcpy(data, cache->buf, read_len);
				ret = read_len;
				cache->pos = pos;
				(*msstor_lseek)(arg, pos + ret, PSP_SEEK_SET);
			} else {
				printk("%s: read -> 0x%08X\n", __func__, ret);
			}

			read_missed += len;
		} else {
			ret = (*msstor_read)(arg, data, len);
//			printk("%s: read len %d too large\n", __func__, len);
			read_uncacheable += len;
		}
	}

	read_call += len;

	return ret;
}

static int msstor_cache_write(PspIoDrvFileArg *arg, const char *data, int len)
{
	int ret;
	SceOff pos;

	pos = (*msstor_lseek)(arg, 0, PSP_SEEK_CUR);
	disable_cache_within_range(pos, len);
	ret = (*msstor_write)(arg, data, len);

	return ret;
}

int msstor_init(void)
{
	PspIoDrvFuncs *funcs;
	PspIoDrv *pdrv;
	SceUID memid;
	int bufsize;

	if(psp_model == PSP_GO) {
		bufsize = CACHE_BUFSIZE_GO;
	} else {
		bufsize = CACHE_BUFSIZE;
	}

	if((bufsize % 0x200) != 0) {
		printk("%s: alignment error\n", __func__);

		return -1;
	}

	memid = sceKernelAllocPartitionMemory(1, "MsStorCache", PSP_SMEM_High, bufsize + 64, NULL);

	if(memid < 0) {
		printk("%s: sctrlKernelAllocPartitionMemory -> 0x%08X\n", __func__, memid);
		return -2;
	}

	g_cache.buf = sceKernelGetBlockHeadAddr(memid);

	if(g_cache.buf == NULL) {
		return -3;
	}

	g_cache.buf = (void*)(((u32)g_cache.buf & (~(64-1))) + 64);
	g_cache.bufsize = bufsize;
	g_cache.pos = -1;

	if(psp_model == PSP_GO && sctrlKernelBootFrom() == 0x50) {
		pdrv = sctrlHENFindDriver("eflash0a0f1p");
	} else {
		pdrv = sctrlHENFindDriver("msstor0p");
	}

	if(pdrv == NULL) {
		return -4;
	}

	funcs = pdrv->funcs;
	msstor_read = funcs->IoRead;
	msstor_write = funcs->IoWrite;
	msstor_lseek = funcs->IoLseek;
	funcs->IoRead = msstor_cache_read;
	funcs->IoWrite = msstor_cache_write;

	return 0;
}

// call @SystemControl:SystemCtrlPrivate,0xD3014719@
void msstor_stat(int reset)
{
	char buf[256];

	if(read_call != 0) {
		sprintf(buf, "Mstor cache size: %dKB\n", g_cache.bufsize / 1024);
		sceIoWrite(1, buf, strlen(buf));
		sprintf(buf, "hit percent: %02d%%/%02d%%/%02d%%, [%d/%d/%d/%d]\n", 
				(int)(100 * read_hit / read_call), 
				(int)(100 * read_missed / read_call), 
				(int)(100 * read_uncacheable / read_call), 
				(int)read_hit, (int)read_missed, (int)read_uncacheable, (int)read_call);
		sceIoWrite(1, buf, strlen(buf));
		sprintf(buf, "caches stat:\n");
		sceIoWrite(1, buf, strlen(buf));

		if(1) {
			sprintf(buf, "Cache Pos: 0x%08X\n", (uint)g_cache.pos);
			sceIoWrite(1, buf, strlen(buf));
		}
	} else {
		sprintf(buf, "no msstor cache call yet\n");
		sceIoWrite(1, buf, strlen(buf));
	}

	if(reset) {
		read_call = read_hit = read_missed = read_uncacheable = 0;
	}
}
