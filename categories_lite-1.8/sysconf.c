/*
 *  this file is part of Game Categories Lite
 *  Contain parts of 6.39 TN-A, XmbControl
 *
 *  Copyright (C) 2009, Bubbletune
 *  Copyright (C) 2011, Total_Noob
 *  Copyright (C) 2011, Codestation
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <psputils.h>
#include <pspkernel.h>
#include "categories_lite.h"
#include "psppaf.h"
#include "vshitem.h"
#include "utils.h"
#include "context.h"
#include "config.h"
#include "stub_funcs.h"
#include "language.h"
#include "logger.h"

#define OPTION_PAGE "page_psp_config_umd_cache"

char user_buffer[256];
static u32 backup[4] = { 0, 0, 0, 0 };
int context_mode = 0;
static int last_sysconf_mode = 0;
static SceSysconfItem *sysconf_item[] = { NULL, NULL, NULL, NULL };
static const char *last_sysconf_key = NULL;
static int pending_sysconf_mode = 0;
static unsigned long long pending_sysconf_time = 0;
int sysconf_hint_mode = 0;
unsigned long long sysconf_hint_time = 0;
static const char *fallback_show_opts[] = {
    "No",
    "Only Memory Stick",
    "Only Internal Storage",
    "Both"
};

extern int sysconf_plug;
extern int model;

static void ensureLangLoadedForMode(int mode) {
    int need = 0;
    switch (mode) {
    case 1:
        if (!lang_container.mode[0] || !lang_container.mode[1] || !lang_container.mode[2] ||
            !lang_container.mode[0][0] || !lang_container.mode[1][0] || !lang_container.mode[2][0]) need = 1;
        break;
    case 2:
        if (!lang_container.prefix[0] || !lang_container.prefix[1] ||
            !lang_container.prefix[0][0] || !lang_container.prefix[1][0]) need = 1;
        break;
    case 3:
        if (!lang_container.show[0] || !lang_container.show[1] ||
            !lang_container.show[2] || !lang_container.show[3] ||
            !lang_container.show[0][0] || !lang_container.show[1][0] ||
            !lang_container.show[2][0] || !lang_container.show[3][0]) need = 1;
        break;
    case 4:
        if (!lang_container.sort[0] || !lang_container.sort[1] ||
            !lang_container.sort[0][0] || !lang_container.sort[1][0]) need = 1;
        break;
    default:
        break;
    }
    if (need) {
        int lid = get_registry_value("/CONFIG/SYSTEM/XMB", "language");
        LoadLanguage(lid, model == 4 ? INTERNAL_STORAGE : MEMORY_STICK);
    }
}

#define GC_SYSCONF_MODE "gc0"
#define GC_SYSCONF_MODE_SUB "gcs0"
#define GC_SYSCONF_PREFIX "gc1"
#define GC_SYSCONF_PREFIX_SUB "gcs1"
#define GC_SYSCONF_SHOW "gc2"
#define GC_SYSCONF_SHOW_SUB "gcs2"
#define GC_SYSCONF_SORT "gc3"
#define GC_SYSCONF_SORT_SUB "gcs3"

static const char *sysconf_str[] = {
    GC_SYSCONF_MODE,
    GC_SYSCONF_PREFIX,
    GC_SYSCONF_SHOW,
    GC_SYSCONF_SORT
};

static const char *sysconf_sub[] = {
    GC_SYSCONF_MODE_SUB,
    GC_SYSCONF_PREFIX_SUB,
    GC_SYSCONF_SHOW_SUB,
    GC_SYSCONF_SORT_SUB
};
static const char sysconf_page_mode[] = OPTION_PAGE;
static const char sysconf_page_prefix[] = OPTION_PAGE;
static const char sysconf_page_show[] = OPTION_PAGE;
static const char sysconf_page_sort[] = OPTION_PAGE;
static const char *sysconf_page[] = {
    sysconf_page_mode,
    sysconf_page_prefix,
    sysconf_page_show,
    sysconf_page_sort
};
static const unsigned long long pending_window_us = 200000ULL;
static const unsigned long long hint_window_us = 300000ULL;

void (*AddSysconfItem)(u32 *option, SceSysconfItem **item);
SceSysconfItem *(*GetSysconfItem)(void *arg0, void *arg1);

int (*vshGetRegistryValue)(u32 *option, char *name, void *arg2, int size, int *value);
int (*vshSetRegistryValue)(u32 *option, char *name, int size,  int *value);

int (*ResolveRefWString)(void *resource, u32 *data, int *a2, char **string, int *t0);
int (*GetPageNodeByID)(void *resource, char *name, SceRcoEntry **child);

void AddSysconfItemPatched(u32 *option, SceSysconfItem **item) {
    AddSysconfItem(option, item);
    if(sysconf_plug) {
        for(u32 i = 0; i < ITEMSOF(sysconf_item); i++) {
            if(sysconf_plug || !sysconf_item[i]) {
                sysconf_item[i] = (SceSysconfItem *)sce_paf_private_malloc(sizeof(SceSysconfItem));
            }
            sce_paf_private_memcpy(sysconf_item[i], *item, sizeof(SceSysconfItem));
            sysconf_item[i]->id = 6;
            sysconf_item[i]->text = sysconf_str[i];
            sysconf_item[i]->regkey = sysconf_str[i];
            sysconf_item[i]->subtitle = sysconf_sub[i];
            sysconf_item[i]->page = sysconf_page[i];
            option[2] = 1;
            AddSysconfItem(option, &sysconf_item[i]);
        }
    }
    sysconf_plug = 0;
    context_gamecats = 0;
    kprintf("called, option addr: %08X\n", option);
}

void HijackContext(SceRcoEntry *src, char **options, int n) {
    SceRcoEntry *plane = (SceRcoEntry *)((u32)src + src->first_child);
    SceRcoEntry *mlist = (SceRcoEntry *)((u32)plane + plane->first_child);
    u32 *mlist_param = (u32 *)((u32)mlist + mlist->param);
    kprintf("Called, n: %i, context_mode: %i\n", n, context_mode);
    /* Backup */
    if(backup[0] == 0 && backup[1] == 0 && backup[2] == 0 && backup[3] == 0) {
        kprintf("Making context backup\n");
        backup[0] = mlist->first_child;
        backup[1] = mlist->child_count;
        backup[2] = mlist_param[16];
        backup[3] = mlist_param[18];
    }

    if(context_mode) {
        SceRcoEntry *base = (SceRcoEntry *)((u32)mlist + mlist->first_child);

        SceRcoEntry *item = (SceRcoEntry *)sce_paf_private_malloc(base->next_entry * n);
        u32 *item_param = (u32 *)((u32)item + base->param);

        mlist->first_child = (u32)item - (u32)mlist;
        mlist->child_count = n;
        mlist_param[16] = 13;
        mlist_param[18] = 6;

        for(int i = 0; i < n; i++) {
            sce_paf_private_memcpy(item, base, base->next_entry);

            item_param[0] = 0xDEAD;
            item_param[1] = (u32)options[i];

            if(i != 0) {
                item->prev_entry = item->next_entry;
            }
            if(i == n - 1) {
                item->next_entry = 0;
            }

            item = (SceRcoEntry *)((u32)item + base->next_entry);
            item_param = (u32 *)((u32)item + base->param);
        }
    } else {
        /* Restore */
        mlist->first_child = backup[0];
        mlist->child_count = backup[1];
        mlist_param[16] = backup[2];
        mlist_param[18] = backup[3];
    }

    sceKernelDcacheWritebackAll();
}

