#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#include "libforestdb/forestdb.h"
#include "couch_db.h"
/*
#include "configuration.h"
#include "debug.h"
#include "option.h"
#include "bitwise_utils.h"
#include "filemgr.h"
#include "filemgr_ops.h"
#include "wal.h"
#include "btreeblock.h"*/

#include "memleak.h"

#define META_BUF_MAXLEN (256)
#define SEQNUM_NOT_USED (0xffffffffffffffff)
#define MAX_KEYLEN (4096)

struct _db {
    fdb_handle *fdb;
    char *filename;
};

static uint64_t config_flags = 0x0;
static uint64_t cache_size = 0;
static int c_auto = 1;
static size_t c_threshold = 30;
static size_t wal_size = 4096;
couchstore_error_t couchstore_set_flags(uint64_t flags) {
    config_flags = flags;
    return COUCHSTORE_SUCCESS;
}
couchstore_error_t couchstore_set_cache(uint64_t size) {
    cache_size = size;
    return COUCHSTORE_SUCCESS;
}
couchstore_error_t couchstore_set_compaction(int mode,
                                             size_t threshold) {
    c_auto = mode;
    c_threshold = threshold;
    return COUCHSTORE_SUCCESS;
}
couchstore_error_t couchstore_set_wal_size(size_t size) {
    wal_size = size;
    return COUCHSTORE_SUCCESS;
}
couchstore_error_t couchstore_close_conn() {
    fdb_shutdown();
    return COUCHSTORE_SUCCESS;
}

