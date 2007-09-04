/* Copyright (C) 2007 MySQL AB & Sanja Belkin

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#ifndef _ma_loghandler_h
#define _ma_loghandler_h

/* transaction log default cache size  (TODO: make it global variable) */
#define TRANSLOG_PAGECACHE_SIZE 1024*1024*2
/* transaction log default file size  (TODO: make it global variable) */
#define TRANSLOG_FILE_SIZE 1024*1024*1024
/* transaction log default flags (TODO: make it global variable) */
#define TRANSLOG_DEFAULT_FLAGS 0

/* Transaction log flags */
#define TRANSLOG_PAGE_CRC              1
#define TRANSLOG_SECTOR_PROTECTION     (1<<1)
#define TRANSLOG_RECORD_CRC            (1<<2)
#define TRANSLOG_FLAGS_NUM ((TRANSLOG_PAGE_CRC | TRANSLOG_SECTOR_PROTECTION | \
                           TRANSLOG_RECORD_CRC) + 1)

#define RECHEADER_READ_ERROR -1
#define RECHEADER_READ_EOF   -2

/*
  Page size in transaction log
  It should be Power of 2 and multiple of DISK_DRIVE_SECTOR_SIZE
  (DISK_DRIVE_SECTOR_SIZE * 2^N)
*/
#define TRANSLOG_PAGE_SIZE (8*1024)

#include "ma_loghandler_lsn.h"
#include "trnman_public.h"

/* short transaction ID type */
typedef uint16 SHORT_TRANSACTION_ID;

struct st_maria_info;

/* Length of CRC at end of pages */
#define CRC_LENGTH 4
/* Size of file id in logs */
#define FILEID_STORE_SIZE 2
/* Size of page reference in log */
#define PAGE_STORE_SIZE ROW_EXTENT_PAGE_SIZE
/* Size of page ranges in log */
#define PAGERANGE_STORE_SIZE ROW_EXTENT_COUNT_SIZE
#define DIRPOS_STORE_SIZE 1

/* Store methods to match the above sizes */
#define fileid_store(T,A) int2store(T,A)
#define page_store(T,A)   int5store(T,A)
#define dirpos_store(T,A) ((*(uchar*) (T)) = A)
#define pagerange_store(T,A) int2store(T,A)
#define fileid_korr(P) uint2korr(P)
#define page_korr(P)   uint5korr(P)
#define dirpos_korr(P) ((P)[0])
#define pagerange_korr(P) uint2korr(P)

/*
  Length of disk drive sector size (we assume that writing it
  to disk is atomic operation)
*/
#define DISK_DRIVE_SECTOR_SIZE 512

/*
  Number of empty entries we need to have in LEX_STRING for
  translog_write_record()
*/
#define LOG_INTERNAL_PARTS 1

/* position reserved in an array of parts of a log record */
#define TRANSLOG_INTERNAL_PARTS 2

/* types of records in the transaction log */
/* Todo: Set numbers for these when we have all entries figured out */

enum translog_record_type
{
  LOGREC_RESERVED_FOR_CHUNKS23= 0,
  LOGREC_REDO_INSERT_ROW_HEAD,
  LOGREC_REDO_INSERT_ROW_TAIL,
  LOGREC_REDO_INSERT_ROW_BLOB,
  LOGREC_REDO_INSERT_ROW_BLOBS,
  LOGREC_REDO_PURGE_ROW_HEAD,
  LOGREC_REDO_PURGE_ROW_TAIL,
  LOGREC_REDO_PURGE_BLOCKS,
  LOGREC_REDO_DELETE_ROW,
  LOGREC_REDO_UPDATE_ROW_HEAD,
  LOGREC_REDO_INDEX,
  LOGREC_REDO_UNDELETE_ROW,
  LOGREC_CLR_END,
  LOGREC_PURGE_END,
  LOGREC_UNDO_ROW_INSERT,
  LOGREC_UNDO_ROW_DELETE,
  LOGREC_UNDO_ROW_UPDATE,
  LOGREC_UNDO_ROW_PURGE,
  LOGREC_UNDO_KEY_INSERT,
  LOGREC_UNDO_KEY_DELETE,
  LOGREC_PREPARE,
  LOGREC_PREPARE_WITH_UNDO_PURGE,
  LOGREC_COMMIT,
  LOGREC_COMMIT_WITH_UNDO_PURGE,
  LOGREC_CHECKPOINT,
  LOGREC_REDO_CREATE_TABLE,
  LOGREC_REDO_RENAME_TABLE,
  LOGREC_REDO_DROP_TABLE,
  LOGREC_REDO_DELETE_ALL,
  LOGREC_REDO_REPAIR_TABLE,
  LOGREC_FILE_ID,
  LOGREC_LONG_TRANSACTION_ID,
  LOGREC_RESERVED_FUTURE_EXTENSION= 63
};
#define LOGREC_NUMBER_OF_TYPES 64              /* Maximum, can't be extended */

