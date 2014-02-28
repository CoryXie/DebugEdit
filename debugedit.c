/* Copyright (C) 2001, 2002, 2003, 2005, 2007, 2009, 2010 Red Hat, Inc.
   Written by Alexander Larsson <alexl@redhat.com>, 2002
   Based on code by Jakub Jelinek <jakub@redhat.com>, 2001.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.  */


/* Needed for libelf */
#define _FILE_OFFSET_BITS 64

#include <assert.h>
#include <byteswap.h>
#include <endian.h>
#include <errno.h>
#include <error.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <popt.h>

#include <gelf.h>
#include "dwarf.h"
#include "hashtab.h"

#define DW_TAG_partial_unit 0x3c
#define DW_FORM_sec_offset 0x17
#define DW_FORM_exprloc 0x18
#define DW_FORM_flag_present 0x19
#define DW_FORM_ref_sig8 0x20

char *base_dir = NULL;
char *dest_dir = NULL;
char *list_file = NULL;
int win_path = 0;
int list_file_fd = -1;

typedef struct
    {
    Elf *elf;
    GElf_Ehdr ehdr;
    Elf_Scn **scn;
    const char *filename;
    int lastscn;
    GElf_Shdr shdr[0];
    } DSO;

typedef struct
    {
    unsigned char *ptr;
    uint32_t addend;
    } REL;

#define read_uleb128(ptr) ({        \
  unsigned int ret = 0;            \
  unsigned int c;            \
  int shift = 0;            \
  do                    \
    {                    \
      c = *ptr++;            \
      ret |= (c & 0x7f) << shift;    \
      shift += 7;            \
    } while (c & 0x80);            \
                    \
  if (shift >= 35)            \
    ret = UINT_MAX;            \
  ret;                    \
})

static uint16_t (*do_read_16) (unsigned char *ptr);
static uint32_t (*do_read_32) (unsigned char *ptr);
static void (*write_32) (unsigned char *ptr, GElf_Addr val);

static int ptr_size;
static int cu_version;

static inline uint16_t
buf_read_ule16 (unsigned char *data)
    {
    return data[0] | (data[1] << 8);
    }

static inline uint16_t
buf_read_ube16 (unsigned char *data)
    {
    return data[1] | (data[0] << 8);
    }

static inline uint32_t
buf_read_ule32 (unsigned char *data)
    {
    return data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
    }

static inline uint32_t
buf_read_ube32 (unsigned char *data)
    {
    return data[3] | (data[2] << 8) | (data[1] << 16) | (data[0] << 24);
    }

static const char *
strptr (DSO *dso, int sec, off_t offset)
    {
    Elf_Scn *scn;
    Elf_Data *data;

    scn = dso->scn[sec];
    if (offset >= 0 && (GElf_Addr) offset < dso->shdr[sec].sh_size)
        {
        data = NULL;
        while ((data = elf_getdata (scn, data)) != NULL)
            {
            if (data->d_buf
                    && offset >= data->d_off
                    && offset < data->d_off + data->d_size)
                return (const char *) data->d_buf + (offset - data->d_off);
            }
        }

    return NULL;
    }


#define read_1(ptr) *ptr++

#define read_16(ptr) ({                    \
  uint16_t ret = do_read_16 (ptr);            \
  ptr += 2;                        \
  ret;                            \
})

#define read_32(ptr) ({                    \
  uint32_t ret = do_read_32 (ptr);            \
  ptr += 4;                        \
  ret;                            \
})

REL *relptr, *relend;
int reltype;

#define do_read_32_relocated(ptr) ({            \
  uint32_t dret = do_read_32 (ptr);            \
  if (relptr)                        \
    {                            \
      while (relptr < relend && relptr->ptr < ptr)    \
    ++relptr;                    \
      if (relptr < relend && relptr->ptr == ptr)    \
    {                        \
      if (reltype == SHT_REL)            \
        dret += relptr->addend;            \
      else                        \
        dret = relptr->addend;            \
    }                        \
    }                            \
  dret;                            \
})

#define read_32_relocated(ptr) ({            \
  uint32_t ret = do_read_32_relocated (ptr);        \
  ptr += 4;                        \
  ret;                            \
})

static void
dwarf2_write_le32 (unsigned char *p, GElf_Addr val)
    {
    uint32_t v = (uint32_t) val;

    p[0] = v;
    p[1] = v >> 8;
    p[2] = v >> 16;
    p[3] = v >> 24;
    }


static void
dwarf2_write_be32 (unsigned char *p, GElf_Addr val)
    {
    uint32_t v = (uint32_t) val;

    p[3] = v;
    p[2] = v >> 8;
    p[1] = v >> 16;
    p[0] = v >> 24;
    }

static struct
    {
    const char *name;
    unsigned char *data;
    Elf_Data *elf_data;
    size_t size;
    int sec, relsec;
    } debug_sections[] =
    {
#define DEBUG_INFO    0
#define DEBUG_ABBREV    1
#define DEBUG_LINE    2
#define DEBUG_ARANGES    3
#define DEBUG_PUBNAMES    4
#define DEBUG_PUBTYPES    5
#define DEBUG_MACINFO    6
#define DEBUG_LOC    7
#define DEBUG_STR    8
#define DEBUG_FRAME    9
#define DEBUG_RANGES    10
#define DEBUG_TYPES    11
#define DEBUG_MACRO    12
#define DEBUG_GDB_SCRIPT    13
#define DEBUG_SYMTAB    14

        { ".debug_info", NULL, NULL, 0, 0, 0 },
        { ".debug_abbrev", NULL, NULL, 0, 0, 0 },
        { ".debug_line", NULL, NULL, 0, 0, 0 },
        { ".debug_aranges", NULL, NULL, 0, 0, 0 },
        { ".debug_pubnames", NULL, NULL, 0, 0, 0 },
        { ".debug_pubtypes", NULL, NULL, 0, 0, 0 },
        { ".debug_macinfo", NULL, NULL, 0, 0, 0 },
        { ".debug_loc", NULL, NULL, 0, 0, 0 },
        { ".debug_str", NULL, NULL, 0, 0, 0 },
        { ".debug_frame", NULL, NULL, 0, 0, 0 },
        { ".debug_ranges", NULL, NULL, 0, 0, 0 },
        { ".debug_types", NULL, NULL, 0, 0, 0 },
        { ".debug_macro", NULL, NULL, 0, 0, 0 },
        { ".debug_gdb_scripts", NULL, NULL, 0, 0, 0 },
        { ".symtab", NULL, NULL, 0, 0, 0 },
        { NULL, NULL, NULL, 0, 0, 0 }
    };