SceSysconfItem *GetSysconfItemPatched(void *arg0, void *arg1) {
    SceSysconfItem *item = GetSysconfItem(arg0, arg1);
    kprintf("sysconf:GetSysconfItem item->text=%s regkey=%s id=%i\n",
            item->text ? item->text : "(null)",
            item->regkey ? item->regkey : "(null)",
            item->id);
    int next_mode = context_mode;
    const char *key = item->regkey ? item->regkey : item->text;
    for(u32 i = 0; i < ITEMSOF(sysconf_str); i++) {
        if(sce_paf_private_strcmp(key, sysconf_str[i]) == 0 ||
           (item->text && sce_paf_private_strcmp(item->text, sysconf_str[i]) == 0)) {
            next_mode = i + 1;
            last_sysconf_key = sysconf_str[i];
            kprintf("sysconf:GetSysconfItem match %s -> mode=%i\n", sysconf_str[i], next_mode);
        }
    }
    context_mode = next_mode;
    if (context_mode > 0) last_sysconf_mode = context_mode;
    kprintf("sysconf:GetSysconfItem final mode=%i last=%i key=%s\n",
            context_mode, last_sysconf_mode, last_sysconf_key ? last_sysconf_key : "(null)");
    return item;
}

int vshGetRegistryValuePatched(u32 *option, char *name, void *arg2, int size, int *value) {
    if (name) {
        kprintf("sysconf:GetReg name=%s\n", name);
        if (strcmp(name, "/CONFIG/SYSTEM/XMB/language") == 0) {
            lang_id = get_registry_value("/CONFIG/SYSTEM/XMB", "language");
            LoadLanguage(lang_id, model == 4 ? INTERNAL_STORAGE : MEMORY_STICK);
        }
        for(u32 i = 0; i < ITEMSOF(sysconf_str); i++) {
            if(sce_paf_private_strcmp(name, sysconf_str[i]) == 0) {
                context_mode = i + 1;
                kprintf("sysconf:GetReg match %s -> mode=%i\n", sysconf_str[i], context_mode);
                if (context_mode > 0) last_sysconf_mode = context_mode;
                last_sysconf_key = sysconf_str[i];
                {
                    unsigned long long now = sceKernelGetSystemTimeWide();
                    pending_sysconf_mode = context_mode;
                    pending_sysconf_time = now;
                }
                switch(i) {
                case 0:
                    *value = config.mode;
                    return 0;
                case 1:
                    *value = config.prefix;
                    return 0;
                case 2:
                    *value = config.uncategorized;
                    return 0;
                case 3:
                    *value = config.catsort;
                    return 0;
                default:
                    *value = 0;
                    return 0;
                }
            }
        }
    }
    return vshGetRegistryValue(option, name, arg2, size, value);
}

