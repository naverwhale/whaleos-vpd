/*
 * Copyright 2012 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <optional>
#include <string>

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fmap.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <uuid/uuid.h>

#include <base/files/file_util.h>
#include <base/logging.h>

extern "C" {
#include "lib/flashrom.h"
#include "lib/lib_smbios.h"
#include "lib/lib_vpd.h"
#include "lib/vpd.h"
#include "lib/vpd_tables.h"
};

namespace {

/* The buffer length. Right now the VPD partition size on flash is 128KB. */
#define BUF_LEN (128 * 1024)

/* The comment shown in the begin of --sh output */
#define SH_COMMENT                                                     \
  "#\n"                                                                \
  "# Prepend 'vpd -O' before this text to always reset VPD content.\n" \
  "# Append more -s at end to set additional key/values.\n"            \
  "# Or an empty line followed by other commands.\n"                   \
  "#\n"

/* Linked list to track temporary files
 */
struct TempfileNode {
  char* filename;
  struct TempfileNode* next;
}* tempfile_list = NULL;

enum FileFlag {
  HAS_SPD = (1 << 0),
  HAS_VPD_2_0 = (1 << 1),
  HAS_VPD_1_2 = (1 << 2),
  /* TODO(yjlou): refine those variables in main() to here:
   *              write_back_to_flash, overwrite_it, modified. */
};
/* Bitmask of FileFlag. */
int file_flag = 0;

/* 2 containers:
 *   file:      stores decoded pairs from file.
 *   argument:  stores parsed pairs from command arguments.
 */
struct PairContainer file;
struct PairContainer set_argument;
struct PairContainer del_argument;

/* The current padding length value.
 * Default: VPD_AS_LONG_AS
 */
int pad_value_len = VPD_AS_LONG_AS;

/* The output buffer */
uint8_t buf[BUF_LEN];
int buf_len = 0;
int max_buf_len = sizeof(buf);

/* The EPS base address used to fill the EPS table entry.
 * If the VPD partition can be found in fmap, this points to the starting
 * offset of VPD partition. If not found, this is used to be the base address
 * to increase SPD and VPD 2.0 offset fields.
 */
#define UNKNOWN_EPS_BASE ((uint32_t)-1)
uint32_t eps_base = UNKNOWN_EPS_BASE;

/* If found_vpd, replace the VPD partition when saveFile().
 * If not found, always create new file when saveFlie(). */
bool found_vpd = false;

/* The VPD partition offset and size in buf[]. The whole partition includes:
 *
 *   SMBIOS EPS
 *   SMBIOS tables[]
 *   SPD
 *   VPD 2.0 data
 *
 */
uint32_t vpd_offset = 0, vpd_size; /* The whole partition */
/* Below offset are related to vpd_offset and assume positive.
 * Those are used in saveFile() to write back data. */
uint32_t eps_offset = 0; /* EPS's starting address. Tables[] is following. */
uint32_t spd_offset = GOOGLE_SPD_OFFSET;      /* SPD address .*/
off_t vpd_2_0_offset = GOOGLE_VPD_2_0_OFFSET; /* VPD 2.0 data address. */

/* This points to the SPD data if it is availiable when loadFile().
 * The memory is allocated in loadFile(), will be used in saveFile(),
 * and freed at end of main(). */
uint8_t* spd_data = NULL;
int32_t spd_len = 256; /* max value for DDR3 */

/* Creates a temporary file and return the filename, or NULL for any failure.
 */
const char* myMkTemp() {
  char tmp_file[] = "/tmp/vpd.flashrom.XXXXXX";

  int fd = mkstemp(tmp_file);
  if (fd < 0) {
    fprintf(stderr, "mkstemp(%s) failed\n", tmp_file);
    return NULL;
  }

  close(fd);
  struct TempfileNode* node = reinterpret_cast<struct TempfileNode*>(
      malloc(sizeof(struct TempfileNode)));
  assert(node);
  node->next = tempfile_list;
  node->filename = strdup(tmp_file);
  assert(node->filename);
  tempfile_list = node;

  return node->filename;
}

/*  Erases all files created by myMkTemp
 */
void cleanTempFiles() {
  while (tempfile_list) {
    struct TempfileNode* node = tempfile_list;
    tempfile_list = node->next;
    if (unlink(node->filename) < 0) {
      fprintf(stderr, "warning: failed removing temporary file: %s\n",
              node->filename);
    }
    free(node->filename);
    free(node);
  }
}

/*  Given the offset of blob block (related to the first byte of EPS) and
 *  the size of blob, the is function generates an SMBIOS ESP.
 */