struct abbrev_attr
    {
    unsigned int attr;
    unsigned int form;
    };

struct abbrev_tag
    {
    unsigned int entry;
    unsigned int tag;
    int nattr;
    struct abbrev_attr attr[0];
    };

static hashval_t
abbrev_hash (const void *p)
    {
    struct abbrev_tag *t = (struct abbrev_tag *)p;

    return t->entry;
    }

static int
abbrev_eq (const void *p, const void *q)
    {
    struct abbrev_tag *t1 = (struct abbrev_tag *)p;
    struct abbrev_tag *t2 = (struct abbrev_tag *)q;

    return t1->entry == t2->entry;
    }

static void
abbrev_del (void *p)
    {
    free (p);
    }

static htab_t
read_abbrev (DSO *dso, unsigned char *ptr)
    {
    htab_t h = htab_try_create (50, abbrev_hash, abbrev_eq, abbrev_del);
    unsigned int attr, form;
    struct abbrev_tag *t;
    int size;
    void **slot;

    if (h == NULL)
        {
no_memory:
        error (0, ENOMEM, "%s: Could not read .debug_abbrev", dso->filename);
        if (h)
            htab_delete (h);
        return NULL;
        }

    while ((attr = read_uleb128 (ptr)) != 0)
        {
        size = 10;
        t = malloc (sizeof (*t) + size * sizeof (struct abbrev_attr));
        if (t == NULL)
            goto no_memory;
        t->entry = attr;
        t->nattr = 0;
        slot = htab_find_slot (h, t, INSERT);
        if (slot == NULL)
            {
            free (t);
            goto no_memory;
            }
        if (*slot != NULL)
            {
            error (0, 0, "%s: Duplicate DWARF abbreviation %d", dso->filename,
                   t->entry);
            free (t);
            htab_delete (h);
            return NULL;
            }
        t->tag = read_uleb128 (ptr);
        ++ptr; /* skip children flag.  */
        while ((attr = read_uleb128 (ptr)) != 0)
            {
            if (t->nattr == size)
                {
                size += 10;
                t = realloc (t, sizeof (*t) + size * sizeof (struct abbrev_attr));
                if (t == NULL)
                    goto no_memory;
                }
            form = read_uleb128 (ptr);
            if (form == 2
                    || (form > DW_FORM_flag_present && form != DW_FORM_ref_sig8))
                {
                error (0, 0, "%s: Unknown DWARF DW_FORM_%d", dso->filename, form);
                htab_delete (h);
                return NULL;
                }

            t->attr[t->nattr].attr = attr;
            t->attr[t->nattr++].form = form;
            }
        if (read_uleb128 (ptr) != 0)
            {
            error (0, 0, "%s: DWARF abbreviation does not end with 2 zeros",
                   dso->filename);
            htab_delete (h);
            return NULL;
            }
        *slot = t;
        }

    return h;
    }

#define IS_DIR_SEPARATOR(c) ((c)=='/')

static char *
canonicalize_path (const char *s, char *d)
    {
    char *rv = d;
    char *droot;

    if (IS_DIR_SEPARATOR (*s))
        {
        *d++ = *s++;
        if (IS_DIR_SEPARATOR (*s) && !IS_DIR_SEPARATOR (s[1]))
            {
            /* Special case for "//foo" meaning a Posix namespace
               escape.  */
            *d++ = *s++;
            }
        while (IS_DIR_SEPARATOR (*s))
            s++;
        }
    droot = d;

    while (*s)
        {
        /* At this point, we're always at the beginning of a path
        segment.  */

        if (s[0] == '.' && (s[1] == 0 || IS_DIR_SEPARATOR (s[1])))
            {
            s++;
            if (*s)
                while (IS_DIR_SEPARATOR (*s))
                    ++s;
            }

        else if (s[0] == '.' && s[1] == '.'
                 && (s[2] == 0 || IS_DIR_SEPARATOR (s[2])))
            {
            char *pre = d - 1; /* includes slash */
            while (droot < pre && IS_DIR_SEPARATOR (*pre))
                pre--;
            if (droot <= pre && ! IS_DIR_SEPARATOR (*pre))
                {
                while (droot < pre && ! IS_DIR_SEPARATOR (*pre))
                    pre--;
                /* pre now points to the slash */
                if (droot < pre)
                    pre++;
                if (pre + 3 == d && pre[0] == '.' && pre[1] == '.')
                    {
                    *d++ = *s++;
                    *d++ = *s++;
                    }
                else
                    {
                    d = pre;
                    s += 2;
                    if (*s)
                        while (IS_DIR_SEPARATOR (*s))
                            s++;
                    }
                }
            else
                {
                *d++ = *s++;
                *d++ = *s++;
                }
            }
        else
            {
            while (*s && ! IS_DIR_SEPARATOR (*s))
                *d++ = *s++;
            }

        if (IS_DIR_SEPARATOR (*s))
            {
            *d++ = *s++;
            while (IS_DIR_SEPARATOR (*s))
                s++;
            }
        }
    while (droot < d && IS_DIR_SEPARATOR (d[-1]))
        --d;
    if (d == rv)
        *d++ = '.';
    *d = 0;

    return rv;
    }

static int
has_prefix (const char  *str,
            const char  *prefix)
    {
    size_t str_len;
    size_t prefix_len;

    str_len = strlen (str);
    prefix_len = strlen (prefix);

    if (str_len < prefix_len)
        return 0;

    return strncmp (str, prefix, prefix_len) == 0;
    }

static int dirty_elf;
static void
dirty_section (unsigned int sec)
    {
    elf_flagdata (debug_sections[sec].elf_data, ELF_C_SET, ELF_F_DIRTY);
    dirty_elf = 1;
    }

void make_win_path(char * path)
    {
    int k = 0;
    while (path[k] != '\0')
        {
        if (path[k] == '/') path[k] = '\\';
        k++;
        }
    }