void logCallbackFunc(int err_code,
                     const char *err_msg,
                     void *pCtxData) {
    fprintf(stderr, "%s - error code: %d, error message: %s\n",
            (char *) pCtxData, err_code, err_msg);
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_open_db(const char *filename,
                                      couchstore_open_flags flags,
                                      Db **pDb)
{
    return couchstore_open_db_ex(filename, flags,
                                 NULL, pDb);
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_open_db_ex(const char *filename,
                                         couchstore_open_flags flags,
                                         const couch_file_ops *ops,
                                         Db **pDb)
{
    fdb_config config;
    fdb_status status;
    fdb_handle *fdb;
    char *fname = (char *)filename;

    memset(&config, 0, sizeof(fdb_config));
    config = fdb_get_default_config();
    if (c_auto) {
        config.compaction_mode = FDB_COMPACTION_AUTO;
        config.compaction_threshold = c_threshold;
    } else {
        config.compaction_mode = FDB_COMPACTION_MANUAL;
    }
    config.chunksize = sizeof(uint64_t);
    config.buffercache_size = (uint64_t)cache_size;
    config.wal_threshold = wal_size;
    config.seqtree_opt = FDB_SEQTREE_NOT_USE;
    if (flags & 0x10) {
        config.durability_opt = FDB_DRB_NONE;
    } else {
        config.durability_opt = FDB_DRB_ASYNC;
    }
    config.compress_document_body = false;
    if (config_flags & 0x1) {
        config.wal_flush_before_commit = true;
    }

    *pDb = (Db*)malloc(sizeof(Db));
    //(*pDb)->seqnum = 0;
    (*pDb)->filename = (char *)malloc(strlen(filename)+1);
    strcpy((*pDb)->filename, filename);

    status = fdb_open(&fdb, fname, &config);
    (*pDb)->fdb = fdb;

    status = fdb_set_log_callback(fdb, logCallbackFunc, (void*)"worker");

    if (status == FDB_RESULT_SUCCESS) {
        return COUCHSTORE_SUCCESS;
    } else {
        free((*pDb)->filename);
        free(*pDb);
        return COUCHSTORE_ERROR_OPEN_FILE;
    }
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_close_db(Db *db)
{
    fdb_close(db->fdb);
    free(db->filename);
    free(db);

    return COUCHSTORE_SUCCESS;
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_db_info(Db *db, DbInfo* info)
{
    char **file, **new_file;
    size_t offset;

    info->space_used = fdb_estimate_space_used(db->fdb);

    // hack the DB handle to get internal filename
    offset = sizeof(void*)*3;
    file = *(char***)((uint8_t*)db->fdb + offset);
    offset = sizeof(void*)*4;
    new_file = *(char***)((uint8_t*)db->fdb + offset);

    if (new_file) {
        info->filename = *new_file;
    } else {
        info->filename = *file;
    }

    return COUCHSTORE_SUCCESS;
}

size_t _docinfo_to_buf(DocInfo *docinfo, void *buf)
{
    // [db_seq,] rev_seq, deleted, content_meta, rev_meta (size), rev_meta (buf)
    size_t offset = 0;

    memcpy((uint8_t*)buf + offset, &docinfo->rev_seq, sizeof(docinfo->rev_seq));
    offset += sizeof(docinfo->rev_seq);

    memcpy((uint8_t*)buf + offset, &docinfo->deleted, sizeof(docinfo->deleted));
    offset += sizeof(docinfo->deleted);

    memcpy((uint8_t*)buf + offset, &docinfo->content_meta, sizeof(docinfo->content_meta));
    offset += sizeof(docinfo->content_meta);

    memcpy((uint8_t*)buf + offset, &docinfo->rev_meta.size, sizeof(docinfo->rev_meta.size));
    offset += sizeof(docinfo->rev_meta.size);

    if (docinfo->rev_meta.size > 0) {
        memcpy((uint8_t*)buf + offset, docinfo->rev_meta.buf, docinfo->rev_meta.size);
        offset += docinfo->rev_meta.size;
    }

    return offset;
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_save_documents(Db *db, Doc* const docs[], DocInfo *infos[],
        unsigned numdocs, couchstore_save_options options)
{
    unsigned i;
    fdb_doc _doc;
    fdb_status status;
    uint8_t buf[META_BUF_MAXLEN];

    for (i=0;i<numdocs;++i){
        _doc.key = docs[i]->id.buf;
        _doc.keylen = docs[i]->id.size;
        _doc.body = docs[i]->data.buf;
        _doc.bodylen = docs[i]->data.size;
        _doc.metalen = _docinfo_to_buf(infos[i], buf);
        _doc.meta = buf;
        _doc.deleted = 0;

        status = fdb_set(db->fdb, &_doc);
        assert(status == FDB_RESULT_SUCCESS);

        infos[i]->db_seq = _doc.seqnum;
        infos[i]->bp = _doc.offset;
    }

    if (status == FDB_RESULT_SUCCESS)
        return COUCHSTORE_SUCCESS;
    else
        return COUCHSTORE_ERROR_ALLOC_FAIL;
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_save_document(Db *db, const Doc *doc, DocInfo *info,
        couchstore_save_options options)
{
    return couchstore_save_documents(db, (Doc**)&doc, (DocInfo**)&info, 1, options);
}

void _buf_to_docinfo(void *buf, size_t size, DocInfo *docinfo)
{
    size_t offset = 0;

    memcpy(&docinfo->rev_seq, (uint8_t*)buf + offset, sizeof(docinfo->rev_seq));
    offset += sizeof(docinfo->rev_seq);

    memcpy(&docinfo->deleted, (uint8_t*)buf + offset, sizeof(docinfo->deleted));
    offset += sizeof(docinfo->deleted);

    memcpy(&docinfo->content_meta, (uint8_t*)buf + offset, sizeof(docinfo->content_meta));
    offset += sizeof(docinfo->content_meta);

    memcpy(&docinfo->rev_meta.size, (uint8_t*)buf + offset, sizeof(docinfo->rev_meta.size));
    offset += sizeof(docinfo->rev_meta.size);

    if (docinfo->rev_meta.size > 0) {
        //docinfo->rev_meta.buf = (char *)malloc(docinfo->rev_meta.size);
        docinfo->rev_meta.buf = ((char *)docinfo) + sizeof(DocInfo);
        memcpy(docinfo->rev_meta.buf, (uint8_t*)buf + offset, docinfo->rev_meta.size);
        offset += docinfo->rev_meta.size;
    }else{
        docinfo->rev_meta.buf = NULL;
    }
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_docinfo_by_id(Db *db, const void *id, size_t idlen, DocInfo **pInfo)
{
    fdb_doc _doc;
    fdb_status status;
    size_t rev_meta_size;
    size_t meta_offset;

    meta_offset = sizeof(uint64_t)*1 + sizeof(int) + sizeof(couchstore_content_meta_flags);

    _doc.key = (void *)id;
    _doc.keylen = idlen;
    _doc.seqnum = SEQNUM_NOT_USED;
    _doc.meta = _doc.body = NULL;

    status = fdb_get_metaonly(db->fdb, &_doc);
    memcpy(&rev_meta_size, (uint8_t*)_doc.meta + meta_offset, sizeof(size_t));

    *pInfo = (DocInfo *)malloc(sizeof(DocInfo) + rev_meta_size);
    (*pInfo)->id.buf = (char *)id;
    (*pInfo)->id.size = idlen;
    (*pInfo)->size = _doc.bodylen;
    (*pInfo)->bp = _doc.offset;
    (*pInfo)->db_seq = _doc.seqnum;
    _buf_to_docinfo(_doc.meta, _doc.metalen, (*pInfo));

    free(_doc.meta);

    return COUCHSTORE_SUCCESS;
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_docinfos_by_id(Db *db, const sized_buf ids[], unsigned numDocs,
        couchstore_docinfos_options options, couchstore_changes_callback_fn callback, void *ctx)
{
    int i;
    fdb_doc _doc;
    fdb_status status;
    DocInfo *docinfo;
    size_t rev_meta_size, max_meta_size = 256;
    size_t meta_offset;

    meta_offset = sizeof(uint64_t)*1 + sizeof(int) + sizeof(couchstore_content_meta_flags);

    docinfo = (DocInfo*)malloc(sizeof(DocInfo) + max_meta_size);

    for (i=0;i<numDocs;++i){
        _doc.key = (void*)ids[i].buf;
        _doc.keylen = ids[i].size;
        _doc.seqnum = SEQNUM_NOT_USED;
        _doc.meta = _doc.body = NULL;

        status = fdb_get_metaonly(db->fdb, &_doc);
        assert(status != FDB_RESULT_FAIL);

        memcpy(&rev_meta_size, (uint8_t*)_doc.meta + meta_offset, sizeof(size_t));
        if (rev_meta_size > max_meta_size) {
            max_meta_size = rev_meta_size;
            docinfo = (DocInfo*)realloc(docinfo, sizeof(DocInfo) + max_meta_size);
        }

        memset(docinfo, 0, sizeof(DocInfo));
        docinfo->id.buf = ids[i].buf;
        docinfo->id.size = ids[i].size;
        docinfo->size = _doc.bodylen;
        docinfo->bp = _doc.offset;
        docinfo->db_seq = _doc.seqnum;
        _buf_to_docinfo(_doc.meta, _doc.metalen, docinfo);
        free(_doc.meta);

        callback(db, docinfo, ctx);
    }

    free(docinfo);

    return COUCHSTORE_SUCCESS;
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_docinfos_by_sequence(Db *db,
                                                   const uint64_t sequence[],
                                                   unsigned numDocs,
                                                   couchstore_docinfos_options options,
                                                   couchstore_changes_callback_fn callback,
                                                   void *ctx)
{
    int i;
    fdb_doc _doc;
    fdb_status status;
    DocInfo *docinfo;
    size_t rev_meta_size, max_meta_size = 256;
    size_t meta_offset;
    uint8_t keybuf[MAX_KEYLEN];

    meta_offset = sizeof(uint64_t)*1 + sizeof(int) + sizeof(couchstore_content_meta_flags);

    docinfo = (DocInfo*)malloc(sizeof(DocInfo) + max_meta_size);

    for (i=0;i<numDocs;++i){
        _doc.key = (void*)keybuf;
        _doc.seqnum = sequence[i];
        _doc.meta = _doc.body = NULL;

        status = fdb_get_metaonly_byseq(db->fdb, &_doc);
        assert(status != FDB_RESULT_FAIL);

        memcpy(&rev_meta_size, (uint8_t*)_doc.meta + meta_offset, sizeof(size_t));
        if (rev_meta_size > max_meta_size) {
            max_meta_size = rev_meta_size;
            docinfo = (DocInfo*)realloc(docinfo, sizeof(DocInfo) + max_meta_size);
        }

        memset(docinfo, 0, sizeof(DocInfo));
        docinfo->id.buf = (char *)keybuf;
        docinfo->id.size = _doc.keylen;
        docinfo->size = _doc.bodylen;
        docinfo->bp = _doc.offset;
        docinfo->db_seq = _doc.seqnum;
        _buf_to_docinfo(_doc.meta, _doc.metalen, docinfo);
        free(_doc.meta);

        callback(db, docinfo, ctx);
    }

    free(docinfo);

    return COUCHSTORE_SUCCESS;
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_open_document(Db *db,
                                            const void *id,
                                            size_t idlen,
                                            Doc **pDoc,
                                            couchstore_open_options options)
{
    fdb_doc _doc;
    fdb_status status;
    size_t rev_meta_size;
    size_t meta_offset;
    couchstore_error_t ret = COUCHSTORE_SUCCESS;

    meta_offset = sizeof(uint64_t)*1 + sizeof(int) + sizeof(couchstore_content_meta_flags);

    _doc.key = (void *)id;
    _doc.keylen = idlen;
    _doc.seqnum = SEQNUM_NOT_USED;
    _doc.meta = _doc.body = NULL;

    status = fdb_get(db->fdb, &_doc);
    if (status != FDB_RESULT_SUCCESS) {
        printf("\nget error %.*s\n", (int)idlen, (char*)id);
        ret = COUCHSTORE_ERROR_DOC_NOT_FOUND;
    }
    //assert(status == FDB_RESULT_SUCCESS);

    *pDoc = (Doc *)malloc(sizeof(Doc));
    (*pDoc)->id.buf = (char*)_doc.key;
    (*pDoc)->id.size = _doc.keylen;
    (*pDoc)->data.buf = (char*)_doc.body;
    (*pDoc)->data.size = _doc.bodylen;

    free(_doc.meta);

    return ret;
}


LIBCOUCHSTORE_API
void couchstore_free_document(Doc *doc)
{
    if (doc->id.buf) free(doc->id.buf);
    if (doc->data.buf) free(doc->data.buf);
    free(doc);
}

LIBCOUCHSTORE_API
void couchstore_free_docinfo(DocInfo *docinfo)
{
    //free(docinfo->rev_meta.buf);
    free(docinfo);
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_commit(Db *db)
{
    fdb_commit(db->fdb, FDB_COMMIT_NORMAL);
    return COUCHSTORE_SUCCESS;
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_compact_db_ex(Db* source, const char* target_filename,
        uint64_t flags, const couch_file_ops *ops)
{
    char *new_filename = (char *)target_filename;
    fdb_set_log_callback(source->fdb, logCallbackFunc, (void*)"compactor");
    fdb_compact(source->fdb, new_filename);

    return COUCHSTORE_SUCCESS;
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_compact_db(Db* source, const char* target_filename)
{
    return couchstore_compact_db_ex(source, target_filename, 0x0, NULL);
}