vpd_err_t buildEpsAndTables(const int size_blob,
                            const int max_buf_len,
                            unsigned char* buf,
                            int* generated) {
  struct vpd_entry* eps;
  unsigned char* table = NULL; /* the structure table */
  int table_len = 0;
  int num_structures = 0;
  vpd_err_t retval = VPD_OK;

  assert(buf);
  assert(generated);
  assert(eps_base != UNKNOWN_EPS_BASE);

  buf += *generated;

  /* Generate type 241 - SPD data */
  table_len = vpd_append_type241(
      0, &table, table_len, GOOGLE_SPD_UUID, eps_base + GOOGLE_SPD_OFFSET,
      spd_len, /* Max length for DDR3 */
      GOOGLE_SPD_VENDOR, GOOGLE_SPD_DESCRIPTION, GOOGLE_SPD_VARIANT);
  if (table_len < 0) {
    retval = VPD_FAIL;
    goto error_1;
  }
  num_structures++;

  /*
   * TODO(hungte) Once most systems have been updated to support VPD_INFO
   * record, we can remove the +sizeof(google_vpd_info) hack.
   */

  /* Generate type 241 - VPD 2.0 */
  table_len = vpd_append_type241(
      1, &table, table_len, GOOGLE_VPD_2_0_UUID,
      (eps_base + GOOGLE_VPD_2_0_OFFSET + sizeof(struct google_vpd_info)),
      size_blob, GOOGLE_VPD_2_0_VENDOR, GOOGLE_VPD_2_0_DESCRIPTION,
      GOOGLE_VPD_2_0_VARIANT);
  if (table_len < 0) {
    retval = VPD_FAIL;
    goto error_1;
  }
  num_structures++;

  /* Generate type 127 */
  table_len = vpd_append_type127(2, &table, table_len);
  if (table_len < 0) {
    retval = VPD_FAIL;
    goto error_1;
  }
  num_structures++;

  /* Generate EPS */
  eps = vpd_create_eps(table_len, num_structures, eps_base);
  if ((*generated + eps->entry_length) > max_buf_len) {
    retval = VPD_FAIL;
    goto error_2;
  }

  /* Copy EPS back to buf */
  memcpy(buf, eps, eps->entry_length);
  buf += eps->entry_length;
  *generated += eps->entry_length;

  /* Copy tables back to buf */
  if ((*generated + table_len) > max_buf_len) {
    retval = VPD_FAIL;
    goto error_2;
  }
  memcpy(buf, table, table_len);
  buf += table_len;
  *generated += table_len;

error_2:
  free(eps);
error_1:
  free(table);

  return retval;
}

int isbase64(uint8_t c) {
  return isalnum(c) || (c == '+') || (c == '/') || (c == '=');
}

std::optional<std::vector<uint8_t>> read_string_from_file(
    const char* file_name) {
  uint32_t i, j;

  auto file_buffer = base::ReadFileToBytes(base::FilePath(file_name));
  if (!file_buffer) {
    PLOG(ERROR) << "Failed to read file: " << file_name;
    return {};
  }

  /*
   * Are the contents a proper base64 blob? Verify it and drop EOL characters
   * along the way. This will help when displaying the contents.
   */
  for (i = 0, j = 0; i < file_buffer->size(); i++) {
    uint8_t c = (*file_buffer)[i];

    if ((c == 0xa) || (c == 0xd))
      continue; /* Skip EOL characters */

    if (!isbase64(c)) {
      fprintf(stderr, "[ERROR] file %s is not in base64 format (%c at %d)\n",
              file_name, c, i);
      return {};
    }
    (*file_buffer)[j++] = c;
  }
  (*file_buffer)[j] = '\0';

  return file_buffer;
}

/*
 * Check if given key name is compliant to recommended format.
 */
vpd_err_t checkKeyName(const uint8_t* name) {
  unsigned char c;
  while ((c = *name++)) {
    if (!(isalnum(c) || c == '_' || c == '.')) {
      fprintf(stderr, "[ERROR] VPD key name does not allow char [%c].\n", c);
      return VPD_ERR_PARAM;
    }
  }
  return VPD_OK;
}

/*
 * Check if key and value are compliant to recommended format.
 * For the checker of the key, see the function |checkKeyName|.
 * If key is "serial_number" or "mlb_serial_number", the value should only
 * contain characters a-z, A-Z, 0-9 or dash (-).
 */
vpd_err_t checkKeyValuePair(const uint8_t* key, const uint8_t* value) {
  int is_serial_number = 0;
  size_t value_len = 0;
  unsigned char c;

  vpd_err_t retval = checkKeyName(key);
  if (retval != VPD_OK)
    return retval;

  if (strncmp("serial_number", (const char*)key, sizeof("serial_number")) ==
          0 ||
      strncmp("mlb_serial_number", (const char*)key,
              sizeof("mlb_serial_number")) == 0)
    is_serial_number = 1;

  if (is_serial_number) {
    // For serial numbers, we only allow a-z, A-Z, 0-9 and dash (-).
    for (value_len = 0; (c = value[value_len]); value_len++) {
      if (!(isalnum(c) || c == '-')) {
        fprintf(stderr, "[ERROR] serial number does not allow char [%c].\n", c);
        return VPD_ERR_PARAM;
      }
    }

    if (value_len == 0) {
      fprintf(stderr, "[ERROR] serial number cannot be empty.\n");
      return VPD_ERR_PARAM;
    }

    if (value[0] == '-') {
      fprintf(stderr, "[ERROR] serial number cannot start with [-].\n");
      return VPD_ERR_PARAM;
    }

    if (value[value_len - 1] == '-') {
      fprintf(stderr, "[ERROR] serial number cannot end with [-].\n");
      return VPD_ERR_PARAM;
    }
  }

  return VPD_OK;
}