static int
edit_dwarf2_line (DSO *dso, uint32_t off, char *comp_dir, int phase)
    {
    unsigned char *ptr = debug_sections[DEBUG_LINE].data, *dir;
    unsigned char **dirt;
    unsigned char *endsec = ptr + debug_sections[DEBUG_LINE].size;
    unsigned char *endcu, *endprol;
    unsigned char opcode_base;
    uint32_t value, dirt_cnt;
    size_t comp_dir_len = strlen (comp_dir);
    size_t abs_file_cnt = 0, abs_dir_cnt = 0;

    if (phase != 0)
        return 0;

    /* XXX: RhBug:929365, should we error out instead of ignoring? */
    if (ptr == NULL)
        return 0;

    ptr += off;

    /* 
     * unit_length 
     * 
     * The size in bytes of the line number information for this 
     * compilation unit, not including the unit_length field itself. 
     */

    endcu = ptr + 4;
    endcu += read_32 (ptr); 
    if (endcu == ptr + 0xffffffff)
        {
        error (0, 0, "%s: 64-bit DWARF not supported", dso->filename);
        return 1;
        }

    if (endcu > endsec)
        {
        error (0, 0, "%s: .debug_line CU does not fit into section",
               dso->filename);
        return 1;
        }
    
    /*
     * version
     *
     * A version number. This number is specific to the line number 
     * information and is independent of the DWARF version number.
     */
     
    value = read_16 (ptr);
    if (value != 2 && value != 3 && value != 4)
        {
        error (0, 0, "%s: DWARF version %d unhandled", dso->filename,
               value);
        return 1;
        }
    
    /* 
     * header_length
     *
     * The number of bytes following the header_length field to the 
     * beginning of the first byte of the line number program itself. 
     * In the 32-bit DWARF format, this is a 4-byte unsigned length; 
     * in the 64-bit DWARF format, this field is an 8-byte unsigned 
     * length (see Section 7.4).
     */
     
    endprol = ptr + 4;
    endprol += read_32 (ptr);
    if (endprol > endcu)
        {
        error (0, 0, "%s: .debug_line CU prologue does not fit into CU",
               dso->filename);
        return 1;
        }

    opcode_base = ptr[4 + (value >= 4)];
    ptr = dir = ptr + 4 + (value >= 4) + opcode_base;

    /*
     * include_directories (sequence of path names)
     *
     * Entries in this sequence describe each path that was searched 
     * for included source files in this compilation.
     *
     * The last entry is followed by a single null byte.
     */
     
    /* dir table: */
    value = 1;
    while (*ptr != 0)
        {
        ptr = (unsigned char *) strchr ((char *)ptr, 0) + 1;
        ++value;
        }

    dirt = (unsigned char **) alloca (value * sizeof (unsigned char *));
    dirt[0] = (unsigned char *) ".";
    dirt_cnt = 1;
    ptr = dir;
    while (*ptr != 0)
        {
        dirt[dirt_cnt++] = ptr;
        ptr = (unsigned char *) strchr ((char *)ptr, 0) + 1;
        }
    ptr++;

    /* file table: */
    while (*ptr != 0)
        {
        char *s, *file;
        size_t file_len, dir_len;

        file = (char *) ptr;
        ptr = (unsigned char *) strchr ((char *)ptr, 0) + 1;
        value = read_uleb128 (ptr);

        if (value >= dirt_cnt)
            {
            error (0, 0, "%s: Wrong directory table index %u",
                   dso->filename, value);
            return 1;
            }
        file_len = strlen (file);
        dir_len = strlen ((char *)dirt[value]);
        s = malloc (comp_dir_len + 1 + file_len + 1 + dir_len + 1);
        if (s == NULL)
            {
            error (0, ENOMEM, "%s: Reading file table", dso->filename);
            return 1;
            }
        
        if (*file == '/')
            {
            memcpy (s, file, file_len + 1);
            if (dest_dir && has_prefix (file, base_dir))
                ++abs_file_cnt;
            }
        else if (*dirt[value] == '/')
            {
            memcpy (s, dirt[value], dir_len);
            s[dir_len] = '/';
            memcpy (s + dir_len + 1, file, file_len + 1);
            }
        else
            {
            char *p = s;
            if (comp_dir_len != 0)
                {
                memcpy (s, comp_dir, comp_dir_len);
                s[comp_dir_len] = '/';
                p += comp_dir_len + 1;
                }
            memcpy (p, dirt[value], dir_len);
            p[dir_len] = '/';
            memcpy (p + dir_len + 1, file, file_len + 1);
            }
        
        printf("@@@@linedirt[%d] %s\n", value, dirt[value]);
        
        canonicalize_path (s, s);
                
        if (list_file_fd != -1)
            {
            char *p = NULL;
            if (base_dir == NULL)
                p = s;
            else if (has_prefix (s, base_dir))
                p = s + strlen (base_dir);
            else if (has_prefix (s, dest_dir))
                p = s + strlen (dest_dir);

            if (p)
                {
                size_t size = strlen (p) + 1;
                while (size > 0)
                    {
                    ssize_t ret = write (list_file_fd, p, size);
                    if (ret == -1)
                        break;
                    size -= ret;
                    p += ret;
                    }
                }
            }

        if (win_path)
            make_win_path(s);

        free (s);

        read_uleb128 (ptr);
        read_uleb128 (ptr);
        }
    ++ptr;

    if (dest_dir)
        {
        unsigned char *srcptr, *buf = NULL;
        size_t base_len = strlen (base_dir);
        size_t dest_len = strlen (dest_dir);
        size_t shrank = 0;

        if (dest_len == base_len)
            abs_file_cnt = 0;
        
        if (abs_file_cnt)
            {
            srcptr = buf = malloc (ptr - dir);
            memcpy (srcptr, dir, ptr - dir);
            ptr = dir;
            }
        else
            ptr = srcptr = dir;
        
        while (*srcptr != 0)
            {
            size_t len = strlen ((char *)srcptr) + 1;
            const unsigned char *readptr = srcptr;

            char *orig = strdup ((const char *) srcptr);
            
            printf("####linesrcptr %s\n", srcptr);

            if (*srcptr == '/' && has_prefix ((char *)srcptr, base_dir))
                {
                if (dest_len < base_len)
                    ++abs_dir_cnt;
                memcpy (ptr, dest_dir, dest_len);
                ptr += dest_len;
                readptr += base_len;
                }
            
            srcptr += len;

            shrank += srcptr - readptr;
            
            canonicalize_path ((char *)readptr, (char *)ptr);

            if (win_path)
                make_win_path((char *)ptr);
            
            len = strlen ((char *)ptr) + 1;
            shrank -= len;
            ptr += len;

            if (memcmp (orig, ptr - len, len))
                dirty_section (DEBUG_STR);
            free (orig);
            }

        if (shrank > 0)
            {
            if (--shrank == 0)
                error (EXIT_FAILURE, 0,
                       "canonicalization unexpectedly shrank by one character");
            else
                {
                memset (ptr, 'X', shrank);
                ptr += shrank;
                *ptr++ = '\0';
                }
            }

        if (abs_dir_cnt + abs_file_cnt != 0)
            {
            size_t len = (abs_dir_cnt + abs_file_cnt) * (base_len - dest_len);

            if (len == 1)
                error (EXIT_FAILURE, 0, "-b arg has to be either the same length as -d arg, or more than 1 char longer");
            memset (ptr, 'X', len - 1);
            ptr += len - 1;
            *ptr++ = '\0';
            }
        *ptr++ = '\0';
        ++srcptr;

        while (*srcptr != 0)
            {
            size_t len = strlen ((char *)srcptr) + 1;

            printf("@@@@line srcptr %s\n", srcptr);
            
            if (*srcptr == '/' && has_prefix ((char *)srcptr, base_dir))
                {
                memcpy (ptr, dest_dir, dest_len);
                if (dest_len < base_len)
                    {
                    memmove (ptr + dest_len, srcptr + base_len,
                             len - base_len);
                    ptr += dest_len - base_len;
                    }
                dirty_section (DEBUG_STR);
                }
            else if (ptr != srcptr)
                memmove (ptr, srcptr, len);
            
            srcptr += len;
            ptr += len;
            dir = srcptr;
            
            read_uleb128 (srcptr);
            read_uleb128 (srcptr);
            read_uleb128 (srcptr);
            
            if (ptr != dir)
                memmove (ptr, dir, srcptr - dir);
            ptr += srcptr - dir;
            }
        
        *ptr = '\0';
        
        free (buf);
        }
    return 0;
    }

