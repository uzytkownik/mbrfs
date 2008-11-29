/*
 * MBRFS is a filesystem in userspace reading disk images as partitions
 * Copyright (C) 2008  Maciej Piechotka
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define MBR_VERSION "0.0.0"

#define FUSE_USE_VERSION 26
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <fuse.h>

struct mbr_partition
{
  struct mbr_table {
    uint8_t boot;
    uint8_t start_chs[3];
    uint8_t type;
    uint8_t end_chs[3];
    uint32_t offset;
    uint32_t length;
  } table;

  off_t offset;
  off_t length;
  
  struct mbr_partition *next;
  struct mbr_partition *sub;

  bool mounted;
};

struct mbr_data
{
  int fd;
  uint8_t ro;
  char *filename;
  struct mbr_partition *primary;
};

static struct mbr_partition *
mbr_find_partition (struct mbr_data *data, const char *name,
		    char **rest)
{
  struct mbr_partition *cur;
  char *iter;

  if(rest)
    *rest = NULL;
  
  if (strncmp (name, "/mbr", 4) != 0)
    {
      return NULL;
    }

  cur = data->primary;
  
  iter = (char *)name + 4;
  while (1)
    {
      char *tmp;
      long long d;

      d = strtoll (iter, &tmp, 10);

      if (d < 0 || iter == tmp)
	return NULL;

      while (d--)
	{
	  if (cur->next)
	    cur = cur->next;
	  else
	    return NULL;
	}
      
      if (*tmp == '.')
	{
	  if (!cur->sub)
	    return NULL;
	  cur = cur->sub;
	  iter = tmp + 1;
	}
      else
	{
	  if (*tmp == '\0')
	    {
	      break;
	    }
	  else if (*tmp == '/')
	    {
	      if (rest)
		*rest = tmp + 1;
	    }
	  else
	    {
	      return NULL;
	    }	 
	}
    }
  return cur;
}

static struct mbr_data *
mbr_get_data ()
{
  return (struct mbr_data *) fuse_get_context ()->private_data;
}

static int
mbr_getattr (const char *path, struct stat *stbuf)
{
  struct mbr_partition *part;
  struct mbr_data *data;
  char *rest;

  memset (stbuf, 0, sizeof (struct stat));

  data = mbr_get_data ();
  
  if (!strcmp(path, "/"))
    {
      stbuf->st_mode = S_IFDIR | 0555;
      stbuf->st_nlink = 2;
      return 0;
    }
  
  part = mbr_find_partition (data, path, &rest);
  if (part == NULL)
    {
      return -ENOENT;
    }
  
  if (!rest && !part->mounted)
    {
      stbuf->st_mode = S_IFREG | 0644;
      stbuf->st_nlink = 1;
      stbuf->st_size = part->length;
      return 0;
    }
  else
    {
      return -ENOENT;
    }
}

static void
mbr_readdir_rec (void *buf, fuse_fill_dir_t filler,
		 const char *prefix, struct mbr_partition *part)
{
  size_t len;
  int iter;
  
  len = strlen (prefix) + 20;
  iter = 0;
  while (part)
    {
      char *tmp = alloca (len + 2);
      if (part->sub)
	{
	  snprintf (tmp, len, "%s%d.", prefix, iter);
	  mbr_readdir_rec (buf, filler, tmp, part->sub);
	  filler (buf, tmp, NULL, 0);
	}
      else if (part->table.type != 0x00)
	{
	  snprintf (tmp, len, "%s%d", prefix, iter);
	  filler (buf, tmp, NULL, 0);
	}
      
      part = part->next;
      iter++;
    }
}

static int
mbr_readdir (const char *path, void *buf, fuse_fill_dir_t filler,
	     off_t offset, struct fuse_file_info *fi)
{
  if (!strcmp (path, "/"))
    {
      filler (buf, ".", NULL, 0);
      filler (buf, "..", NULL, 0);

      mbr_readdir_rec (buf, filler, "mbr", mbr_get_data ()->primary);
      return 0;
    }
  else
    {
      /* TODO: Implement mounting */
      return -ENOENT;
    }
}

static int
mbr_open (const char *path, struct fuse_file_info *fi)
{
  struct mbr_data *data;
  struct mbr_partition *part;
  char *rest;
  
  if (!strcmp(path, "/"))
    {
      return 0;
    }
  
  data = mbr_get_data ();
  part = mbr_find_partition (data, path, &rest);

  if (part == NULL)
    return -ENOENT;

  if (!rest && !part->mounted)
    {
      if (data->ro  && (fi->flags & 3 != O_RDONLY))
	return -EROFS;
      
      return 0;
    }
  else
    {
      return -ENOENT;
    }
}

static int
mbr_read (const char *path, char *buf,
	  size_t size, off_t offset,
	  struct fuse_file_info *fi)
{
  struct mbr_data *data;
  struct mbr_partition *part;
  void *raw;
  char *rest;
  
  data = mbr_get_data ();
  part = mbr_find_partition (data, path, &rest);

  if (part == NULL)
    return -ENOENT;
  