/*
 * Given a key=value string, this function parses it and adds to arugument
 * pair container. The 'value' can be stored in a base64 format file, in this
 * case the value field is the file name.
 */
vpd_err_t parseString(const uint8_t* string, bool read_from_file) {
  uint8_t* value;
  std::optional<std::vector<uint8_t>> file_contents;
  vpd_err_t retval = VPD_OK;

  uint8_t* key =
      reinterpret_cast<uint8_t*>(strdup(reinterpret_cast<const char*>(string)));
  if (!key || key[0] == '\0' || key[0] == '=') {
    if (key)
      free(key);
    return VPD_ERR_SYNTAX;
  }

  /*
   * Goes through the key string, and stops at the first '='.
   * If '=' is not found, the whole string is the key and
   * the value points to the end of string ('\0').
   */
  for (value = key; *value && *value != '='; value++) {
  }
  if (*value == '=') {
    *(value++) = '\0';

    if (read_from_file) {
      /* 'value' in fact is a file name */
      file_contents = read_string_from_file((const char*)value);
      if (!file_contents) {
        free(key);
        return VPD_ERR_SYNTAX;
      }
      value = file_contents->data();
    }
  }

  retval = checkKeyValuePair(key, value);
  if (retval == VPD_OK)
    setString(&set_argument, key, value, pad_value_len);

  free(key);

  return retval;
}

/* Given an address, compare if it is SMBIOS signature ("_SM_"). */
int isEps(const void* ptr) {
  return !memcmp(VPD_ENTRY_MAGIC, ptr, sizeof(VPD_ENTRY_MAGIC) - 1);
  /* TODO(yjlou): need more EPS validity checks here. */
}

/* There are two possible file content appearng here:
 *   1. a full and complete BIOS file
 *   2. a full but only VPD partition area is valid. (no fmap)
 *   3. a full BIOS, but VPD partition is blank.
 *
 * The first case is easy. Just lookup the fmap and find out the VPD partition.
 * The second is harder. We try to search the SMBIOS signature (since others
 * are blank). For the third, we just return and leave caller to read full
 * content, including fmap info.
 *
 * If found, vpd_offset and vpd_size are updated.
 */
vpd_err_t findVpdPartition(const std::vector<uint8_t>& read_buf,
                           const std::string& region_name,
                           uint32_t* vpd_offset,
                           uint32_t* vpd_size) {
  assert(vpd_offset);
  assert(vpd_size);

  /* scan the file and find out the VPD partition. */
  const off_t sig_offset = fmap_find(read_buf.data(), read_buf.size());
  if (sig_offset < 0) {
    return VPD_ERR_NOT_FOUND;
  }

  const struct fmap* fmap;
  if (sig_offset + sizeof(*fmap) > read_buf.size()) {
    LOG(ERROR) << "Bad FMAP at: " << sig_offset;
    return VPD_FAIL;
  }
  /* FMAP signature is found, try to search the partition name in table. */
  fmap = (const struct fmap*)(read_buf.data() + sig_offset);

  const struct fmap_area* area = fmap_find_area(fmap, region_name.c_str());
  if (!area) {
    LOG(ERROR) << "The VPD partition [" << region_name << "] is not found.";
    return VPD_ERR_NOT_FOUND;
  }
  *vpd_offset = area->offset;
  *vpd_size = area->size;
  /* Mark found here then saveFile() knows where to write back (vpd_offset,
   * vpd_size). */
  found_vpd = true;
  return VPD_OK;
}

vpd_err_t getVpdPartitionFromFullBios(const std::string& region_name,
                                      uint32_t* offset,
                                      uint32_t* size) {
  const char* filename = myMkTemp();
  if (!filename) {
    return VPD_ERR_SYSTEM;
  }

  if (FLASHROM_OK != flashromFullRead(filename)) {
    fprintf(stderr, "[WARN] Cannot read full BIOS.\n");
    return VPD_ERR_ROM_READ;
  }
  auto buf = base::ReadFileToBytes(base::FilePath(filename));
  assert(buf);
  if (findVpdPartition(*buf, region_name, offset, size)) {
    fprintf(stderr, "[WARN] Cannot get eps_base from full BIOS.\n");
    return VPD_ERR_INVALID;
  }
  return VPD_OK;
}

/* Below 2 functions are the helper functions for extract data from VPD 1.x
 * binary-encoded structure.
 * Note that the returning pointer is a static buffer. Thus the later call will
 * destroy the former call's result.
 */
uint8_t* extractString(const uint8_t* value, const int max_len) {
  static uint8_t buf[128];

  /* not longer than the buffer size */
  const int copy_len = (max_len > sizeof(buf) - 1) ? sizeof(buf) - 1 : max_len;
  memcpy(buf, value, copy_len);
  buf[copy_len] = '\0';

  return buf;
}