static unsigned char *
edit_attributes (DSO *dso, unsigned char *ptr, struct abbrev_tag *t, int phase)
    {
    int i;
    uint32_t list_offs;
    int found_list_offs;
    char *comp_dir;

    comp_dir = NULL;
    list_offs = 0;
    found_list_offs = 0;
    for (i = 0; i < t->nattr; ++i)
        {
        uint32_t form = t->attr[i].form;
        size_t len = 0;
        size_t base_len, dest_len;

        while (1)
            {
            if (t->attr[i].attr == DW_AT_stmt_list)
                {
                if (form == DW_FORM_data4
                        || form == DW_FORM_sec_offset)
                    {
                    list_offs = do_read_32_relocated (ptr);
                    found_list_offs = 1;
                    }
                }

            if (t->attr[i].attr == DW_AT_comp_dir)
                {
                if (form == DW_FORM_string)
                    {
                    free (comp_dir);
                    comp_dir = strdup ((char *)ptr);
                    
                    printf("####1comp_dir %s\n", comp_dir);
                    
                    if (phase == 1 && dest_dir && has_prefix ((char *)ptr, base_dir))
                        {
                        base_len = strlen (base_dir);
                        dest_len = strlen (dest_dir);
                        
                        printf("####1updating base from %s to %s\n", base_dir, dest_dir);

                        memcpy (ptr, dest_dir, dest_len);
                        if (dest_len < base_len)
                            {
                            if (win_path)
                                memset(ptr + dest_len, '\\',
                                       base_len - dest_len);
                            else
                                memset(ptr + dest_len, '/',
                                       base_len - dest_len);

                            }
                        dirty_section (DEBUG_INFO);
                        }
                    }
                else if (form == DW_FORM_strp &&
                         debug_sections[DEBUG_STR].data)
                    {
                    char *dir;

                    dir = (char *) debug_sections[DEBUG_STR].data
                          + do_read_32_relocated (ptr);

                    free (comp_dir);
                    comp_dir = strdup (dir);

                    printf("####2comp_dir %s\n", comp_dir);

                    if (phase == 1 && dest_dir && has_prefix (dir, base_dir))
                        {
                        base_len = strlen (base_dir);
                        dest_len = strlen (dest_dir);

                        printf("####2updating base from %s to %s\n", base_dir, dest_dir);

                        memcpy (dir, dest_dir, dest_len);
                        if (dest_len < base_len)
                            {
                            memmove (dir + dest_len, dir + base_len,
                                     strlen (dir + base_len) + 1);
                            }
                        dirty_section (DEBUG_STR);
                        }
                    }
                }
            else if ((t->tag == DW_TAG_compile_unit
                      || t->tag == DW_TAG_partial_unit)
                     && t->attr[i].attr == DW_AT_name)
                {
                char *name;
                
                if (form == DW_FORM_strp && debug_sections[DEBUG_STR].data)
                    {
                    name = (char *) debug_sections[DEBUG_STR].data
                           + do_read_32_relocated (ptr);
                    }
                else if (form == DW_FORM_string && debug_sections[DEBUG_INFO].data)
                    {
                    name = (char *)(ptr);
                    }

                printf("====name %s\n", name);
                
                /* 
                 * If the compile unit has full path from root '/',
                 * and the compilation directory is still null,
                 * then we construct a compilation directory string.
                 */
                 
                if (*name == '/' && comp_dir == NULL)
                    {
                    /* 
                     * The strrchr() function shall locate the last 
                     * occurrence of c (converted to a char) in the 
                     * string pointed to by s. The terminating null 
                     * byte is considered to be part of the string.
                     *
                     * Upon successful completion, strrchr() shall 
                     * return a pointer to the byte or a null pointer
                     * if c does not occur in the string.
                     */
                     
                    char *enddir = strrchr (name, '/');
                    
                    /* 
                     * The compilation directory is constructed by 
                     * removing the file name from the compile unit
                     * name attribute.
                     */
                     
                    if (enddir != name)
                        {
                        comp_dir = malloc (enddir - name + 1);
                        memcpy (comp_dir, name, enddir - name);
                        comp_dir [enddir - name] = '\0';
                        }
                    else
                        comp_dir = strdup ("/");
                    }
                
                if (phase == 1 && dest_dir && has_prefix (name, base_dir))
                    {
                    base_len = strlen (base_dir);
                    dest_len = strlen (dest_dir);
                    
                    printf("====updating base from %s to %s\n", base_dir, dest_dir);
                    
                    memcpy (name, dest_dir, dest_len);
                    
                    if (form == DW_FORM_strp)
                        {
                        if (dest_len < base_len)
                            {
                            memmove (name + dest_len, name + base_len,
                                     strlen (name + base_len) + 1);
                            }

                        dirty_section (DEBUG_STR);
                        }
                    else 
                        {
                        if (dest_len < base_len)
                            {
                            if (win_path)
                                memset(name + dest_len, '\\',
                                       base_len - dest_len);
                            else
                                memset(name + dest_len, '/',
                                       base_len - dest_len);
                            }
                        
                        dirty_section (DEBUG_INFO);
                        }

                    if (win_path)
                        make_win_path((char *)name);
                   
                    }
                    
                }

            switch (form)
                {
                case DW_FORM_ref_addr:
                    if (cu_version == 2)
                        ptr += ptr_size;
                    else
                        ptr += 4;
                    break;
                case DW_FORM_flag_present:
                    break;
                case DW_FORM_addr:
                    ptr += ptr_size;
                    break;
                case DW_FORM_ref1:
                case DW_FORM_flag:
                case DW_FORM_data1:
                    ++ptr;
                    break;
                case DW_FORM_ref2:
                case DW_FORM_data2:
                    ptr += 2;
                    break;
                case DW_FORM_ref4:
                case DW_FORM_data4:
                case DW_FORM_sec_offset:
                    ptr += 4;
                    break;
                case DW_FORM_ref8:
                case DW_FORM_data8:
                case DW_FORM_ref_sig8:
                    ptr += 8;
                    break;
                case DW_FORM_sdata:
                case DW_FORM_ref_udata:
                case DW_FORM_udata:
                    read_uleb128 (ptr);
                    break;
                case DW_FORM_strp:
                    ptr += 4;
                    break;
                case DW_FORM_string:
                    ptr = (unsigned char *) strchr ((char *)ptr, '\0') + 1;
                    break;
                case DW_FORM_indirect:
                    form = read_uleb128 (ptr);
                    continue;
                case DW_FORM_block1:
                    len = *ptr++;
                    break;
                case DW_FORM_block2:
                    len = read_16 (ptr);
                    form = DW_FORM_block1;
                    break;
                case DW_FORM_block4:
                    len = read_32 (ptr);
                    form = DW_FORM_block1;
                    break;
                case DW_FORM_block:
                case DW_FORM_exprloc:
                    len = read_uleb128 (ptr);
                    form = DW_FORM_block1;
                    assert (len < UINT_MAX);
                    break;
                default:
                    error (0, 0, "%s: Unknown DWARF DW_FORM_%d", dso->filename,
                           form);
                    return NULL;
                }

            if (form == DW_FORM_block1)
                ptr += len;

            break;
            }
        }

    /* Ensure the CU current directory will exist even if only empty.  Source
       filenames possibly located in its parent directories refer relatively to
       it and the debugger (GDB) cannot safely optimize out the missing
       CU current dir subdirectories.  */
    if (comp_dir && list_file_fd != -1)
        {
        char *p;
        size_t size;

        if (base_dir && has_prefix (comp_dir, base_dir))
            p = comp_dir + strlen (base_dir);
        else if (dest_dir && has_prefix (comp_dir, dest_dir))
            p = comp_dir + strlen (dest_dir);
        else
            p = comp_dir;

        size = strlen (p) + 1;
        while (size > 0)
            {
            ssize_t ret = write (list_file_fd, p, size);
            if (ret == -1)
                break;
            size -= ret;
            p += ret;
            }
        }

    if (found_list_offs && comp_dir)
        edit_dwarf2_line (dso, list_offs, comp_dir, phase);

    free (comp_dir);

    return ptr;
    }