/* Size of log file; One log file is restricted to 4G */
typedef uint32 translog_size_t;

#define TRANSLOG_RECORD_HEADER_MAX_SIZE 1024

typedef struct st_translog_group_descriptor
{
  TRANSLOG_ADDRESS addr;
  uint8 num;
} TRANSLOG_GROUP;


typedef struct st_translog_header_buffer
{
  /* LSN of the read record */
  LSN lsn;
  /* array of groups descriptors, can be used only if groups_no > 0 */
  TRANSLOG_GROUP *groups;
  /* short transaction ID or 0 if it has no sense for the record */
  SHORT_TRANSACTION_ID short_trid;
  /*
     The Record length in buffer (including read header, but excluding
     hidden part of record (type, short TrID, length)
  */
  translog_size_t record_length;
  /*
     Buffer for write decoded header of the record (depend on the record
     type)
  */
  uchar header[TRANSLOG_RECORD_HEADER_MAX_SIZE];
  /* number of groups listed in  */
  uint groups_no;
  /* in multi-group number of chunk0 pages (valid only if groups_no > 0) */
  uint chunk0_pages;
  /* type of the read record */
  enum translog_record_type type;
  /* chunk 0 data address (valid only if groups_no > 0) */
  TRANSLOG_ADDRESS chunk0_data_addr;
   /*
     Real compressed LSN(s) size economy (<number of LSN(s)>*7 - <real_size>)
  */
  int16 compressed_LSN_economy;
  /* short transaction ID or 0 if it has no sense for the record */
  uint16 non_header_data_start_offset;
  /* non read body data length in this first chunk */
  uint16 non_header_data_len;
  /* chunk 0 data size (valid only if groups_no > 0) */
  uint16 chunk0_data_len;
} TRANSLOG_HEADER_BUFFER;


typedef struct st_translog_scanner_data
{
  uchar buffer[TRANSLOG_PAGE_SIZE];             /* buffer for page content */
  TRANSLOG_ADDRESS page_addr;                  /* current page address */
  /* end of the log which we saw last time */
  TRANSLOG_ADDRESS horizon;
  TRANSLOG_ADDRESS last_file_page;             /* Last page on in this file */
  uchar *page;                                  /* page content pointer */
  /* offset of the chunk in the page */
  translog_size_t page_offset;
  /* set horizon only once at init */
  my_bool fixed_horizon;
} TRANSLOG_SCANNER_DATA;


struct st_translog_reader_data
{
  TRANSLOG_HEADER_BUFFER header;                /* Header */
  TRANSLOG_SCANNER_DATA scanner;                /* chunks scanner */
  translog_size_t body_offset;                  /* current chunk body offset */
  /* data offset from the record beginning */
  translog_size_t current_offset;
  /* number of bytes read in header */
  uint16 read_header;
  uint16 chunk_size;                            /* current chunk size */
  uint current_group;                           /* current group */
  uint current_chunk;                           /* current chunk in the group */
  my_bool eor;                                  /* end of the record */
};

struct st_transaction;
C_MODE_START

/* Records types for unittests */
#define LOGREC_FIXED_RECORD_0LSN_EXAMPLE 1
#define LOGREC_VARIABLE_RECORD_0LSN_EXAMPLE 2
#define LOGREC_FIXED_RECORD_1LSN_EXAMPLE 3
#define LOGREC_VARIABLE_RECORD_1LSN_EXAMPLE 4
#define LOGREC_FIXED_RECORD_2LSN_EXAMPLE 5
#define LOGREC_VARIABLE_RECORD_2LSN_EXAMPLE 6

extern void example_loghandler_init();

extern my_bool translog_init(const char *directory, uint32 log_file_max_size,
			     uint32 server_version, uint32 server_id,
			     PAGECACHE *pagecache, uint flags);

extern my_bool
translog_write_record(LSN *lsn, enum translog_record_type type,
                      struct st_transaction *trn,
                      struct st_maria_info *tbl_info,
                      translog_size_t rec_len, uint part_no,
                      LEX_STRING *parts_data, uchar *store_share_id);

extern void translog_destroy();

extern int translog_read_record_header(LSN lsn, TRANSLOG_HEADER_BUFFER *buff);