uint8_t* extractHex(const uint8_t* value, const int len) {
  char tmp[4]; /* for a hex string */
  static uint8_t buf[128];
  int in, out; /* in points to value[], while out points to buf[]. */

  for (in = 0, out = 0;; ++in) {
    if (out + 3 > sizeof(buf) - 1) {
      goto end_of_func; /* no more buffer */
    }
    if (in >= len) { /* no more input */
      if (out)
        --out; /* remove the tailing colon */
      goto end_of_func;
    }
    snprintf(tmp, sizeof(tmp), "%02x:", value[in]);
    memcpy(&buf[out], tmp, strnlen(tmp, sizeof(tmp)));
    out += strlen(tmp);
  }

end_of_func:
  buf[out] = '\0';

  return buf;
}

vpd_err_t loadRawFile(const char* filename, struct PairContainer* container) {
  uint32_t index;

  auto vpd_buf = base::ReadFileToBytes(base::FilePath(filename));
  if (!vpd_buf) {
    fprintf(stderr, "[ERROR] Cannot LoadRawFile('%s').\n", filename);
    return VPD_ERR_SYSTEM;
  }

  for (index = 0; index < vpd_buf->size() &&
                  (*vpd_buf)[index] != VPD_TYPE_TERMINATOR &&
                  (*vpd_buf)[index] != VPD_TYPE_IMPLICIT_TERMINATOR;) {
    vpd_err_t retval =
        decodeToContainer(container, vpd_buf->size(), vpd_buf->data(), &index);
    if (VPD_OK != retval) {
      fprintf(stderr, "decodeToContainer() error.\n");
      return retval;
    }
  }
  file_flag |= HAS_VPD_2_0;

  return VPD_OK;
}