static int
rel_cmp (const void *a, const void *b)
    {
    REL *rela = (REL *) a, *relb = (REL *) b;

    if (rela->ptr < relb->ptr)
        return -1;

    if (rela->ptr > relb->ptr)
        return 1;

    return 0;
    }

static void
edit_symtab (DSO *dso, Elf_Data *data)
    {
    GElf_Sym sym;
    GElf_Shdr shdr;
    unsigned long stridx = -1;
    int i;
    char *s;
    int sec = debug_sections[DEBUG_SYMTAB].sec;
    Elf_Data *strtab_data;
    gelf_getshdr(dso->scn[sec], &shdr);

    stridx = shdr.sh_link;

    strtab_data = elf_getdata(dso->scn[stridx], NULL);
    
    i = 0;
    while (gelf_getsym(data, i++, &sym) != NULL) 
        {
        s = elf_strptr(dso->elf, stridx, sym.st_name);

        if (GELF_ST_TYPE(sym.st_info) == STT_FILE)
            {
            printf("file %s\n", s);
            
            if (dest_dir && has_prefix (s, base_dir))
                {
                int base_len = strlen (base_dir);
                int dest_len = strlen (dest_dir);
            
                printf("!!!!updating symbol file base from %s to %s\n", base_dir, dest_dir);
            
                memcpy (s, dest_dir, dest_len);
                if (dest_len < base_len)
                    {
                    memmove (s + dest_len, s + base_len,
                             strlen (s + base_len) + 1);
                    }

                make_win_path(s);
                
                elf_flagdata (strtab_data, ELF_C_SET, ELF_F_DIRTY);
                }
            }
        else
            {
            printf("symbol %s\n", s);
            }
        }
    }

