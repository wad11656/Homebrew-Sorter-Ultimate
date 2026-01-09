/*
 *  this file is part of Game Categories Lite
 *
 *  Copyright (C) 2011  Codestation
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

#include <pspiofilemgr.h>
#include "psppaf.h"
#include "utils.h"
#include "config.h"
#include "logger.h"

extern int global_pos;

static char *filter_data[2] = { NULL, NULL };
static SceSize filter_size[2];
static int counter[2];

static int count_filters(char *buf, SceSize size) {
    int count = 0;
    for(u32 i = 0; i < size; i++) {
        if(buf[i] == '\r' || buf[i] == '\n') {
            buf[i] = 0;
        }
    }
    for(u32 i = 0; i < size; i++) {
        if(buf[i] && (i == 0 || !buf[i-1])) {
            count++;
        }
    }
    return count;
}

static int names_match(const char *filter, const char *name) {
    if(sce_paf_private_strcmp(filter, name) == 0) return 1;
    // normalize CAT_ and/or NN prefixes for both strings
    const char *a = filter;
    const char *b = name;
    if(config.prefix) {
        if(sce_paf_private_strncmp(a, "CAT_", 4) == 0) a += 4;
        if(sce_paf_private_strncmp(b, "CAT_", 4) == 0) b += 4;
    }
    if(config.catsort) {
        if(a[0] >= '0' && a[0] <= '9' && a[1] >= '0' && a[1] <= '9') a += 2;
        if(b[0] >= '0' && b[0] <= '9' && b[1] >= '0' && b[1] <= '9') b += 2;
    }
    return sce_paf_private_strcmp(a, b) == 0;
}

int check_filter_for(const char *str, int location) {
    if(location < 0 || location > 1) return 0;
    int c = counter[location];
    char *buf = filter_data[location];

    if(buf == NULL) return 0;
    kprintf("checking <<%s>> in filter\n", str);
    while(c) {
        while(!*buf) buf++;
        kprintf("veryfing %s\n", buf);
        if(names_match(buf, str)) {
            kprintf("match for [%s]\n", str);
            return 1;
        }
        buf += sce_paf_private_strlen(buf);
        c--;
    }
    return 0;
}

void unload_filter() {
    for(int i = 0; i < 2; i++) {
        if(filter_data[i] != NULL) {
            sce_paf_private_free(filter_data[i]);
            filter_data[i] = NULL;
            counter[i] = 0;
            filter_size[i] = 0;
        }
    }
}

static int load_filter_for(int location) {
    SceUID fd;

    if(location < 0 || location > 1) return -1;
    if(filter_data[location] != NULL) {
        sce_paf_private_free(filter_data[location]);
        filter_data[location] = NULL;
        counter[location] = 0;
        filter_size[location] = 0;
    }

    kprintf("loading filter\n");
    sce_paf_private_strcpy(filebuf, "xx0:/seplugins/gclite_filter.txt");
    SET_DEVICENAME(filebuf, location);
    if((fd = sceIoOpen(filebuf, PSP_O_RDONLY, 0777)) >= 0) {
        filter_size[location] = sceIoLseek(fd, 0, PSP_SEEK_END);
        sceIoLseek(fd, 0, PSP_SEEK_SET);
        filter_data[location] = sce_paf_private_malloc(filter_size[location]);
        sceIoRead(fd, filter_data[location], filter_size[location]);
        sceIoClose(fd);
        counter[location] = count_filters(filter_data[location], filter_size[location]);
        kprintf("total filter strings: %i\n", counter[location]);
        return 0;
    } else {
        kprintf("filter not found\n");
    }
    return -1;
}

int load_filter() {
    int r0 = load_filter_for(MEMORY_STICK);
    int r1 = load_filter_for(INTERNAL_STORAGE);
    return (r0 >= 0 || r1 >= 0) ? 0 : -1;
}

int check_filter(const char *str) {
    return check_filter_for(str, global_pos);
}