vpd_err_t loadFile(const std::string& region_name,
                   const char* filename,
                   struct PairContainer* container,
                   bool overwrite_it) {
  struct vpd_entry* eps;
  uint32_t related_eps_base;
  struct vpd_header* header;
  struct vpd_table_binary_blob_pointer* data;
  uint8_t spd_uuid[16], vpd_2_0_uuid[16], vpd_1_2_uuid[16];
  int expected_handle;
  int table_len;
  uint32_t index;
  vpd_err_t retval = VPD_OK;

  auto read_buf = base::ReadFileToBytes(base::FilePath(filename));
  if (!read_buf) {
    fprintf(stderr, "[WARN] Cannot LoadFile('%s'), that's fine.\n", filename);
    return VPD_OK;
  }

  if (0 == findVpdPartition(*read_buf, region_name, &vpd_offset, &vpd_size)) {
    eps_base = vpd_offset;
  } else {
    /* We cannot parse out the VPD partition address from given file.
     * Then, try to read the whole BIOS chip. */
    uint32_t offset, size;
    retval = getVpdPartitionFromFullBios(region_name, &offset, &size);
    if (VPD_OK == retval) {
      eps_base = offset;
      vpd_size = size;
    } else {
      if (overwrite_it) {
        return VPD_OK;
      } else {
        fprintf(stderr, "[ERROR] getVpdPartitionFromFullBios() failed.");
        return retval;
      }
    }
  }

  /* Update the following variables:
   *   eps_base: integer, the VPD EPS address in ROM.
   *   vpd_offset: integer, the VPD partition offset in file (read_buf[]).
   *   vpd_buf: uint8_t*, points to the VPD partition.
   *   eps: vpd_entry*, points to the EPS structure.
   *   eps_offset: integer, the offset of EPS related to vpd_buf[].
   */
  const uint8_t* vpd_buf = read_buf->data() + vpd_offset;
  /* eps and eps_offset will be set slightly later. */

  if (eps_base == UNKNOWN_EPS_BASE) {
    fprintf(stderr,
            "[ERROR] Cannot determine eps_base. Cannot go on.\n"
            "        Ensure you have a valid FMAP.\n");
    return VPD_ERR_INVALID;
  }

  /* In overwrite mode, we don't care the content inside. Stop parsing. */
  if (overwrite_it) {
    return VPD_OK;
  }

  if (vpd_size < sizeof(struct vpd_entry)) {
    fprintf(stderr, "[ERROR] vpd_size:%d is too small to be compared.\n",
            vpd_size);
    return VPD_ERR_INVALID;
  }
  /* try to search the EPS if it is not aligned to the begin of partition. */
  for (index = 0; index < vpd_size; index += 16) {
    if (isEps(&vpd_buf[index])) {
      eps = (struct vpd_entry*)&vpd_buf[index];
      eps_offset = index;
      break;
    }
  }
  /* jump if the VPD partition is not recognized. */
  if (index >= vpd_size) {
    /* But OKAY if the VPD partition starts with FF, which might be un-used. */
    if (!memcmp("\xff\xff\xff\xff", vpd_buf, sizeof(VPD_ENTRY_MAGIC) - 1)) {
      fprintf(stderr, "[WARN] VPD partition not formatted. It's fine.\n");
      return VPD_OK;
    } else {
      fprintf(stderr, "SMBIOS signature is not matched.\n");
      fprintf(stderr, "You may use -O to overwrite the data.\n");
      return VPD_ERR_INVALID;
    }
  }

  /* adjust the eps_base for data->offset field below. */
  related_eps_base = eps->table_address - sizeof(*eps);

  /* EPS is done above. Parse structure tables below. */
  /* Get the first type 241 blob, at the tail of EPS. */
  header = reinterpret_cast<struct vpd_header*>(
      reinterpret_cast<uint8_t*>(eps) + eps->entry_length);
  data = reinterpret_cast<struct vpd_table_binary_blob_pointer*>(
      reinterpret_cast<uint8_t*>(header) + sizeof(*header));

  /* prepare data structure to compare */
  uuid_parse(GOOGLE_SPD_UUID, spd_uuid);
  uuid_parse(GOOGLE_VPD_2_0_UUID, vpd_2_0_uuid);
  uuid_parse(GOOGLE_VPD_1_2_UUID, vpd_1_2_uuid);

  /* Iterate all tables */
  for (expected_handle = 0; header->type != VPD_TYPE_END; ++expected_handle) {
    /* make sure we haven't have too much handle already. */
    if (expected_handle > 65535) {
      fprintf(stderr, "[ERROR] too many handles. Terminate parsing.\n");
      return VPD_ERR_INVALID;
    }

    /* check type */
    if (header->type != VPD_TYPE_BINARY_BLOB_POINTER) {
      fprintf(stderr,
              "[ERROR] We now only support Binary Blob Pointer (241). "
              "But the %dth handle is type %d. Terminate parsing.\n",
              header->handle, header->type);
      return VPD_ERR_INVALID;
    }

    /* make sure handle is increasing as expected */
    if (header->handle != expected_handle) {
      fprintf(stderr,
              "[ERROR] The handle value must be %d, but is %d.\n"
              "        Use -O option to re-format.\n",
              expected_handle, header->handle);
      return VPD_ERR_INVALID;
    }

    /* point to the table 241 data part */
    index = data->offset - related_eps_base;
    if (index >= read_buf->size()) {
      fprintf(stderr,
              "[ERROR] the table offset looks suspicious. "
              "index=0x%x, data->offset=0x%x, related_eps_base=0x%x\n",
              index, data->offset, related_eps_base);
      return VPD_ERR_INVALID;
    }

    /*
     * The main switch case
     */
    if (!memcmp(data->uuid, spd_uuid, sizeof(data->uuid))) {
      /* SPD */
      spd_offset = index;
      spd_len = data->size;
      if (vpd_offset + spd_offset + spd_len >= read_buf->size()) {
        fprintf(stderr,
                "[ERROR] SPD offset in BBP is not correct.\n"
                "        vpd=0x%x spd=0x%x len=0x%x file_size=0x%zx\n"
                "        If this file is VPD partition only, try to\n"
                "        use -E to adjust offset values.\n",
                (uint32_t)vpd_offset, (uint32_t)spd_offset, spd_len,
                read_buf->size());
        return VPD_ERR_INVALID;
      }

      if (!(spd_data = reinterpret_cast<uint8_t*>(malloc(spd_len)))) {
        fprintf(stderr, "spd_data: malloc(%d bytes) failed.\n", spd_len);
        return VPD_ERR_SYSTEM;
      }
      memcpy(spd_data, read_buf->data() + vpd_offset + spd_offset, spd_len);
      file_flag |= HAS_SPD;

    } else if (!memcmp(data->uuid, vpd_2_0_uuid, sizeof(data->uuid))) {
      /* VPD 2.0 */
      /* iterate all pairs */
      for (; vpd_buf[index] != VPD_TYPE_TERMINATOR &&
             vpd_buf[index] != VPD_TYPE_IMPLICIT_TERMINATOR;) {
        retval = decodeToContainer(container, vpd_size, vpd_buf, &index);
        if (VPD_OK != retval) {
          fprintf(stderr, "decodeToContainer() error.\n");
          return retval;
        }
      }
      file_flag |= HAS_VPD_2_0;

    } else if (!memcmp(data->uuid, vpd_1_2_uuid, sizeof(data->uuid))) {
      /* VPD 1_2: please refer to "Google VPD Type 241 Format v1.2" */
      const struct V12 {
        uint8_t prod_sn[0x20];
        uint8_t sku[0x10];
        uint8_t uuid[0x10];
        uint8_t mb_sn[0x10];
        uint8_t imei[0x10];
        uint8_t ssd_sn[0x10];
        uint8_t mem_sn[0x10];
        uint8_t wlan_mac[0x06];
      }* v12 = reinterpret_cast<const struct V12*>(&vpd_buf[index]);
      setString(container, reinterpret_cast<const uint8_t*>("Product_SN"),
                extractString(v12->prod_sn, sizeof(v12->prod_sn)),
                VPD_AS_LONG_AS);
      setString(container, reinterpret_cast<const uint8_t*>("SKU"),
                extractString(v12->sku, sizeof(v12->sku)), VPD_AS_LONG_AS);
      setString(container, reinterpret_cast<const uint8_t*>("UUID"),
                extractHex(v12->uuid, sizeof(v12->uuid)), VPD_AS_LONG_AS);
      setString(container, reinterpret_cast<const uint8_t*>("MotherBoard_SN"),
                extractString(v12->mb_sn, sizeof(v12->mb_sn)), VPD_AS_LONG_AS);
      setString(container, reinterpret_cast<const uint8_t*>("IMEI"),
                extractString(v12->imei, sizeof(v12->imei)), VPD_AS_LONG_AS);
      setString(container, reinterpret_cast<const uint8_t*>("SSD_SN"),
                extractString(v12->ssd_sn, sizeof(v12->ssd_sn)),
                VPD_AS_LONG_AS);
      setString(container, reinterpret_cast<const uint8_t*>("Memory_SN"),
                extractString(v12->mem_sn, sizeof(v12->mem_sn)),
                VPD_AS_LONG_AS);
      setString(container, reinterpret_cast<const uint8_t*>("WLAN_MAC"),
                extractHex(v12->wlan_mac, sizeof(v12->wlan_mac)),
                VPD_AS_LONG_AS);
      file_flag |= HAS_VPD_1_2;

    } else {
      /* un-supported UUID */
      char outstr[37]; /* 36-char + 1 null terminator */

      uuid_unparse(data->uuid, outstr);
      fprintf(stderr, "[ERROR] un-supported UUID: %s\n", outstr);
      return VPD_ERR_INVALID;
    }

    /* move to next table */
    if ((table_len = vpd_type241_size(header)) < 0) {
      fprintf(stderr, "[ERROR] Cannot get type 241 structure table length.\n");
      return VPD_ERR_INVALID;
    }

    header = reinterpret_cast<struct vpd_header*>(
        reinterpret_cast<uint8_t*>(header) + table_len);
    data = reinterpret_cast<struct vpd_table_binary_blob_pointer*>(
        reinterpret_cast<uint8_t*>(header) + sizeof(*header));
  }

  return VPD_OK;
}