static int
edit_dwarf2 (DSO *dso)
    {
    Elf_Data *data;
    Elf_Scn *scn;
    int i, j;

    for (i = 0; debug_sections[i].name; ++i)
        {
        debug_sections[i].data = NULL;
        debug_sections[i].size = 0;
        debug_sections[i].sec = 0;
        debug_sections[i].relsec = 0;
        }
    ptr_size = 0;

    /* Record .debug_* sections into debug_sections[] array */
    
    for (i = 1; i < dso->ehdr.e_shnum; ++i)
        if (! (dso->shdr[i].sh_flags & (SHF_ALLOC | SHF_WRITE | SHF_EXECINSTR))
                && dso->shdr[i].sh_size)
            {
            /* 
             * .debug_* sections do not have the following features:
             *
             * SHF.ALLOC - Section occupies memory during execution 
             * SHF.WRITE - Section whose contents should be writable in execution
             * SHF.EXECINSTR - Section contains executable machine instructions
             */
             
            const char *name = strptr (dso, dso->ehdr.e_shstrndx,
                                       dso->shdr[i].sh_name);

            if (strncmp (name, ".debug_", sizeof (".debug_") - 1) == 0)
                {
                for (j = 0; debug_sections[j].name; ++j)
                    if (strcmp (name, debug_sections[j].name) == 0)
                        {
                        if (debug_sections[j].data)
                            {
                            error (0, 0, "%s: Found two copies of %s section",
                                   dso->filename, name);
                            return 1;
                            }

                        scn = dso->scn[i];
                        data = elf_getdata (scn, NULL);
                        assert (data != NULL && data->d_buf != NULL);
                        assert (elf_getdata (scn, data) == NULL);
                        assert (data->d_off == 0);
                        assert (data->d_size == dso->shdr[i].sh_size);
                        debug_sections[j].data = data->d_buf;
                        debug_sections[j].elf_data = data;
                        debug_sections[j].size = data->d_size;
                        debug_sections[j].sec = i;
                        break;
                        }

                if (debug_sections[j].name == NULL)
                    {
                    error (0, 0, "%s: Unknown debugging section %s",
                           dso->filename, name);
                    }
                }
            else if (dso->ehdr.e_type == ET_REL /* Relocatable file */
                     && ((dso->shdr[i].sh_type == SHT_REL /* Relocation entries without explicit addends */
                          && strncmp (name, ".rel.debug_",
                                      sizeof (".rel.debug_") - 1) == 0)
                         || (dso->shdr[i].sh_type == SHT_RELA /* Relocation entries with explicit addends */
                             && strncmp (name, ".rela.debug_",
                                         sizeof (".rela.debug_") - 1) == 0)))
                {
                for (j = 0; debug_sections[j].name; ++j)
                    if (strcmp (name + sizeof (".rel") - 1
                                + (dso->shdr[i].sh_type == SHT_RELA),
                                debug_sections[j].name) == 0)
                        {
                        debug_sections[j].relsec = i;
                        
                        printf("Relocation section %d name %s\n", i, name);
                        
                        break;
                        }
                }
            else if (strncmp (name, ".symtab", sizeof (".symtab") - 1) == 0)
                {
                printf("########.symtab sec %d\n", i);
                scn = dso->scn[i];
                data = elf_getdata (scn, NULL);
                debug_sections[DEBUG_SYMTAB].data = data->d_buf;
                debug_sections[DEBUG_SYMTAB].elf_data = data;
                debug_sections[DEBUG_SYMTAB].size = data->d_size;
                debug_sections[DEBUG_SYMTAB].sec = i;

                edit_symtab(dso, data);
                }
            }

    /* Get buffer reading functions according to endian mode */
    
    if (dso->ehdr.e_ident[EI_DATA] == ELFDATA2LSB)
        {
        do_read_16 = buf_read_ule16;
        do_read_32 = buf_read_ule32;
        write_32 = dwarf2_write_le32;
        }
    else if (dso->ehdr.e_ident[EI_DATA] == ELFDATA2MSB)
        {
        do_read_16 = buf_read_ube16;
        do_read_32 = buf_read_ube32;
        write_32 = dwarf2_write_be32;
        }
    else
        {
        error (0, 0, "%s: Wrong ELF data enconding", dso->filename);
        return 1;
        }

    /* Edit .debug_info section */
    
    if (debug_sections[DEBUG_INFO].data != NULL)
        {
        unsigned char *ptr, *endcu, *endsec;
        uint32_t value;
        htab_t abbrev;
        struct abbrev_tag tag, *t;
        int phase;
        REL *relbuf = NULL;

        /* Handle Relocation entries */
        
        if (debug_sections[DEBUG_INFO].relsec)
            {
            int ndx, maxndx;
            GElf_Rel rel;
            GElf_Rela rela;
            GElf_Sym sym;
            GElf_Addr base = dso->shdr[debug_sections[DEBUG_INFO].sec].sh_addr;
            Elf_Data *symdata = NULL;
            int rtype;

            i = debug_sections[DEBUG_INFO].relsec;
            scn = dso->scn[i];
            data = elf_getdata (scn, NULL);
            assert (data != NULL && data->d_buf != NULL);
            assert (elf_getdata (scn, data) == NULL);
            assert (data->d_off == 0);
            assert (data->d_size == dso->shdr[i].sh_size);
            maxndx = dso->shdr[i].sh_size / dso->shdr[i].sh_entsize;
            relbuf = malloc (maxndx * sizeof (REL));
            reltype = dso->shdr[i].sh_type;
            if (relbuf == NULL)
                error (1, errno, "%s: Could not allocate memory", dso->filename);

            symdata = elf_getdata (dso->scn[dso->shdr[i].sh_link], NULL);
            assert (symdata != NULL && symdata->d_buf != NULL);
            assert (elf_getdata (dso->scn[dso->shdr[i].sh_link], symdata)
                    == NULL);
            assert (symdata->d_off == 0);
            assert (symdata->d_size
                    == dso->shdr[dso->shdr[i].sh_link].sh_size);

            for (ndx = 0, relend = relbuf; ndx < maxndx; ++ndx)
                {
                if (dso->shdr[i].sh_type == SHT_REL)
                    {
                    gelf_getrel (data, ndx, &rel);
                    rela.r_offset = rel.r_offset;
                    rela.r_info = rel.r_info;
                    rela.r_addend = 0;
                    }
                else
                    gelf_getrela (data, ndx, &rela);
                gelf_getsym (symdata, ELF64_R_SYM (rela.r_info), &sym);
                /* Relocations against section symbols are uninteresting
                in REL.  */
                if (dso->shdr[i].sh_type == SHT_REL && sym.st_value == 0)
                    continue;
                /* Only consider relocations against .debug_str, .debug_line
                and .debug_abbrev.  */
                if (sym.st_shndx != debug_sections[DEBUG_STR].sec
                        && sym.st_shndx != debug_sections[DEBUG_LINE].sec
                        && sym.st_shndx != debug_sections[DEBUG_ABBREV].sec)
                    continue;
                rela.r_addend += sym.st_value;
                rtype = ELF64_R_TYPE (rela.r_info);
                switch (dso->ehdr.e_machine)
                    {
                    case EM_SPARC:
                    case EM_SPARC32PLUS:
                    case EM_SPARCV9:
                        if (rtype != R_SPARC_32 && rtype != R_SPARC_UA32)
                            goto fail;
                        break;
                    case EM_386:
                        if (rtype != R_386_32)
                            goto fail;
                        break;
                    case EM_PPC:
                    case EM_PPC64:
                        if (rtype != R_PPC_ADDR32 && rtype != R_PPC_UADDR32)
                            goto fail;
                        break;
                    case EM_S390:
                        if (rtype != R_390_32)
                            goto fail;
                        break;
                    case EM_IA_64:
                        if (rtype != R_IA64_SECREL32LSB)
                            goto fail;
                        break;
                    case EM_X86_64:
                        if (rtype != R_X86_64_32)
                            goto fail;
                        break;
                    default:
fail:
                        error (1, 0, "%s: Unhandled relocation %d in .debug_info section",
                               dso->filename, rtype);
                    }
                relend->ptr = debug_sections[DEBUG_INFO].data
                              + (rela.r_offset - base);
                relend->addend = rela.r_addend;
                ++relend;
                }
            if (relbuf == relend)
                {
                free (relbuf);
                relbuf = NULL;
                relend = NULL;
                }
            else
                qsort (relbuf, relend - relbuf, sizeof (REL), rel_cmp);
            }

        for (phase = 0; phase < 2; phase++)
            {
            printf("@@@###@@@phase %d@@@###@@@\n", phase);
            
            ptr = debug_sections[DEBUG_INFO].data;
            relptr = relbuf;
            endsec = ptr + debug_sections[DEBUG_INFO].size;

            /* Parse the .debug_info data buffer */
            
            while (ptr < endsec)
                {
                /* The .debug_info should be at least 11 bytes */
                
                if (ptr + 11 > endsec)
                    {
                    error (0, 0, "%s: .debug_info CU header too small",
                           dso->filename);
                    return 1;
                    }

                endcu = ptr + 4; 
                endcu += read_32 (ptr); /* Length - 32 bits */
                if (endcu == ptr + 0xffffffff)
                    {
                    error (0, 0, "%s: 64-bit DWARF not supported", dso->filename);
                    return 1;
                    }

                if (endcu > endsec)
                    {
                    error (0, 0, "%s: .debug_info too small", dso->filename);
                    return 1;
                    }

                cu_version = read_16 (ptr); /* Version - 16 bits */
                if (cu_version != 2 && cu_version != 3 && cu_version != 4)
                    {
                    error (0, 0, "%s: DWARF version %d unhandled", dso->filename,
                           cu_version);
                    return 1;
                    }

                value = read_32_relocated (ptr); /* Abbrev Offset - 32 bits */
                if (value >= debug_sections[DEBUG_ABBREV].size)
                    {
                    if (debug_sections[DEBUG_ABBREV].data == NULL)
                        error (0, 0, "%s: .debug_abbrev not present", dso->filename);
                    else
                        error (0, 0, "%s: DWARF CU abbrev offset too large",
                               dso->filename);
                    return 1;
                    }

                if (ptr_size == 0)
                    {
                    ptr_size = read_1 (ptr); /* Pointer Size - 8 bits */
                    if (ptr_size != 4 && ptr_size != 8)
                        {
                        error (0, 0, "%s: Invalid DWARF pointer size %d",
                               dso->filename, ptr_size);
                        return 1;
                        }
                    }
                else if (read_1 (ptr) != ptr_size) /* Pointer Size - 8 bits */
                    {
                    error (0, 0, "%s: DWARF pointer size differs between CUs",
                           dso->filename);
                    return 1;
                    }
                
                /* Read from .debug_abbrev section at Abbrev Offset */
                
                abbrev = read_abbrev (dso,
                                      debug_sections[DEBUG_ABBREV].data + value);
                if (abbrev == NULL)
                    return 1;

                while (ptr < endcu)
                    {
                    tag.entry = read_uleb128 (ptr);
                    if (tag.entry == 0)
                        continue;
                    t = htab_find_with_hash (abbrev, &tag, tag.entry);
                    if (t == NULL)
                        {
                        error (0, 0, "%s: Could not find DWARF abbreviation %d",
                               dso->filename, tag.entry);
                        htab_delete (abbrev);
                        return 1;
                        }

                    ptr = edit_attributes (dso, ptr, t, phase);
                    if (ptr == NULL)
                        break;
                    }

                htab_delete (abbrev);
                }
            }
        free (relbuf);
        }

    return 0;
    }