int vshSetRegistryValuePatched(u32 *option, char *name, int size,  int *value) {
    u32 *cfg;
    if (name) {
        kprintf("name: %s\n", name);
        for(u32 i = 0; i < ITEMSOF(sysconf_str); i++) {
            if(sce_paf_private_strcmp(name, sysconf_str[i]) == 0) {
                switch(i) {
                case 0:
                    cfg = &config.mode;
                    break;
                case 1:
                    cfg = &config.prefix;
                    break;
                case 2:
                    cfg = &config.uncategorized;
                    break;
                case 3:
                    cfg = &config.catsort;
                    break;
                default:
                    cfg = NULL;
                    break;
                }
                if(cfg) {
                    *cfg = *value;
                    save_config();
                    return 0;
                }
            }
        }
    }
    return vshSetRegistryValue(option, name, size, value);
}

int ResolveRefWStringPatched(void *resource, u32 *data, int *a2, char **string, int *t0) {
    kprintf("Processing\n");
    if (data[0] == 0xDEAD) {
        kprintf("data: %s\n", (char *)data[1]);
        gc_utf8_to_unicode((wchar_t *)user_buffer, (char *) data[1]);
        *(wchar_t **) string = (wchar_t *) user_buffer;
        return 0;
    }
    return ResolveRefWString(resource, data, a2, string, t0);
}