vpd_err_t saveFile(const struct PairContainer* container,
                   const char* filename,
                   int write_back_to_flash) {
  FILE* fp;

  unsigned char eps[1024];
  memset(eps, 0xff, sizeof(eps));

  /* prepare info */
  struct google_vpd_info* info = (struct google_vpd_info*)buf;
  buf_len = sizeof(*info);
  memset(info, 0, buf_len);
  memcpy(info->header.magic, VPD_INFO_MAGIC, sizeof(info->header.magic));

  /* encode into buffer */
  vpd_err_t retval = encodeContainer(&file, max_buf_len, buf, &buf_len);
  if (VPD_OK != retval) {
    fprintf(stderr, "encodeContainer() error.\n");
    return retval;
  }
  retval = encodeVpdTerminator(max_buf_len, buf, &buf_len);
  if (VPD_OK != retval) {
    fprintf(stderr, "Out of space for terminator.\n");
    return retval;
  }
  info->size = buf_len - sizeof(*info);

  int eps_len = 0;
  retval = buildEpsAndTables(buf_len, sizeof(eps), eps, &eps_len);
  if (VPD_OK != retval) {
    fprintf(stderr, "Cannot build EPS.\n");
    return retval;
  }
  assert(eps_len <= GOOGLE_SPD_OFFSET);

  /* Write data in the following order:
   *   1. EPS
   *   2. SPD
   *   3. VPD 2.0
   */
  if (found_vpd) {
    /* We found VPD partition in -f file, which means file is existed.
     * Instead of truncating the whole file, open to write partial. */
    if (!(fp = fopen(filename, "r+"))) {
      fprintf(stderr, "File [%s] cannot be opened for write.\n", filename);
      return VPD_ERR_SYSTEM;
    }
  } else {
    /* VPD is not found, which means the file is pure VPD data.
     * Always creates the new file and overwrites the original content. */
    if (!(fp = fopen(filename, "w+"))) {
      fprintf(stderr, "File [%s] cannot be opened for write.\n", filename);
      return VPD_ERR_SYSTEM;
    }
  }

  const uint32_t file_seek = write_back_to_flash ? 0 : vpd_offset;

  /* write EPS */
  fseek(fp, file_seek + eps_offset, SEEK_SET);
  if (fwrite(eps, eps_len, 1, fp) != 1) {
    fprintf(stderr, "fwrite(EPS) error (%s)\n", strerror(errno));
    return VPD_ERR_SYSTEM;
  }

  /* write SPD */
  if (spd_data) {
    fseek(fp, file_seek + spd_offset, SEEK_SET);
    if (fwrite(spd_data, spd_len, 1, fp) != 1) {
      fprintf(stderr, "fwrite(SPD) error (%s)\n", strerror(errno));
      return VPD_ERR_SYSTEM;
    }
  }

  /* write VPD 2.0 */
  fseek(fp, file_seek + vpd_2_0_offset, SEEK_SET);
  if (fwrite(buf, buf_len, 1, fp) != 1) {
    fprintf(stderr, "fwrite(VPD 2.0) error (%s)\n", strerror(errno));
    return VPD_ERR_SYSTEM;
  }
  fclose(fp);

  return VPD_OK;
}