static struct poptOption optionsTable[] =
    {
        {
        "base-dir",  'b', POPT_ARG_STRING, &base_dir, 0,
        "base build directory of objects", NULL
        },
        {
        "dest-dir",  'd', POPT_ARG_STRING, &dest_dir, 0,
        "directory to rewrite base-dir into", NULL
        },
        {
        "list-file", 'l', POPT_ARG_STRING, &list_file, 0,
        "file where to put list of source and header file names", NULL
        },
        {
        "win-path",  'w', POPT_ARG_NONE, &win_path, 0,
        "change the path delimiter to be Windows compatible", NULL
        },
    POPT_AUTOHELP
        { NULL, 0, 0, NULL, 0, NULL, NULL }
    };

static DSO *
fdopen_dso (int fd, const char *name)
    {
    Elf *elf = NULL;
    GElf_Ehdr ehdr;
    int i;
    DSO *dso = NULL;

    elf = elf_begin (fd, ELF_C_RDWR, NULL);
    if (elf == NULL)
        {
        error (0, 0, "cannot open ELF file: %s", elf_errmsg (-1));
        goto error_out;
        }

    if (elf_kind (elf) != ELF_K_ELF)
        {
        error (0, 0, "\"%s\" is not an ELF file", name);
        goto error_out;
        }

    if (gelf_getehdr (elf, &ehdr) == NULL)
        {
        error (0, 0, "cannot get the ELF header: %s",
               elf_errmsg (-1));
        goto error_out;
        }

    if (ehdr.e_type != ET_DYN && ehdr.e_type != ET_EXEC && ehdr.e_type != ET_REL)
        {
        error (0, 0, "\"%s\" is not a shared library", name);
        goto error_out;
        }

    /* Allocate DSO structure. Leave place for additional 20 new section
       headers.  */
    dso = (DSO *)
          malloc (sizeof(DSO) + (ehdr.e_shnum + 20) * sizeof(GElf_Shdr)
                  + (ehdr.e_shnum + 20) * sizeof(Elf_Scn *));
    if (!dso)
        {
        error (0, ENOMEM, "Could not open DSO");
        goto error_out;
        }

    elf_flagelf (elf, ELF_C_SET, ELF_F_LAYOUT);

    memset (dso, 0, sizeof(DSO));
    dso->elf = elf;
    dso->ehdr = ehdr;
    dso->scn = (Elf_Scn **) &dso->shdr[ehdr.e_shnum + 20];

    for (i = 0; i < ehdr.e_shnum; ++i)
        {
        dso->scn[i] = elf_getscn (elf, i);
        gelf_getshdr (dso->scn[i], dso->shdr + i);
        }

    dso->filename = (const char *) strdup (name);
    return dso;

error_out:
    if (dso)
        {
        free ((char *) dso->filename);
        free (dso);
        }
    if (elf)
        elf_end (elf);
    if (fd != -1)
        close (fd);
    return NULL;
    }