int GetPageNodeByIDPatched(void *resource, char *name, SceRcoEntry **child) {
    int forced_mode = 0;
    if (name) {
        for (u32 i = 0; i < ITEMSOF(sysconf_page); i++) {
            if (name == sysconf_page[i]) {
                forced_mode = i + 1;
                break;
            }
        }
    }
    int res = GetPageNodeByID(resource, name, child);
    if(name) {
        //kprintf("name: %s, mode: %i\n", name, context_mode);
        if (sce_paf_private_strcmp(name, OPTION_PAGE) == 0) {
            int desired_mode = 0;
            if (forced_mode > 0) {
                desired_mode = forced_mode;
            } else {
                unsigned long long now = sceKernelGetSystemTimeWide();
                if (sysconf_hint_mode > 0 && (now - sysconf_hint_time) < hint_window_us) {
                    desired_mode = sysconf_hint_mode;
                    sysconf_hint_mode = 0;
                    sysconf_hint_time = 0;
                }
                if (desired_mode == 0 && pending_sysconf_mode > 0) {
                    if ((now - pending_sysconf_time) < pending_window_us) {
                        desired_mode = pending_sysconf_mode;
                    } else {
                        pending_sysconf_mode = 0;
                    }
                }
                if (desired_mode == 0 && last_sysconf_key) {
                    for (u32 i = 0; i < ITEMSOF(sysconf_str); i++) {
                        if (sce_paf_private_strcmp(last_sysconf_key, sysconf_str[i]) == 0) {
                            desired_mode = i + 1;
                            break;
                        }
                    }
                }
                if (desired_mode == 0 && last_sysconf_mode > 0) {
                    desired_mode = last_sysconf_mode;
                }
            }
            if (desired_mode > 0) {
                context_mode = desired_mode;
                last_sysconf_mode = desired_mode;
                last_sysconf_key = sysconf_str[desired_mode - 1];
            }
            ensureLangLoadedForMode(context_mode);
            kprintf("sysconf:GetPage name=%s mode=%i last=%i key=%s\n",
                    name ? name : "(null)", context_mode, last_sysconf_mode,
                    last_sysconf_key ? last_sysconf_key : "(null)");
            kprintf("sysconf:GetPage show=[%s|%s|%s|%s]\n",
                    (lang_container.show[0] ? lang_container.show[0] : "(null)"),
                    (lang_container.show[1] ? lang_container.show[1] : "(null)"),
                    (lang_container.show[2] ? lang_container.show[2] : "(null)"),
                    (lang_container.show[3] ? lang_container.show[3] : "(null)"));
            switch(context_mode) {
            case 0:
                HijackContext(*child, NULL, 0);
                break;
            case 1:
                HijackContext(*child, lang_container.mode, ITEMSOF(lang_container.mode));
                break;
            case 2:
                HijackContext(*child, lang_container.prefix, ITEMSOF(lang_container.prefix));
                break;
            case 3:
                {
                    const char *opts[4];
                    for (int i = 0; i < 4; ++i) {
                        const char *s = lang_container.show[i];
                        opts[i] = (s && s[0]) ? s : fallback_show_opts[i];
                    }
                    HijackContext(*child, (char **)opts, 4);
                }
                break;
            case 4:
                HijackContext(*child, lang_container.sort, ITEMSOF(lang_container.sort));
                break;
            }
        }
    }
    return res;
}

void PatchVshmainForSysconf(u32 text_addr) {
    vshGetRegistryValue = redir2stub(text_addr+patches.vshGetRegistryValueOffset[patch_index], get_registry_stub, vshGetRegistryValuePatched);
    vshSetRegistryValue = redir2stub(text_addr+patches.vshSetRegistryValueOffset[patch_index], set_registry_stub, vshSetRegistryValuePatched);
}

void PatchPafForSysconf(u32 text_addr) {
    GetPageNodeByID = redir2stub(text_addr+patches.GetPageNodeByIDOffset[patch_index], get_page_node_stub, GetPageNodeByIDPatched);
    ResolveRefWString = redir2stub(text_addr+patches.ResolveRefWStringOffset[patch_index], resolve_ref_wstring_stub, ResolveRefWStringPatched);
}

void PatchSysconf(u32 text_addr) {
    AddSysconfItem = redir_call(text_addr+patches.AddSysconfItem[patch_index], AddSysconfItemPatched);
    GetSysconfItem = redir_call(text_addr+patches.GetSysconfItem[patch_index], GetSysconfItemPatched);
}