void usage(const char* progname) {
  printf("Chrome OS VPD 2.0 utility --\n");
#ifdef VPD_VERSION
  printf("%s\n", VPD_VERSION);
#endif
  printf("\n");
  printf("Usage: %s [OPTION] ...\n", progname);
  printf("   OPTIONs include:\n");
  printf("      -h               This help page and version.\n");
  printf("      -f <filename>    The output file name.\n");
  printf("      -E <address>     EPS base address (default:0x240000).\n");
  printf("      -S <key=file>    To add/change a string value, reading its\n");
  printf("                       base64 contents from a file.\n");
  printf("      -s <key=value>   To add/change a string value.\n");
  printf("      -p <pad length>  Pad if length is shorter.\n");
  printf("      -i <partition>   Specify VPD partition name in fmap.\n");
  printf("      -l               List content in the file.\n");
  printf("      --sh             Dump content for shell script.\n");
  printf("      --raw            Parse from a raw blob (without headers).\n");
  printf("      -0/--null-terminated\n");
  printf("                       Dump content in null terminate format.\n");
  printf("      -O               Overwrite and re-format VPD partition.\n");
  printf("      -g <key>         Print value string only.\n");
  printf("      -d <key>         Delete a key.\n");
  printf("\n");
  printf("   Notes:\n");
  printf("      You can specify multiple -s and -d. However, vpd always\n");
  printf("         applies -s first, then -d.\n");
  printf("      -g and -l must be mutually exclusive.\n");
  printf("\n");
}

}  // namespace