int
main (int argc, char *argv[])
    {
    DSO *dso;
    int fd, i;
    const char *file;
    poptContext optCon;   /* context for parsing command-line options */
    int nextopt;
    const char **args;
    struct stat stat_buf;
    char *p;

    optCon = poptGetContext("debugedit", argc, (const char **)argv, optionsTable, 0);

    while ((nextopt = poptGetNextOpt (optCon)) > 0 || nextopt == POPT_ERROR_BADOPT)
        /* do nothing */ ;

    if (nextopt != -1)
        {
        fprintf (stderr, "Error on option %s: %s.\nRun '%s --help' to see a full list of available command line options.\n",
                 poptBadOption (optCon, 0),
                 poptStrerror (nextopt),
                 argv[0]);
        exit (1);
        }

    args = poptGetArgs (optCon);
    if (args == NULL || args[0] == NULL || args[1] != NULL)
        {
        poptPrintHelp(optCon, stdout, 0);
        exit (1);
        }

    if (dest_dir != NULL)
        {
        if (base_dir == NULL)
            {
            fprintf (stderr, "You must specify a base dir if you specify a dest dir\n");
            exit (1);
            }
        if (strlen (dest_dir) > strlen (base_dir))
            {
            fprintf (stderr, "Dest dir longer than base dir is not supported\n");
            exit (1);
            }
        }

    /* Make sure there are trailing slashes in dirs */
    
    if (base_dir != NULL && base_dir[strlen (base_dir)-1] != '/')
        {
        p = malloc (strlen (base_dir) + 2);
        strcpy (p, base_dir);
        strcat (p, "/");
        free (base_dir);
        base_dir = p;
        }
    
    if (dest_dir != NULL && dest_dir[strlen (dest_dir)-1] != '/')
        {
        p = malloc (strlen (dest_dir) + 2);
        strcpy (p, dest_dir);
        if (win_path)
            strcat (p, "\\");
        else
            strcat (p, "/");
        free (dest_dir);
        dest_dir = p;
        }

    if (list_file != NULL)
        {
        list_file_fd = open (list_file, O_WRONLY|O_CREAT|O_APPEND, 0644);
        }

    file = args[0];

    if (elf_version(EV_CURRENT) == EV_NONE)
        {
        fprintf (stderr, "library out of date\n");
        exit (1);
        }

    if (stat(file, &stat_buf) < 0)
        {
        fprintf (stderr, "Failed to open input file '%s': %s\n", file, strerror(errno));
        exit (1);
        }

    /* Make sure we can read and write */
    
    chmod (file, stat_buf.st_mode | S_IRUSR | S_IWUSR);

    fd = open (file, O_RDWR);
    if (fd < 0)
        {
        fprintf (stderr, "Failed to open input file '%s': %s\n", file, strerror(errno));
        exit (1);
        }

    dso = fdopen_dso (fd, file);
    if (dso == NULL)
        exit (1);

    for (i = 1; i < dso->ehdr.e_shnum; i++)
        {
        const char *name;
        name = strptr (dso, dso->ehdr.e_shstrndx, dso->shdr[i].sh_name);
        
        printf ("sh:%d, sh_type: %d, sh_name: %s\n", i, dso->shdr[i].sh_type, name);
        
        switch (dso->shdr[i].sh_type)
            {
            case SHT_PROGBITS:
                
                /* TODO: Handle stabs */
                if (strcmp (name, ".stab") == 0)
                    {
                    fprintf (stderr, "Stabs debuginfo not supported: %s\n", file);
                    break;
                    }
                
                if (strcmp (name, ".debug_info") == 0)
                    edit_dwarf2 (dso);

                break;
            default:
                break;
            }
        }

    if (elf_update (dso->elf, ELF_C_WRITE) < 0)
        {
        fprintf (stderr, "Failed to write file: %s\n", elf_errmsg (elf_errno()));
        exit (1);
        }
    
    if (elf_end (dso->elf) < 0)
        {
        fprintf (stderr, "elf_end failed: %s\n", elf_errmsg (elf_errno()));
        exit (1);
        }
    
    close (fd);

    /* Restore old access rights */
    chmod (file, stat_buf.st_mode);

    poptFreeContext (optCon);

    return 0;
    }