  if (!rest && !part->mounted)
    {
      if (offset + (off_t)size > part->length)
	{
	  if (offset > part->length)
	    return 0;
	  size = part->length - offset;
	}
      
      raw = mmap (NULL, size, PROT_READ, MAP_PRIVATE, data->fd, offset);
      memcpy (buf, raw, size);
      munmap (raw, size);
      
      return size;
    }
  else
    {
      return -ENOENT;
    }
}

static struct fuse_operations mbr_oper = {
  .getattr = mbr_getattr,
  .readdir = mbr_readdir,
  .open    = mbr_open,
  .read    = mbr_read
};

enum {
  KEY_VERSION,
  KEY_HELP
};

static struct fuse_opt mbr_opts[] = {
  {"image=%s", offsetof(struct mbr_data, filename), 0},
  FUSE_OPT_KEY("-V",             KEY_VERSION),
  FUSE_OPT_KEY("--version",      KEY_VERSION),
  FUSE_OPT_KEY("-h",             KEY_HELP),
  FUSE_OPT_KEY("--help",         KEY_HELP),
  FUSE_OPT_END
};

static int
mbr_opt_proc (void *data, const char *arg, int key,
	      struct fuse_args *args)
{
  switch (key)
    {
    case KEY_VERSION:
      fprintf (stderr, "MBRFS %s\n", MBR_VERSION);
      fuse_opt_add_arg (args, "--version");
      fuse_main (args->argc, args->argv, &mbr_oper, data);
      exit(0);
    case KEY_HELP:
      fuse_opt_add_arg (args, "-ho");
      fuse_main (args->argc, args->argv, &mbr_oper, data);
      exit(0);
    default:
      return 1;
    }
}

static struct mbr_partition *
mbr_read_ebr (struct mbr_data *data, off_t offset)
{
  uint8_t *mbr;
  struct mbr_partition *part;
  uint32_t inner_offset;

  part = calloc(1, sizeof (struct mbr_partition));
  
  mbr = mmap (NULL, 512, PROT_READ, MAP_PRIVATE, data->fd, offset);
  if (mbr[0x1FE] != 0x55 || mbr[0x1FF] != 0xAA)
    {
      fprintf(stderr, "Wrong magic number.\n");
      exit (2);
    }

  memcpy (&part->table, mbr + 0x1BE, 16);
  part->offset = offset + (off_t)((off_t)part->table.offset * (off_t)512);
  part->length = (off_t)((off_t)part->table.length * (off_t)512);

  inner_offset = *((uint32_t *)&mbr[0x1CE + 0x008]);

  munmap (mbr, 512);

  if (part->table.type == 0x05 || part->table.type == 0x0f)
    {
      part->sub = mbr_read_ebr (data, part->offset);
    }
  
  if (inner_offset)
    {
      part->next = mbr_read_ebr (data, offset + (off_t)inner_offset);
    }
  
  return part;
}

static void
mbr_read_mbr (struct mbr_data *data)
{
  int iter;
  uint8_t *mbr;
  
  mbr = mmap (NULL, 512, PROT_READ, MAP_PRIVATE, data->fd, 0);
  if (mbr == MAP_FAILED)
    {
      fprintf (stderr, "Cannot map memory\n");
      exit (2);
    }
  
  /* Check magic number */
  if (mbr[0x1FE] != 0x55 || mbr[0x1FF] != 0xAA)
    {
      fprintf(stderr, "Wrong magic number.\n");
      exit (2);
    }

  /* Load data from MBR */
  data->primary = calloc (4, sizeof (struct mbr_partition));
  data->primary[0].next = &data->primary[1];
  data->primary[1].next = &data->primary[2];
  data->primary[2].next = &data->primary[3];

  iter = 0;
  while (iter < 4)
    {
      memcpy (&data->primary[iter].table, mbr + 0x1BE + iter * 0x010, 16);
      data->primary[iter].length =
	(off_t)((off_t)data->primary[iter].table.length * (off_t)512);
      data->primary[iter].offset =
	(off_t)((off_t)data->primary[iter].table.length * (off_t)512);
      iter++;
    }
  
  munmap (mbr, 512);

  /* Read EBRs */
  iter = 0;
  while (iter < 4)
    {
      if (data->primary[iter].table.type == 0x05 ||
	  data->primary[iter].table.type == 0x0f)
	{
	  data->primary[iter].sub =
	    mbr_read_ebr (data, data->primary[iter].offset);
	}
      iter++;
    }
}

int main(int argc, char *argv[])
{
  int ret;
  struct fuse_args args = FUSE_ARGS_INIT (argc, argv);
  struct mbr_data data;
  
  memset (&data, 0, sizeof (struct mbr_data));

  if (fuse_opt_parse (&args, &data, mbr_opts, mbr_opt_proc))
    return 2;

  data.fd = open (data.filename, O_RDWR);
  if (data.fd == -1)
    {
      switch (errno)
	{
	case EACCES:
	case EROFS:
	  data.ro = 1;
	  data.fd = open (data.filename, O_RDONLY);
	  if (data.fd != -1)
	    {
	      break;
	    }
	default:
	  fprintf (stderr, "Cannot open file %s: %s\n",
		   data.filename, strerror (errno));
	  exit (2);
	}
    }

  mbr_read_mbr (&data);
  
  ret = fuse_main (args.argc, args.argv, &mbr_oper, &data);

  if (ret)
    printf("\n");

  fuse_opt_free_args (&args);

  return ret;
}