int main(int argc, char* argv[]) {
  int opt;
  int option_index = 0;
  vpd_err_t retval = VPD_OK;
  int export_type = VPD_EXPORT_KEY_VALUE;
  const char* optstring = "hf:s:S:p:i:lOg:d:0";
  static struct option long_options[] = {
      {"help", 0, 0, 'h'},
      {"file", 0, 0, 'f'},
      {"string", 0, 0, 's'},
      {"base64file", 0, 0, 'S'},
      {"pad", required_argument, 0, 'p'},
      {"partition", 0, 0, 'i'},
      {"list", 0, 0, 'l'},
      {"overwrite", 0, 0, 'O'},
      {"filter", 0, 0, 'g'},
      {"sh", 0, &export_type, VPD_EXPORT_AS_PARAMETER},
      {"raw", 0, 0, 'R'},
      {"null-terminated", 0, 0, '0'},
      {"delete", 0, 0, 'd'},
      {0, 0, 0, 0}};
  std::string region_name = "RO_VPD";
  char* filename = NULL;
  const char* load_file = NULL;
  const char* save_file = NULL;
  const char* tmp_part_file = NULL;
  const char* tmp_full_file = NULL;
  std::optional<std::string> key_to_export;
  int write_back_to_flash = 0;
  bool list_it = false;
  bool overwrite_it = false;
  int modified = 0;
  int num_to_delete;
  bool read_from_file = false;
  bool raw_input = false;

  initContainer(&file);
  initContainer(&set_argument);
  initContainer(&del_argument);

  while ((opt = getopt_long(argc, argv, optstring, long_options,
                            &option_index)) != EOF) {
    switch (opt) {
      case 'h':
        usage(argv[0]);
        goto teardown;
        break;

      case 'f':
        filename = strdup(optarg);
        break;

      case 'S':
        read_from_file = true;
        /* Fall through into the next case */
      case 's':
        retval =
            parseString(reinterpret_cast<uint8_t*>(optarg), read_from_file);
        if (VPD_OK != retval) {
          fprintf(stderr, "The string [%s] cannot be parsed.\n\n", optarg);
          goto teardown;
        }
        read_from_file = false;
        break;

      case 'p':
        errno = 0;
        pad_value_len = strtol(optarg, NULL, 0);

        /* FIXME: this is not a stable way to detect error because
         *        implementation may (or may not) assign errno. */
        if (!pad_value_len && errno == EINVAL) {
          fprintf(stderr, "Not a number for pad length: %s\n", optarg);
          retval = VPD_ERR_SYNTAX;
          goto teardown;
        }
        break;

      case 'i':
        region_name = std::string(optarg);
        if (region_name != "RO_VPD" && region_name != "RW_VPD") {
          LOG(ERROR) << "Invalid VPD partition name: " << region_name;
          retval = VPD_ERR_SYNTAX;
          goto teardown;
        }
        break;

      case 'l':
        list_it = true;
        break;

      case 'O':
        overwrite_it = true;
        /* This option forces to write empty data back even no new pair is
         * given. */
        modified = 1;
        break;

      case 'g':
        key_to_export = std::string(optarg);
        break;

      case 'd':
        /* Add key into container for delete. Since value is non-sense,
         * keep it empty. */
        setString(&del_argument, (const uint8_t*)optarg, (const uint8_t*)"", 0);
        break;

      case '0':
        export_type = VPD_EXPORT_NULL_TERMINATE;
        break;

      case 'R':
        raw_input = true;
        break;

      case 0:
        break;

      default:
        fprintf(stderr, "Invalid option (%s), use --help for usage.\n", optarg);
        retval = VPD_ERR_SYNTAX;
        goto teardown;
        break;
    }
  }

  if (optind < argc) {
    fprintf(stderr, "[ERROR] unexpected argument: %s\n\n", argv[optind]);
    usage(argv[0]);
    retval = VPD_ERR_SYNTAX;
    goto teardown;
  }

  if (list_it && key_to_export) {
    fprintf(stderr, "[ERROR] -l and -g must be mutually exclusive.\n");
    retval = VPD_ERR_SYNTAX;
    goto teardown;
  }

  if (VPD_EXPORT_KEY_VALUE != export_type && !list_it) {
    fprintf(stderr,
            "[ERROR] --sh/--null-terminated can be set only if -l is set.\n");
    retval = VPD_ERR_SYNTAX;
    goto teardown;
  }

  if (raw_input && !filename) {
    fprintf(stderr, "[ERROR] Needs -f FILE for raw input.\n");
    retval = VPD_ERR_SYNTAX;
    goto teardown;
  }

  if (raw_input &&
      (lenOfContainer(&set_argument) || lenOfContainer(&del_argument))) {
    fprintf(stderr, "[ERROR] Changing in raw mode is not supported.\n");
    retval = VPD_ERR_SYNTAX;
    goto teardown;
  }

  tmp_part_file = myMkTemp();
  tmp_full_file = myMkTemp();
  if (!tmp_part_file || !tmp_full_file) {
    fprintf(stderr, "[ERROR] Failed creating temporary files.\n");
    retval = VPD_ERR_SYSTEM;
    goto teardown;
  }

  /* if no filename is specified, call flashrom to read from flash. */
  if (!filename) {
    if (FLASHROM_OK != flashromPartialRead(tmp_part_file, tmp_full_file,
                                           region_name.c_str())) {
      fprintf(stderr, "[WARN] flashromPartialRead() failed, try full read.\n");
      /* Try to read whole file */
      if (FLASHROM_OK != flashromFullRead(tmp_full_file)) {
        fprintf(stderr, "[ERROR] flashromFullRead() error!\n");
        retval = VPD_ERR_ROM_READ;
        goto teardown;
      }
    }

    write_back_to_flash = 1;
    load_file = tmp_full_file;
    save_file = tmp_part_file;
  } else {
    load_file = filename;
    save_file = filename;
  }

  if (raw_input)
    retval = loadRawFile(load_file, &file);
  else
    retval = loadFile(region_name, load_file, &file, overwrite_it);
  if (VPD_OK != retval) {
    fprintf(stderr, "loadFile('%s') error.\n", load_file);
    goto teardown;
  }

  /* Do -s */
  if (lenOfContainer(&set_argument) > 0) {
    mergeContainer(&file, &set_argument);
    modified++;
  }

  /* Do -d */
  num_to_delete = lenOfContainer(&del_argument);
  if (subtractContainer(&file, &del_argument) != num_to_delete) {
    fprintf(stderr,
            "[ERROR] At least one of the keys to delete"
            " does not exist. Command ignored.\n");
    retval = VPD_ERR_PARAM;
    goto teardown;
  } else if (num_to_delete > 0) {
    modified++;
  }

  /* Do -g */
  if (key_to_export) {
    struct StringPair* foundString = findString(
        &file, reinterpret_cast<const uint8_t*>(key_to_export->c_str()), NULL);
    if (!foundString) {
      fprintf(stderr, "findString(): Vpd data '%s' was not found.\n",
              key_to_export->c_str());
      retval = VPD_FAIL;
      goto teardown;
    } else {
      uint8_t dump_buf[BUF_LEN * 2];
      int dump_len = 0;

      retval =
          exportStringValue(foundString, sizeof(dump_buf), dump_buf, &dump_len);
      if (VPD_OK != retval) {
        fprintf(stderr, "exportStringValue(): Cannot export the value.\n");
        goto teardown;
      }

      fwrite(dump_buf, dump_len, 1, stdout);
    }
  }

  /* Do -l */
  if (list_it) {
    /* Reserve larger size because the exporting generates longer string than
     * the encoded data. */
    uint8_t list_buf[BUF_LEN * 5 + 64];
    int list_len = 0;

    retval = exportContainer(export_type, &file, sizeof(list_buf), list_buf,
                             &list_len);
    if (VPD_OK != retval) {
      fprintf(stderr, "exportContainer(): Cannot generate string.\n");
      goto teardown;
    }

    /* Export necessary program parameters */
    if (VPD_EXPORT_AS_PARAMETER == export_type) {
      printf("%s%s -i %s \\\n", SH_COMMENT, argv[0], region_name.c_str());

      if (filename)
        printf("    -f %s \\\n", filename);
    }

    fwrite(list_buf, list_len, 1, stdout);
  }

  if (modified) {
    if (file_flag & HAS_VPD_1_2) {
      fprintf(stderr, "[ERROR] Writing VPD 1.2 not supported yet.\n");
      retval = VPD_FAIL;
      goto teardown;
    }

    retval = saveFile(&file, save_file, write_back_to_flash);
    if (VPD_OK != retval) {
      fprintf(stderr, "saveFile('%s') error: %d\n", filename, retval);
      goto teardown;
    }

    if (write_back_to_flash) {
      if (FLASHROM_OK !=
          flashromPartialWrite(save_file, tmp_full_file, region_name.c_str())) {
        fprintf(stderr, "flashromPartialWrite() error.\n");
        retval = VPD_ERR_ROM_WRITE;
        goto teardown;
      }
    }
  }

teardown:
  if (spd_data)
    free(spd_data);
  if (filename)
    free(filename);
  destroyContainer(&file);
  destroyContainer(&set_argument);
  destroyContainer(&del_argument);
  cleanTempFiles();

  return retval;
}