extern void translog_free_record_header(TRANSLOG_HEADER_BUFFER *buff);

extern translog_size_t translog_read_record(LSN lsn,
					    translog_size_t offset,
					    translog_size_t length,
					    uchar *buffer,
					    struct st_translog_reader_data
					    *data);

extern my_bool translog_flush(LSN lsn);

extern my_bool translog_init_scanner(LSN lsn,
				     my_bool fixed_horizon,
				     struct st_translog_scanner_data *scanner);

extern int translog_read_next_record_header(TRANSLOG_SCANNER_DATA *scanner,
                                            TRANSLOG_HEADER_BUFFER *buff);
extern LSN translog_get_file_max_lsn_stored(uint32 file);
extern my_bool translog_purge(LSN low);
extern my_bool translog_is_file(uint file_no);
extern my_bool translog_lock();
extern my_bool translog_unlock();
extern void translog_lock_assert_owner();
extern TRANSLOG_ADDRESS translog_get_horizon();
extern int translog_assign_id_to_share(struct st_maria_share *share,
                                       struct st_transaction *trn);
extern void translog_deassign_id_from_share(struct st_maria_share *share);
extern void
translog_assign_id_to_share_from_recovery(struct st_maria_share *share,
                                          uint16 id);
extern my_bool translog_inited;

/*
  all the rest added because of recovery; should we make
  ma_loghandler_for_recovery.h ?
*/

#define SHARE_ID_MAX 65535 /* array's size */

extern LSN translog_first_lsn_in_log();
extern LSN translog_first_theoretical_lsn();

/* record parts descriptor */
struct st_translog_parts
{
  /* full record length */
  translog_size_t record_length;
  /* full record length with chunk headers */
  translog_size_t total_record_length;
  /* current part index */
  uint current;
  /* total number of elements in parts */
  uint elements;
  /* array of parts (LEX_STRING) */
  LEX_STRING *parts;
};

typedef my_bool(*prewrite_rec_hook) (enum translog_record_type type,
                                     TRN *trn, struct st_maria_info *tbl_info,
                                     struct st_translog_parts *parts);

typedef my_bool(*inwrite_rec_hook) (enum translog_record_type type,
                                    TRN *trn, struct st_maria_info *tbl_info,
                                    LSN *lsn,
                                    struct st_translog_parts *parts);

typedef uint16(*read_rec_hook) (enum translog_record_type type,
                                uint16 read_length, uchar *read_buff,
                                uchar *decoded_buff);


/* record classes */
enum record_class
{
  LOGRECTYPE_NOT_ALLOWED,
  LOGRECTYPE_VARIABLE_LENGTH,
  LOGRECTYPE_PSEUDOFIXEDLENGTH,
  LOGRECTYPE_FIXEDLENGTH
};

/* C++ can't bear that a variable's name is "class" */
#ifndef __cplusplus

enum enum_record_in_group {
  LOGREC_NOT_LAST_IN_GROUP= 0, LOGREC_LAST_IN_GROUP, LOGREC_IS_GROUP_ITSELF
};

/*
  Descriptor of log record type
  Note: Don't reorder because of constructs later...
*/
typedef struct st_log_record_type_descriptor
{
  /* internal class of the record */
  enum record_class class;
  /*
    length for fixed-size record, pseudo-fixed record
    length with uncompressed LSNs
  */
  uint16 fixed_length;
  /* how much record body (belonged to headers too) read with headers */
  uint16 read_header_len;
  /* HOOK for writing the record called before lock */
  prewrite_rec_hook prewrite_hook;
  /* HOOK for writing the record called when LSN is known, inside lock */
  inwrite_rec_hook inwrite_hook;
  /* HOOK for reading headers */
  read_rec_hook read_hook;
  /*
    For pseudo fixed records number of compressed LSNs followed by
    system header
  */
  int16 compressed_LSN;
  /*  the rest is for maria_read_log & Recovery */
  /** @brief for debug error messages or "maria_read_log" command-line tool */
  const char *name;
  enum enum_record_in_group record_in_group;
  /* a function to execute when we see the record during the REDO phase */
  int (*record_execute_in_redo_phase)(const TRANSLOG_HEADER_BUFFER *);
  /* a function to execute when we see the record during the UNDO phase */
  int (*record_execute_in_undo_phase)(const TRANSLOG_HEADER_BUFFER *, TRN *);
} LOG_DESC;

extern LOG_DESC log_record_type_descriptor[LOGREC_NUMBER_OF_TYPES];
#endif

C_MODE_END
#endif
