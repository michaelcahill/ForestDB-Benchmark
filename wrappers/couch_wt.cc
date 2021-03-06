#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "wiredtiger.h"
#include "couch_db.h"

#define METABUF_MAXLEN (256)

struct _db {
    WT_CURSOR *cursor;
    WT_SESSION *session;
    char *filename;
    int sync;
};

static WT_CONNECTION *conn = NULL;
static uint64_t cache_size = 0;
static int indexing_type = 0;

couchstore_error_t couchstore_set_cache(uint64_t size) {
    cache_size = size;
    return COUCHSTORE_SUCCESS;
}
couchstore_error_t couchstore_set_idx_type(int type) {
    indexing_type = type;
    return COUCHSTORE_SUCCESS;
}

couchstore_error_t couchstore_open_conn(const char *filename)
{
    int fd;
    int ret;
    char config[256];

#ifdef PRIu64
    sprintf(config, "create,log=(enabled),cache_size=%"PRIu64, cache_size);
#else
    sprintf(config, "create,log=(enabled),cache_size=%llu", cache_size);
#endif
    // create directory if not exist
    fd = open(filename, O_RDONLY, 0666);
    if (fd == -1) {
        // create
        char cmd[256];

        sprintf(cmd, "mkdir %s\n", filename);
        ret = system(cmd);
    }

    wiredtiger_open(filename, NULL, config, &conn);

    return COUCHSTORE_SUCCESS;
}

couchstore_error_t couchstore_close_conn()
{
    conn->close(conn, NULL);
    return COUCHSTORE_SUCCESS;
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
    int i, len;
    Db *ppdb;
    char fileonly[256];
    char table_name[256];
    char table_config[256];
    char *err;

    assert(conn);

    *pDb = (Db*)malloc(sizeof(Db));
    ppdb = *pDb;

    ppdb->filename = (char*)malloc(strlen(filename)+1);
    strcpy(ppdb->filename, filename);

    // take filename only (discard directory path)
    len = strlen(filename);
    for (i=len-1; i>=0; --i) {
        if (filename[i] == '/') {
            strcpy(fileonly, filename + (i+1));
            break;
        }
        if (i == 0) { // there is no directory path, filename only
            strcpy(fileonly, filename);
        }
    }

    sprintf(table_name, "table:%s", fileonly);
    if (indexing_type == 1) {
        // lsm-tree
        sprintf(table_config,
                "type=lsm,split_pct=100,leaf_item_max=1KB,"
                "internal_page_max=4KB,leaf_page_max=4KB,"
                "lsm=(chunk_size=4MB,"
                     "bloom_config=(leaf_page_max=4MB))");
    } else
        sprintf(table_config,
                "split_pct=100,leaf_item_max=1KB,"
                "internal_page_max=4KB,leaf_page_max=4KB");

    conn->open_session(conn, NULL, NULL, &ppdb->session);
    ppdb->session->create(ppdb->session, table_name, table_config);
    ppdb->session->open_cursor(ppdb->session, table_name, NULL, NULL, &ppdb->cursor);
    ppdb->sync = 1;

    return COUCHSTORE_SUCCESS;
}

couchstore_error_t couchstore_set_sync(Db *db, int sync)
{
    db->sync = sync;
    return COUCHSTORE_SUCCESS;
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_close_db(Db *db)
{
    db->cursor->close(db->cursor);
    db->session->close(db->session, NULL);
    free(db->filename);
    free(db);

    return COUCHSTORE_SUCCESS;
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_db_info(Db *db, DbInfo* info)
{
    struct stat filestat;

    info->filename = db->filename;
    info->doc_count = 0;
    info->deleted_count = 0;
    info->header_position = 0;
    info->last_sequence = 0;

    stat(db->filename, &filestat);
    info->space_used = filestat.st_size;

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

    memcpy((uint8_t*)buf + offset, &docinfo->content_meta,
           sizeof(docinfo->content_meta));
    offset += sizeof(docinfo->content_meta);

    memcpy((uint8_t*)buf + offset, &docinfo->rev_meta.size,
           sizeof(docinfo->rev_meta.size));
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
    int ret;
    unsigned i;
    uint16_t metalen;
    uint8_t metabuf[METABUF_MAXLEN];
    uint8_t *buf;
    char *err = NULL;
    WT_ITEM item;

    ret = db->session->begin_transaction( db->session, NULL );
    assert(ret == 0);

    for (i=0;i<numdocs;++i){
        item.data = docs[i]->id.buf;
        item.size = docs[i]->id.size;
        db->cursor->set_key(db->cursor, &item);

        metalen = _docinfo_to_buf(infos[i], metabuf);
        buf = (uint8_t*)malloc(sizeof(metalen) + metalen + docs[i]->data.size);
        memcpy(buf + sizeof(metalen), metabuf, metalen);
        memcpy(buf, &metalen, sizeof(metalen));
        memcpy(buf + sizeof(metalen) + metalen, docs[i]->data.buf, docs[i]->data.size);

        item.data = buf;
        item.size = sizeof(metalen) + metalen + docs[i]->data.size;
        db->cursor->set_value(db->cursor, &item);

        ret = db->cursor->insert(db->cursor);
        if (ret != 0) {
            printf("ERR\n");
        }

        infos[i]->db_seq = 0;
        free(buf);
    }

    ret = db->session->commit_transaction( db->session, db->sync ? "sync" : NULL );
    assert(ret == 0);

    return COUCHSTORE_SUCCESS;
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

    memcpy(&docinfo->content_meta, (uint8_t*)buf + offset,
           sizeof(docinfo->content_meta));
    offset += sizeof(docinfo->content_meta);

    memcpy(&docinfo->rev_meta.size, (uint8_t*)buf + offset,
           sizeof(docinfo->rev_meta.size));
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
couchstore_error_t couchstore_open_document(Db *db,
                                            const void *id,
                                            size_t idlen,
                                            Doc **pDoc,
                                            couchstore_open_options options)
{
    int ret;
    char *err = NULL;
    void *value;
    size_t valuelen;
    size_t rev_meta_size;
    size_t meta_offset;
    WT_ITEM item;

    item.data = id;
    item.size = idlen;
    db->cursor->set_key(db->cursor, &item);
    ret = db->cursor->search(db->cursor);
    assert(ret == 0);

    meta_offset = sizeof(uint64_t)*1 + sizeof(int) +
                  sizeof(couchstore_content_meta_flags);

    db->cursor->get_value(db->cursor, &item);

    *pDoc = (Doc *)malloc(sizeof(Doc));
    (*pDoc)->id.buf = (char*)id;
    (*pDoc)->id.size = idlen;
    (*pDoc)->data.buf = (char*)malloc(item.size);
    memcpy((*pDoc)->data.buf, item.data, item.size);
    (*pDoc)->data.size = item.size;

    return COUCHSTORE_SUCCESS;
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
    return COUCHSTORE_SUCCESS;
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_compact_db_ex(Db* source, const char* target_filename,
        uint64_t flags, const couch_file_ops *ops)
{
    return COUCHSTORE_SUCCESS;
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_compact_db(Db* source, const char* target_filename)
{
    return couchstore_compact_db_ex(source, target_filename, 0x0, NULL);
}

