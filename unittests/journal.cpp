/**
 * Copyright (C) 2005-2008 Christoph Rupp (chris@crupp.de).
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or 
 * (at your option) any later version.
 *
 * See files COPYING.* for License information.
 */

#include "../src/config.h"

#include <stdexcept>
#include <cstring>
#include <sstream>

#include "../src/internal_fwd_decl.h"
#include "../src/txn.h"
#include "../src/journal.h"
#include "../src/os.h"
#include "../src/db.h"
#include "../src/device.h"
#include "../src/env.h"
#include "../src/btree.h"
#include "../src/btree_key.h"
#include "../src/freelist.h"
#include "../src/cache.h"
#include "memtracker.h"
#include "os.hpp"

#include "bfc-testsuite.hpp"
#include "hamster_fixture.hpp"

using namespace bfc;

struct LogEntry
{
    LogEntry()
    :   lsn(0), txn_id(0), type(0), dbname(0)
    {
    }

    LogEntry(ham_u64_t _lsn, ham_u64_t _txn_id, 
                ham_u32_t _type, ham_u16_t _dbname)
    :   lsn(_lsn), txn_id(_txn_id), type(_type), dbname(_dbname)
    {
    }

    ham_u64_t lsn;
    ham_u64_t txn_id;
    ham_u32_t type;
    ham_u16_t dbname;
};

struct InsertLogEntry : public LogEntry
{
    InsertLogEntry(ham_u64_t _lsn, ham_u64_t _txn_id, ham_u16_t _dbname, 
                ham_key_t *_key, ham_record_t *_record)
    : LogEntry(_lsn, _txn_id, JOURNAL_ENTRY_TYPE_INSERT, _dbname),
        key(_key), record(_record)
    {
    }

    ham_key_t *key;
    ham_record_t *record;
};

struct EraseLogEntry : public LogEntry
{
    EraseLogEntry(ham_u64_t _lsn, ham_u64_t _txn_id, ham_u16_t _dbname, 
                ham_key_t *_key)
    : LogEntry(_lsn, _txn_id, JOURNAL_ENTRY_TYPE_INSERT, _dbname),
        key(_key)
    {
    }

    ham_key_t *key;
};

class JournalTest : public hamsterDB_fixture
{
    define_super(hamsterDB_fixture);

public:
    JournalTest()
        : hamsterDB_fixture("JournalTest")
    {
        testrunner::get_instance()->register_fixture(this);
        BFC_REGISTER_TEST(JournalTest, structHeaderTest);
        BFC_REGISTER_TEST(JournalTest, structEntryTest);
        BFC_REGISTER_TEST(JournalTest, structJournalTest);
        BFC_REGISTER_TEST(JournalTest, createCloseTest);
        BFC_REGISTER_TEST(JournalTest, createCloseOpenCloseTest);
        BFC_REGISTER_TEST(JournalTest, negativeCreateTest);
        BFC_REGISTER_TEST(JournalTest, negativeOpenTest);
        BFC_REGISTER_TEST(JournalTest, appendTxnBeginTest);
        BFC_REGISTER_TEST(JournalTest, appendTxnAbortTest);
        BFC_REGISTER_TEST(JournalTest, appendTxnCommitTest);
        BFC_REGISTER_TEST(JournalTest, appendInsertTest);
        BFC_REGISTER_TEST(JournalTest, appendEraseTest);
        BFC_REGISTER_TEST(JournalTest, clearTest);
        BFC_REGISTER_TEST(JournalTest, iterateOverEmptyLogTest);
        BFC_REGISTER_TEST(JournalTest, iterateOverLogOneEntryTest);
        BFC_REGISTER_TEST(JournalTest, iterateOverLogMultipleEntryTest);
        BFC_REGISTER_TEST(JournalTest, iterateOverLogMultipleEntrySwapTest);
        BFC_REGISTER_TEST(JournalTest, iterateOverLogMultipleEntrySwapTwiceTest);
        BFC_REGISTER_TEST(JournalTest, recoverVerifyTxnIdsTest);
        BFC_REGISTER_TEST(JournalTest, recoverCommittedTxnsTest);
        BFC_REGISTER_TEST(JournalTest, recoverAutoAbortTxnsTest);
        BFC_REGISTER_TEST(JournalTest, recoverSkipAlreadyFlushedTest);
        BFC_REGISTER_TEST(JournalTest, recoverInsertTest);
        BFC_REGISTER_TEST(JournalTest, recoverEraseTest);
        BFC_REGISTER_TEST(JournalTest, lsnOverflowTest);
    }

protected:
    ham_db_t *m_db;
    ham_env_t *m_env;
    memtracker_t *m_alloc;

public:
    virtual void setup() 
    { 
        __super::setup();

        (void)os::unlink(BFC_OPATH(".test"));

        m_alloc=memtracker_new();
        BFC_ASSERT_EQUAL(0, ham_new(&m_db));
        BFC_ASSERT_EQUAL(0, ham_create(m_db, BFC_OPATH(".test"), 
                        HAM_ENABLE_DUPLICATES
                        |HAM_ENABLE_TRANSACTIONS
                        |HAM_ENABLE_RECOVERY, 0644));
    
        m_env=db_get_env(m_db);
    }
    
    virtual void teardown() 
    { 
        __super::teardown();

        BFC_ASSERT_EQUAL(0, ham_close(m_db, 0));
        ham_delete(m_db);
        BFC_ASSERT_EQUAL((unsigned long)0, memtracker_get_leaks(m_alloc));
    }

    journal_t *disconnect_and_create_new_journal(void)
    {
        journal_t *log;
        ham_env_t *env=db_get_env(m_db);

        BFC_ASSERT_EQUAL(HAM_WOULD_BLOCK, 
            journal_create(env, 0644, 0, &log));
        BFC_ASSERT_NULL(log);

        /* 
         * make sure db->journal is already NULL, i.e. disconnected. 
         * Otherwise an BFC ASSERT for journal_close() will segfault 
         * the teardown() code, which will try to close the db->journal 
         * all over AGAIN! 
         */
        log = env_get_journal(env);
        env_set_journal(env, NULL);
        BFC_ASSERT_EQUAL(0, journal_close(log, HAM_FALSE));
        BFC_ASSERT_EQUAL(0, 
            journal_create(env, 0644, 0, &log));
        BFC_ASSERT_NOTNULL(log);
        env_set_journal(env, log);
        return (log);
    }

    void structHeaderTest()
    {
        journal_header_t hdr;

        journal_header_set_magic(&hdr, 0x1234);
        BFC_ASSERT_EQUAL((ham_u32_t)0x1234, journal_header_get_magic(&hdr));

        journal_header_set_lsn(&hdr, 0x888ull);
        BFC_ASSERT_EQUAL((ham_u64_t)0x888ull, journal_header_get_lsn(&hdr));
    }

    void structEntryTest()
    {
        journal_entry_t e;

        journal_entry_set_lsn(&e, 0x13);
        BFC_ASSERT_EQUAL((ham_u64_t)0x13, journal_entry_get_lsn(&e));

        journal_entry_set_txn_id(&e, 0x15);
        BFC_ASSERT_EQUAL((ham_u64_t)0x15, journal_entry_get_txn_id(&e));

        journal_entry_set_followup_size(&e, 0x16);
        BFC_ASSERT_EQUAL((ham_u64_t)0x16, journal_entry_get_followup_size(&e));
        journal_entry_set_followup_size(&e, 0);

        journal_entry_set_flags(&e, 0xff000000);
        BFC_ASSERT_EQUAL((ham_u32_t)0xff000000, journal_entry_get_flags(&e));

        journal_entry_set_dbname(&e, 99);
        BFC_ASSERT_EQUAL((ham_u16_t)99, journal_entry_get_dbname(&e));

        journal_entry_set_type(&e, JOURNAL_ENTRY_TYPE_INSERT);
        BFC_ASSERT_EQUAL((ham_u32_t)JOURNAL_ENTRY_TYPE_INSERT, 
                journal_entry_get_type(&e));
    }

    void structJournalTest(void)
    {
        journal_t log;

        BFC_ASSERT_NOTNULL(env_get_journal(m_env));

        journal_set_allocator(&log, (mem_allocator_t *)m_alloc);
        BFC_ASSERT_EQUAL((mem_allocator_t *)m_alloc, 
                        journal_get_allocator(&log));

        journal_set_current_fd(&log, 1);
        BFC_ASSERT_EQUAL((unsigned)1, journal_get_current_fd(&log));

        journal_set_fd(&log, 0, (ham_fd_t)0x20);
        BFC_ASSERT_EQUAL((ham_fd_t)0x20, journal_get_fd(&log, 0));
        journal_set_fd(&log, 1, (ham_fd_t)0x21);
        BFC_ASSERT_EQUAL((ham_fd_t)0x21, journal_get_fd(&log, 1));

        journal_set_lsn(&log, 0x99);
        BFC_ASSERT_EQUAL((ham_u64_t)0x99, journal_get_lsn(&log));

        journal_set_last_checkpoint_lsn(&log, 0x100);
        BFC_ASSERT_EQUAL((ham_u64_t)0x100, 
                        journal_get_last_checkpoint_lsn(&log));

        for (int i=0; i<2; i++) {
            journal_set_open_txn(&log, i, 0x15+i);
            BFC_ASSERT_EQUAL((ham_size_t)0x15+i, 
                    journal_get_open_txn(&log, i));
            journal_set_closed_txn(&log, i, 0x25+i);
            BFC_ASSERT_EQUAL((ham_size_t)0x25+i, 
                    journal_get_closed_txn(&log, i));
        }
    }

    void createCloseTest(void)
    {
        ham_bool_t isempty;
        journal_t *log = disconnect_and_create_new_journal();

        BFC_ASSERT_EQUAL((ham_offset_t)1, journal_get_lsn(log));
        /* TODO make sure that the two files exist and 
         * contain only the header */

        BFC_ASSERT_EQUAL(0, journal_is_empty(log, &isempty));
        BFC_ASSERT_EQUAL(1, isempty);

        // do not close the journal - it will be closed in teardown()
    }

    void createCloseOpenCloseTest(void)
    {
        ham_bool_t isempty;
        journal_t *log=env_get_journal(m_env);
        BFC_ASSERT_EQUAL(0, journal_is_empty(log, &isempty));
        BFC_ASSERT_EQUAL(1, isempty);
        BFC_ASSERT_EQUAL(0, journal_close(log, HAM_TRUE));

        BFC_ASSERT_EQUAL(0, journal_open(m_env, 0, &log));
        BFC_ASSERT(log!=0);
        BFC_ASSERT_EQUAL(0, journal_is_empty(log, &isempty));
        BFC_ASSERT_EQUAL(1, isempty);
        env_set_journal(m_env, log);
    }

    void negativeCreateTest(void)
    {
        journal_t *log;
        const char *oldfilename=env_get_filename(m_env);
        env_set_filename(m_env, "/::asdf");
        BFC_ASSERT_EQUAL(HAM_IO_ERROR, 
                journal_create(m_env, 0644, 0, &log));
        env_set_filename(m_env, oldfilename);
        BFC_ASSERT_EQUAL((journal_t *)0, log);
    }

    void negativeOpenTest(void)
    {
        ham_fd_t fd;
        journal_t *log;
        const char *oldfilename=env_get_filename(m_env);
        env_set_filename(m_env, "xxx$$test");
        BFC_ASSERT_EQUAL(HAM_FILE_NOT_FOUND, 
                    journal_open(m_env, 0, &log));
        BFC_ASSERT_EQUAL((journal_t *)0, log);

        /* if journal_open() fails, it will call journal_close() internally and 
         * journal_close() overwrites the header structure. therefore we have
         * to patch the file before we start the test. */
        BFC_ASSERT_EQUAL(0, os_open("data/log-broken-magic.jrn0", 0, &fd));
        BFC_ASSERT_EQUAL(0, os_pwrite(fd, 0, (void *)"x", 1));
        BFC_ASSERT_EQUAL(0, os_close(fd, 0));

        env_set_filename(m_env, "data/log-broken-magic");
        BFC_ASSERT_EQUAL(HAM_LOG_INV_FILE_HEADER, 
                    journal_open(m_env,  0, &log));
        env_set_filename(m_env, oldfilename);
        BFC_ASSERT_EQUAL((journal_t *)0, log);
    }

    void appendTxnBeginTest(void)
    {
        ham_bool_t isempty;
        journal_t *log = disconnect_and_create_new_journal();
        BFC_ASSERT_EQUAL(0, journal_is_empty(log, &isempty));
        BFC_ASSERT_EQUAL(1, isempty);

        BFC_ASSERT_EQUAL((ham_size_t)0, journal_get_open_txn(log, 0));
        BFC_ASSERT_EQUAL((ham_size_t)0, journal_get_closed_txn(log, 0));
        BFC_ASSERT_EQUAL((ham_size_t)0, journal_get_open_txn(log, 1));
        BFC_ASSERT_EQUAL((ham_size_t)0, journal_get_closed_txn(log, 1));

        ham_txn_t *txn;
        BFC_ASSERT_EQUAL(0, ham_txn_begin(&txn, m_db, 0));

        BFC_ASSERT_EQUAL((ham_size_t)1, journal_get_open_txn(log, 0));
        BFC_ASSERT_EQUAL((ham_size_t)0, journal_get_closed_txn(log, 0));
        BFC_ASSERT_EQUAL((ham_size_t)0, journal_get_open_txn(log, 1));
        BFC_ASSERT_EQUAL((ham_size_t)0, journal_get_closed_txn(log, 1));

        BFC_ASSERT_EQUAL(0, journal_is_empty(log, &isempty));
        BFC_ASSERT_EQUAL(0, isempty);
        BFC_ASSERT_EQUAL((ham_u64_t)2, journal_get_lsn(log));

        BFC_ASSERT_EQUAL(0, ham_txn_abort(txn, 0));
    }

    void appendTxnAbortTest(void)
    {
        ham_bool_t isempty;
        journal_t *log = disconnect_and_create_new_journal();
        BFC_ASSERT_EQUAL(0, journal_is_empty(log, &isempty));
        BFC_ASSERT_EQUAL(1, isempty);

        ham_txn_t *txn;
        BFC_ASSERT_EQUAL(0, ham_txn_begin(&txn, m_db, 0));
        BFC_ASSERT_EQUAL(0, journal_is_empty(log, &isempty));
        BFC_ASSERT_EQUAL(0, isempty);
        BFC_ASSERT_EQUAL((ham_u64_t)2, journal_get_lsn(log));
        BFC_ASSERT_EQUAL((ham_size_t)1, journal_get_open_txn(log, 0));
        BFC_ASSERT_EQUAL((ham_size_t)0, journal_get_closed_txn(log, 0));
        BFC_ASSERT_EQUAL((ham_size_t)0, journal_get_open_txn(log, 1));
        BFC_ASSERT_EQUAL((ham_size_t)0, journal_get_closed_txn(log, 1));

        ham_u64_t lsn;
        BFC_ASSERT_EQUAL(0, env_get_incremented_lsn(m_env, &lsn));
        BFC_ASSERT_EQUAL(0, journal_append_txn_abort(log, txn, lsn));
        BFC_ASSERT_EQUAL(0, journal_is_empty(log, &isempty));
        BFC_ASSERT_EQUAL(0, isempty);
        BFC_ASSERT_EQUAL((ham_u64_t)3, journal_get_lsn(log));
        BFC_ASSERT_EQUAL((ham_size_t)0, journal_get_open_txn(log, 0));
        BFC_ASSERT_EQUAL((ham_size_t)1, journal_get_closed_txn(log, 0));
        BFC_ASSERT_EQUAL((ham_size_t)0, journal_get_open_txn(log, 1));
        BFC_ASSERT_EQUAL((ham_size_t)0, journal_get_closed_txn(log, 1));

        BFC_ASSERT_EQUAL(0, ham_txn_abort(txn, 0));
    }

    void appendTxnCommitTest(void)
    {
        ham_bool_t isempty;
        journal_t *log = disconnect_and_create_new_journal();
        BFC_ASSERT_EQUAL(0, journal_is_empty(log, &isempty));
        BFC_ASSERT_EQUAL(1, isempty);

        ham_txn_t *txn;
        BFC_ASSERT_EQUAL(0, ham_txn_begin(&txn, m_db, 0));
        BFC_ASSERT_EQUAL(0, journal_is_empty(log, &isempty));
        BFC_ASSERT_EQUAL(0, isempty);
        BFC_ASSERT_EQUAL((ham_u64_t)2, journal_get_lsn(log));
        BFC_ASSERT_EQUAL((ham_size_t)1, journal_get_open_txn(log, 0));
        BFC_ASSERT_EQUAL((ham_size_t)0, journal_get_closed_txn(log, 0));
        BFC_ASSERT_EQUAL((ham_size_t)0, journal_get_open_txn(log, 1));
        BFC_ASSERT_EQUAL((ham_size_t)0, journal_get_closed_txn(log, 1));

        ham_u64_t lsn;
        BFC_ASSERT_EQUAL(0, env_get_incremented_lsn(m_env, &lsn));
        BFC_ASSERT_EQUAL(0, journal_append_txn_commit(log, txn, lsn));
        BFC_ASSERT_EQUAL(0, journal_is_empty(log, &isempty));
        BFC_ASSERT_EQUAL(0, isempty);
        BFC_ASSERT_EQUAL((ham_u64_t)3, journal_get_lsn(log));
        BFC_ASSERT_EQUAL((ham_size_t)0, journal_get_open_txn(log, 0));
        BFC_ASSERT_EQUAL((ham_size_t)1, journal_get_closed_txn(log, 0));
        BFC_ASSERT_EQUAL((ham_size_t)0, journal_get_open_txn(log, 1));
        BFC_ASSERT_EQUAL((ham_size_t)0, journal_get_closed_txn(log, 1));

        BFC_ASSERT_EQUAL(0, ham_txn_abort(txn, 0));
    }

    void appendInsertTest(void)
    {
        journal_t *log = disconnect_and_create_new_journal();
        ham_txn_t *txn;
        ham_key_t key={0};
        ham_record_t rec={0};
        key.data=(void *)"key1";
        key.size=5;
        rec.data=(void *)"rec1";
        rec.size=5;
        BFC_ASSERT_EQUAL(0, ham_txn_begin(&txn, m_db, 0));

        ham_u64_t lsn;
        BFC_ASSERT_EQUAL(0, env_get_incremented_lsn(m_env, &lsn));
        BFC_ASSERT_EQUAL(0, 
                    journal_append_insert(log, m_db, txn, &key, &rec, 
                                HAM_OVERWRITE, lsn));
        BFC_ASSERT_EQUAL((ham_u64_t)3, journal_get_lsn(log));
        BFC_ASSERT_EQUAL(0, journal_close(log, HAM_TRUE));

        BFC_ASSERT_EQUAL(0, 
                journal_open(m_env, 0, &log));
        env_set_journal(m_env, log);
        BFC_ASSERT(log!=0);

        /* verify that the insert entry was written correctly */
        journal_iterator_t iter;
        memset(&iter, 0, sizeof(iter));
        journal_entry_t entry;
        journal_entry_insert_t *ins;
        BFC_ASSERT_EQUAL(0,  // this is the txn
                    journal_get_entry(log, &iter, &entry, (void **)&ins));
        BFC_ASSERT_EQUAL(0,  // this is the insert
                    journal_get_entry(log, &iter, &entry, (void **)&ins));
        BFC_ASSERT_EQUAL((ham_u64_t)2, journal_entry_get_lsn(&entry));
        BFC_ASSERT_EQUAL(5, journal_entry_insert_get_key_size(ins));
        BFC_ASSERT_EQUAL(5u, journal_entry_insert_get_record_size(ins));
        BFC_ASSERT_EQUAL(0ull, 
                    journal_entry_insert_get_record_partial_size(ins));
        BFC_ASSERT_EQUAL(0ull, 
                    journal_entry_insert_get_record_partial_offset(ins));
        BFC_ASSERT_EQUAL((unsigned)HAM_OVERWRITE, 
                    journal_entry_insert_get_flags(ins));
        BFC_ASSERT_EQUAL(0, 
                    strcmp("key1", 
                            (char *)journal_entry_insert_get_key_data(ins)));
        BFC_ASSERT_EQUAL(0, 
                    strcmp("rec1", 
                            (char *)journal_entry_insert_get_record_data(ins)));

        allocator_free(journal_get_allocator(log), ins);

        BFC_ASSERT_EQUAL(0, ham_txn_abort(txn, 0));
    }

    void appendEraseTest(void)
    {
        journal_t *log = disconnect_and_create_new_journal();
        ham_txn_t *txn;
        ham_key_t key={0};
        key.data=(void *)"key1";
        key.size=5;
        BFC_ASSERT_EQUAL(0, ham_txn_begin(&txn, m_db, 0));

        ham_u64_t lsn;
        BFC_ASSERT_EQUAL(0, env_get_incremented_lsn(m_env, &lsn));
        BFC_ASSERT_EQUAL(0, 
                    journal_append_erase(log, m_db, txn, &key, 1, 0, lsn));
        BFC_ASSERT_EQUAL((ham_u64_t)3, journal_get_lsn(log));
        BFC_ASSERT_EQUAL(0, journal_close(log, HAM_TRUE));

        BFC_ASSERT_EQUAL(0, 
                journal_open(m_env, 0, &log));
        env_set_journal(m_env, log);
        BFC_ASSERT(log!=0);

        /* verify that the erase entry was written correctly */
        journal_iterator_t iter;
        memset(&iter, 0, sizeof(iter));
        journal_entry_t entry;
        journal_entry_erase_t *er;
        BFC_ASSERT_EQUAL(0, // this is the txn
                    journal_get_entry(log, &iter, &entry, (void **)&er));
        BFC_ASSERT_EQUAL(0, // this is the erase
                    journal_get_entry(log, &iter, &entry, (void **)&er));
        BFC_ASSERT_EQUAL((ham_u64_t)2, journal_entry_get_lsn(&entry));
        BFC_ASSERT_EQUAL(5, journal_entry_erase_get_key_size(er));
        BFC_ASSERT_EQUAL(0u, journal_entry_erase_get_flags(er));
        BFC_ASSERT_EQUAL(1u, journal_entry_erase_get_dupe(er));
        BFC_ASSERT_EQUAL(0, 
                    strcmp("key1", 
                            (char *)journal_entry_erase_get_key_data(er)));

        allocator_free(journal_get_allocator(log), er);

        BFC_ASSERT_EQUAL(0, ham_txn_abort(txn, 0));
    }

    void clearTest(void)
    {
        ham_bool_t isempty;
        journal_t *log = disconnect_and_create_new_journal();
        BFC_ASSERT_EQUAL(0, journal_is_empty(log, &isempty));
        BFC_ASSERT_EQUAL(1, isempty);

        ham_txn_t *txn;
        BFC_ASSERT_EQUAL(0, ham_txn_begin(&txn, m_db, 0));

        BFC_ASSERT_EQUAL(0, journal_is_empty(log, &isempty));
        BFC_ASSERT_EQUAL(0, isempty);
        BFC_ASSERT_EQUAL((ham_u64_t)2, journal_get_lsn(log));

        BFC_ASSERT_EQUAL(0, journal_clear(log));
        BFC_ASSERT_EQUAL(0, journal_is_empty(log, &isempty));
        BFC_ASSERT_EQUAL(1, isempty);
        BFC_ASSERT_EQUAL((ham_u64_t)2, journal_get_lsn(log));

        BFC_ASSERT_EQUAL(0, ham_txn_abort(txn, 0));
        BFC_ASSERT_EQUAL((ham_u64_t)3, journal_get_lsn(log));

        BFC_ASSERT_EQUAL(0, journal_close(log, HAM_FALSE));
        BFC_ASSERT_EQUAL(0, 
                journal_open(m_env, 0, &log));
        BFC_ASSERT_EQUAL((ham_u64_t)3, journal_get_lsn(log));
        env_set_journal(m_env, log);
    }

    void iterateOverEmptyLogTest(void)
    {
        journal_t *log = disconnect_and_create_new_journal();

        journal_iterator_t iter;
        memset(&iter, 0, sizeof(iter));

        journal_entry_t entry;
        ham_u8_t *data;
        BFC_ASSERT_EQUAL(0, 
                    journal_get_entry(log, &iter, &entry, (void **)&data));
        BFC_ASSERT_EQUAL((ham_u64_t)0, journal_entry_get_lsn(&entry));
        BFC_ASSERT_EQUAL((ham_u8_t *)0, data);
    }

    void iterateOverLogOneEntryTest(void)
    {
        ham_txn_t *txn;
        journal_t *log = disconnect_and_create_new_journal();
        BFC_ASSERT_EQUAL(1ull, journal_get_lsn(log));
        BFC_ASSERT_EQUAL(0, ham_txn_begin(&txn, m_db, 0));
        ham_u64_t lsn;
        BFC_ASSERT_EQUAL(0, env_get_incremented_lsn(m_env, &lsn));
        BFC_ASSERT_EQUAL(0, journal_append_txn_begin(log, txn, m_db, lsn));
        BFC_ASSERT_EQUAL(0, journal_close(log, HAM_TRUE));

        BFC_ASSERT_EQUAL(0, 
                journal_open(m_env, 0, &log));
        BFC_ASSERT_EQUAL(2ull, journal_get_lsn(log));
        env_set_journal(m_env, log);
        BFC_ASSERT(log!=0);

        journal_iterator_t iter;
        memset(&iter, 0, sizeof(iter));

        journal_entry_t entry;
        ham_u8_t *data;
        BFC_ASSERT_EQUAL(0, 
                    journal_get_entry(log, &iter, &entry, (void **)&data));
        BFC_ASSERT_EQUAL((ham_u64_t)1, journal_entry_get_lsn(&entry));
        BFC_ASSERT_EQUAL((ham_u64_t)1, txn_get_id(txn));
        BFC_ASSERT_EQUAL((ham_u64_t)1, journal_entry_get_txn_id(&entry));
        BFC_ASSERT_EQUAL((ham_u8_t *)0, data);
        BFC_ASSERT_EQUAL((ham_u32_t)JOURNAL_ENTRY_TYPE_TXN_BEGIN, 
                        journal_entry_get_type(&entry));

        BFC_ASSERT_EQUAL(0, ham_txn_abort(txn, 0));
    }

    void checkJournalEntry(journal_entry_t *entry, ham_u64_t lsn, 
                    ham_u64_t txn_id, ham_u32_t type, ham_u8_t *data)
    {
        BFC_ASSERT_EQUAL(lsn, journal_entry_get_lsn(entry));
        BFC_ASSERT_EQUAL(txn_id, journal_entry_get_txn_id(entry));
        if (!journal_entry_get_followup_size(entry)) {
            BFC_ASSERT_NULL(data);
        }
        else {
            BFC_ASSERT_NOTNULL(data);
            allocator_free((mem_allocator_t *)m_alloc, data);
        }
        BFC_ASSERT_EQUAL(type, journal_entry_get_type(entry));
    }

    void compareJournal(journal_t *journal, LogEntry *vec, unsigned size)
    {
        journal_iterator_t it={0};
        journal_entry_t entry={0};
        void *aux;
        unsigned s=0;

        do {
            BFC_ASSERT_EQUAL(0, journal_get_entry(journal, &it, &entry, &aux));
            if (journal_entry_get_lsn(&entry)==0)
                break;

            if (s==size) {
                BFC_ASSERT_EQUAL(0ull, journal_entry_get_lsn(&entry));
                break;
            }

#if 0
            std::cout << "vector: lsn=" << (*vit).lsn << "; txn=" 
                      << (*vit).txn_id << "; type=" << (*vit).type 
                      << "; dbname=" << (*vit).dbname << std::endl;
            std::cout << "journl: lsn=" << journal_entry_get_lsn(&entry) 
                      << "; txn=" << journal_entry_get_txn_id(&entry)
                      << "; type=" << journal_entry_get_type(&entry)
                      << "; dbname=" << journal_entry_get_dbname(&entry) 
                      << std::endl
                      << std::endl;
#endif

            s++;

            BFC_ASSERT_EQUAL(vec->lsn, journal_entry_get_lsn(&entry));
            BFC_ASSERT_EQUAL(vec->txn_id, journal_entry_get_txn_id(&entry));
            BFC_ASSERT_EQUAL(vec->type, journal_entry_get_type(&entry));
            BFC_ASSERT_EQUAL(vec->dbname, journal_entry_get_dbname(&entry));

            if (aux)
                allocator_free(journal_get_allocator(journal), aux);

            vec++;

        } while (1);

        if (aux)
            allocator_free(journal_get_allocator(journal), aux);
        BFC_ASSERT_EQUAL(s, size);
    }

    void iterateOverLogMultipleEntryTest(void)
    {
        ham_txn_t *txn;
        journal_t *log=env_get_journal(m_env);
        unsigned p=0;

        LogEntry vec[20];
        for (int i=0; i<5; i++) {
            // ham_txn_begin and ham_txn_abort will automatically add a 
            // journal entry
            BFC_ASSERT_EQUAL(0, ham_txn_begin(&txn, m_db, 0));
            vec[p++]=LogEntry(2+i*2, txn_get_id(txn), 
                            JOURNAL_ENTRY_TYPE_TXN_BEGIN, 0xf000);
            BFC_ASSERT_EQUAL(0, ham_txn_abort(txn, 0));
            vec[p++]=LogEntry(3+i*2, txn_get_id(txn), 
                            JOURNAL_ENTRY_TYPE_TXN_ABORT, 0);
        }

        BFC_ASSERT_EQUAL(0, ham_close(m_db, HAM_DONT_CLEAR_LOG));

        BFC_ASSERT_EQUAL(0, 
                ham_open(m_db, BFC_OPATH(".test"), 0));
        m_env=db_get_env(m_db);
        BFC_ASSERT_EQUAL(0, 
                journal_open(m_env, 0, &log));
        env_set_journal(m_env, log);
        BFC_ASSERT(log!=0);

        compareJournal(log, vec, p);

        BFC_ASSERT_EQUAL(0, ham_close(m_db, 0));
    }

    void iterateOverLogMultipleEntrySwapTest(void)
    {
        ham_txn_t *txn;
        journal_t *log=env_get_journal(m_env);
        journal_set_threshold(log, 5);
        unsigned p=0;
        LogEntry vec[20];

        for (int i=0; i<=7; i++) {
            BFC_ASSERT_EQUAL(0, ham_txn_begin(&txn, m_db, 0));
            vec[p++]=LogEntry(2+i*2, txn_get_id(txn), 
                            JOURNAL_ENTRY_TYPE_TXN_BEGIN, 0xf000);
            BFC_ASSERT_EQUAL(0, ham_txn_abort(txn, 0));
            vec[p++]=LogEntry(3+i*2, txn_get_id(txn), 
                            JOURNAL_ENTRY_TYPE_TXN_ABORT, 0);
        }

        BFC_ASSERT_EQUAL(0, ham_close(m_db, HAM_DONT_CLEAR_LOG));

        BFC_ASSERT_EQUAL(0, 
                ham_open(m_db, BFC_OPATH(".test"), 0));
        m_env=db_get_env(m_db);
        BFC_ASSERT_EQUAL(0, 
                journal_open(m_env, 0, &log));
        env_set_journal(m_env, log);
        BFC_ASSERT(log!=0);

        compareJournal(log, vec, p);

        BFC_ASSERT_EQUAL(0, ham_close(m_db, 0));
    }

    void iterateOverLogMultipleEntrySwapTwiceTest(void)
    {
        ham_txn_t *txn;
        journal_t *log=env_get_journal(m_env);
        journal_set_threshold(log, 5);

        unsigned p=0;
        LogEntry vec[20];

        for (int i=0; i<=10; i++) {
            BFC_ASSERT_EQUAL(0, ham_txn_begin(&txn, m_db, 0));
            if (i>=5)
                vec[p++]=LogEntry(2+i*2, txn_get_id(txn), 
                            JOURNAL_ENTRY_TYPE_TXN_BEGIN, 0xf000);
            BFC_ASSERT_EQUAL(0, ham_txn_abort(txn, 0));
            if (i>=5)
                vec[p++]=LogEntry(3+i*2, txn_get_id(txn), 
                            JOURNAL_ENTRY_TYPE_TXN_ABORT, 0);
        }

        BFC_ASSERT_EQUAL(0, ham_close(m_db, HAM_DONT_CLEAR_LOG));

        BFC_ASSERT_EQUAL(0, 
                ham_open(m_db, BFC_OPATH(".test"), 0));
        m_env=db_get_env(m_db);
        BFC_ASSERT_EQUAL(0, 
                journal_open(m_env, 0, &log));
        env_set_journal(m_env, log);
        BFC_ASSERT(log!=0);

        compareJournal(log, vec, p);

        BFC_ASSERT_EQUAL(0, ham_close(m_db, 0));
    }

    void verifyJournalIsEmpty(void)
    {
        ham_offset_t size;
        journal_t *j=env_get_journal(m_env);
        BFC_ASSERT_EQUAL(0, os_get_filesize(journal_get_fd(j, 0), &size));
        BFC_ASSERT_EQUAL(sizeof(journal_header_t), size);
        BFC_ASSERT_EQUAL(0, os_get_filesize(journal_get_fd(j, 1), &size));
        BFC_ASSERT_EQUAL(sizeof(journal_header_t), size);
    }

    void recoverVerifyTxnIdsTest(void)
    {
        ham_txn_t *txn;
        LogEntry vec[20];
        unsigned p=0;

        for (int i=0; i<5; i++) {
            BFC_ASSERT_EQUAL(0, ham_txn_begin(&txn, m_db, 0));
            BFC_ASSERT_EQUAL((ham_u64_t)i+1, txn_get_id(txn));
            vec[p++]=LogEntry(2+i*2, txn_get_id(txn), 
                        JOURNAL_ENTRY_TYPE_TXN_BEGIN, 0xf000);
            ham_u64_t txnid=txn_get_id(txn);
            BFC_ASSERT_EQUAL(0, ham_txn_commit(txn, 0));
            vec[p++]=LogEntry(3+i*2, txnid,
                        JOURNAL_ENTRY_TYPE_TXN_COMMIT, 0);
        }

        BFC_ASSERT_EQUAL(0, ham_close(m_db, HAM_DONT_CLEAR_LOG));

        /* reopen the database */
        BFC_ASSERT_EQUAL(HAM_NEED_RECOVERY, 
                ham_open(m_db, BFC_OPATH(".test"), 
                        HAM_ENABLE_TRANSACTIONS|HAM_ENABLE_RECOVERY));
        BFC_ASSERT_EQUAL(0, 
                ham_open(m_db, BFC_OPATH(".test"), 
                        HAM_ENABLE_TRANSACTIONS|HAM_AUTO_RECOVERY));
        m_env=db_get_env(m_db);

        /* verify that the journal is empty */
        verifyJournalIsEmpty();

        /* verify the lsn */
        BFC_ASSERT_EQUAL(11ull, journal_get_lsn(env_get_journal(m_env)));
        BFC_ASSERT_EQUAL(5ull, env_get_txn_id(m_env));

        /* create another transaction and make sure that the transaction
         * IDs and the lsn's continue seamlessly */
        BFC_ASSERT_EQUAL(0, ham_txn_begin(&txn, m_db, 0));
        BFC_ASSERT_EQUAL(6ull, txn_get_id(txn));
        BFC_ASSERT_EQUAL(0, ham_txn_commit(txn, 0));
    }

    void recoverCommittedTxnsTest(void)
    {
        ham_txn_t *txn[5];
        LogEntry vec[20];
        unsigned p=0;
        ham_key_t key={0};
        ham_record_t rec={0};
        journal_t *log;
        ham_u64_t lsn=2;

        /* create a couple of transaction which insert a key, and commit 
         * them */
        for (int i=0; i<5; i++) {
            BFC_ASSERT_EQUAL(0, ham_txn_begin(&txn[i], m_db, 0));
            vec[p++]=LogEntry(lsn++, txn_get_id(txn[i]), 
                        JOURNAL_ENTRY_TYPE_TXN_BEGIN, 0xf000);
            key.data=&i;
            key.size=sizeof(i);
            BFC_ASSERT_EQUAL(0, ham_insert(m_db, txn[i], &key, &rec, 0));
            vec[p++]=LogEntry(lsn++, txn_get_id(txn[i]), 
                        JOURNAL_ENTRY_TYPE_INSERT, 0xf000);
            vec[p++]=LogEntry(lsn++, txn_get_id(txn[i]), 
                        JOURNAL_ENTRY_TYPE_TXN_COMMIT, 0);
            BFC_ASSERT_EQUAL(0, ham_txn_commit(txn[i], 0));
        }

        /* backup the journal files; then re-create the Environment from the 
         * journal */
        BFC_ASSERT_EQUAL(0, ham_close(m_db, HAM_DONT_CLEAR_LOG));
        BFC_ASSERT_EQUAL(0, ham_open(m_db, BFC_OPATH(".test"), 0));
        m_env=db_get_env(m_db);
        BFC_ASSERT_EQUAL(0, journal_open(m_env, 0, &log));
        env_set_journal(m_env, log);
        BFC_ASSERT(log!=0);
        compareJournal(log, vec, p);
        BFC_ASSERT_EQUAL(0, ham_close(m_db, HAM_DONT_CLEAR_LOG));
        BFC_ASSERT_EQUAL(0, 
                ham_open(m_db, BFC_OPATH(".test"), 
                        HAM_ENABLE_TRANSACTIONS|HAM_AUTO_RECOVERY));
        m_env=db_get_env(m_db);

        /* verify that the journal is empty */
        verifyJournalIsEmpty();

        /* now verify that the committed transactions were re-played from
         * the journal */
        for (int i=0; i<5; i++) {
            key.data=&i;
            key.size=sizeof(i);
            BFC_ASSERT_EQUAL(0, ham_find(m_db, 0, &key, &rec, 0));
        }
    }

    void recoverAutoAbortTxnsTest(void)
    {
#ifndef WIN32
        ham_txn_t *txn[5];
        LogEntry vec[20];
        unsigned p=0;
        ham_key_t key={0};
        ham_record_t rec={0};
        journal_t *log;
        ham_u64_t lsn=2;

        /* create a couple of transaction which insert a key, but do not 
         * commit them! */
        for (int i=0; i<5; i++) {
            BFC_ASSERT_EQUAL(0, ham_txn_begin(&txn[i], m_db, 0));
            vec[p++]=LogEntry(lsn++, txn_get_id(txn[i]), 
                        JOURNAL_ENTRY_TYPE_TXN_BEGIN, 0xf000);
            key.data=&i;
            key.size=sizeof(i);
            BFC_ASSERT_EQUAL(0, ham_insert(m_db, txn[i], &key, &rec, 0));
            vec[p++]=LogEntry(lsn++, txn_get_id(txn[i]), 
                        JOURNAL_ENTRY_TYPE_INSERT, 0xf000);
        }

        /* backup the journal files; then re-create the Environment from the 
         * journal */
        BFC_ASSERT_EQUAL(true, os::copy(BFC_OPATH(".test.jrn0"), 
                    BFC_OPATH(".test.bak0")));
        BFC_ASSERT_EQUAL(true, os::copy(BFC_OPATH(".test.jrn1"), 
                    BFC_OPATH(".test.bak1")));
        for (int i=0; i<5; i++)
            BFC_ASSERT_EQUAL(0, ham_txn_commit(txn[i], 0));
        BFC_ASSERT_EQUAL(0, ham_close(m_db, HAM_DONT_CLEAR_LOG));
        BFC_ASSERT_EQUAL(true, os::copy(BFC_OPATH(".test.bak0"), 
                    BFC_OPATH(".test.jrn0")));
        BFC_ASSERT_EQUAL(true, os::copy(BFC_OPATH(".test.bak1"), 
                    BFC_OPATH(".test.jrn1")));
        BFC_ASSERT_EQUAL(0, ham_open(m_db, BFC_OPATH(".test"), 0));
        m_env=db_get_env(m_db);
        BFC_ASSERT_EQUAL(0, journal_open(m_env, 0, &log));
        env_set_journal(m_env, log);
        BFC_ASSERT(log!=0);
        compareJournal(log, vec, p);
        BFC_ASSERT_EQUAL(0, ham_close(m_db, HAM_DONT_CLEAR_LOG));
        /* by re-creating the database we make sure that it's definitely
         * empty */
        BFC_ASSERT_EQUAL(0, ham_create(m_db, BFC_OPATH(".test"), 0, 0644));
        BFC_ASSERT_EQUAL(0, ham_close(m_db, 0));
        /* now open and recover */
        BFC_ASSERT_EQUAL(true, os::copy(BFC_OPATH(".test.bak0"), 
                    BFC_OPATH(".test.jrn0")));
        BFC_ASSERT_EQUAL(true, os::copy(BFC_OPATH(".test.bak1"), 
                    BFC_OPATH(".test.jrn1")));
        BFC_ASSERT_EQUAL(0, 
                ham_open(m_db, BFC_OPATH(".test"), 
                        HAM_ENABLE_TRANSACTIONS|HAM_AUTO_RECOVERY));
        m_env=db_get_env(m_db);

        /* verify that the journal is empty */
        verifyJournalIsEmpty();

        /* now verify that the transactions were actually aborted */
        for (int i=0; i<5; i++) {
            key.data=&i;
            key.size=sizeof(i);
            BFC_ASSERT_EQUAL(HAM_KEY_NOT_FOUND, 
                        ham_find(m_db, 0, &key, &rec, 0));
        }
#endif
    }

    void recoverSkipAlreadyFlushedTest(void)
    {
#ifndef WIN32
        ham_txn_t *txn[2];
        LogEntry vec[20];
        unsigned p=0;
        ham_key_t key={0};
        ham_record_t rec={0};
        journal_t *log;
        ham_u64_t lsn=2;

        /* create two transactions which insert a key, but only flush the
         * first; instead, manually append the "commit" of the second
         * transaction to the journal (but not to the database!) */
        for (int i=0; i<2; i++) {
            BFC_ASSERT_EQUAL(0, ham_txn_begin(&txn[i], m_db, 0));
            vec[p++]=LogEntry(lsn++, txn_get_id(txn[i]), 
                        JOURNAL_ENTRY_TYPE_TXN_BEGIN, 0xf000);
            key.data=&i;
            key.size=sizeof(i);
            BFC_ASSERT_EQUAL(0, ham_insert(m_db, txn[i], &key, &rec, 0));
            vec[p++]=LogEntry(lsn++, txn_get_id(txn[i]), 
                        JOURNAL_ENTRY_TYPE_INSERT, 0xf000);
            vec[p++]=LogEntry(lsn++, txn_get_id(txn[i]), 
                        JOURNAL_ENTRY_TYPE_TXN_COMMIT, 0);
            if (i==0)
                BFC_ASSERT_EQUAL(0, ham_txn_commit(txn[i], 0));
            else
                BFC_ASSERT_EQUAL(0, 
                    journal_append_txn_commit(env_get_journal(m_env), txn[i],
                            lsn-1));
        }

        /* backup the journal files; then re-create the Environment from the 
         * journal */
        BFC_ASSERT_EQUAL(true, os::copy(BFC_OPATH(".test.jrn0"), 
                    BFC_OPATH(".test.bak0")));
        BFC_ASSERT_EQUAL(true, os::copy(BFC_OPATH(".test.jrn1"), 
                    BFC_OPATH(".test.bak1")));
        BFC_ASSERT_EQUAL(0, ham_txn_commit(txn[1], 0));
        BFC_ASSERT_EQUAL(0, ham_close(m_db, HAM_DONT_CLEAR_LOG));
        BFC_ASSERT_EQUAL(true, os::copy(BFC_OPATH(".test.bak0"), 
                    BFC_OPATH(".test.jrn0")));
        BFC_ASSERT_EQUAL(true, os::copy(BFC_OPATH(".test.bak1"), 
                    BFC_OPATH(".test.jrn1")));
        BFC_ASSERT_EQUAL(0, ham_open(m_db, BFC_OPATH(".test"), 0));
        m_env=db_get_env(m_db);
        BFC_ASSERT_EQUAL(0, journal_open(m_env, 0, &log));
        env_set_journal(m_env, log);
        BFC_ASSERT(log!=0);
        compareJournal(log, vec, p);
        BFC_ASSERT_EQUAL(0, ham_close(m_db, HAM_DONT_CLEAR_LOG));
        /* now open and recover */
        BFC_ASSERT_EQUAL(true, os::copy(BFC_OPATH(".test.bak0"), 
                    BFC_OPATH(".test.jrn0")));
        BFC_ASSERT_EQUAL(true, os::copy(BFC_OPATH(".test.bak1"), 
                    BFC_OPATH(".test.jrn1")));
        BFC_ASSERT_EQUAL(0, 
                ham_open(m_db, BFC_OPATH(".test"), 
                        HAM_ENABLE_TRANSACTIONS|HAM_AUTO_RECOVERY));
        m_env=db_get_env(m_db);

        /* verify that the journal is empty */
        verifyJournalIsEmpty();

        /* now verify that the transactions were both committed */
        for (int i=0; i<2; i++) {
            key.data=&i;
            key.size=sizeof(i);
            BFC_ASSERT_EQUAL(0, ham_find(m_db, 0, &key, &rec, 0));
        }
#endif
    }

    void recoverInsertTest(void)
    {
        ham_txn_t *txn[2];
        LogEntry vec[200];
        unsigned p=0;
        ham_key_t key={0};
        ham_record_t rec={0};
        journal_t *log;
        ham_u64_t lsn=2;

        /* create two transactions with many keys that are inserted */
        for (int i=0; i<2; i++) {
            BFC_ASSERT_EQUAL(0, ham_txn_begin(&txn[i], m_db, 0));
            vec[p++]=LogEntry(lsn++, txn_get_id(txn[i]), 
                        JOURNAL_ENTRY_TYPE_TXN_BEGIN, 0xf000);
        }
        for (int i=0; i<100; i++) {
            key.data=&i;
            key.size=sizeof(i);
            BFC_ASSERT_EQUAL(0, ham_insert(m_db, txn[i&1], &key, &rec, 0));
            vec[p++]=LogEntry(lsn++, txn_get_id(txn[i&1]), 
                        JOURNAL_ENTRY_TYPE_INSERT, 0xf000);
        }
        /* commit the first txn, abort the second */
        vec[p++]=LogEntry(lsn++, txn_get_id(txn[0]), 
                    JOURNAL_ENTRY_TYPE_TXN_COMMIT, 0);
        BFC_ASSERT_EQUAL(0, ham_txn_commit(txn[0], 0));
        vec[p++]=LogEntry(lsn++, txn_get_id(txn[1]), 
                    JOURNAL_ENTRY_TYPE_TXN_ABORT, 0);
        BFC_ASSERT_EQUAL(0, ham_txn_abort(txn[1], 0));

        /* backup the journal files; then re-create the Environment from the 
         * journal */
        BFC_ASSERT_EQUAL(0, ham_close(m_db, HAM_DONT_CLEAR_LOG));
        BFC_ASSERT_EQUAL(0, ham_open(m_db, BFC_OPATH(".test"), 0));
        m_env=db_get_env(m_db);
        BFC_ASSERT_EQUAL(0, journal_open(m_env, 0, &log));
        env_set_journal(m_env, log);
        BFC_ASSERT(log!=0);
        compareJournal(log, vec, p);
        BFC_ASSERT_EQUAL(0, ham_close(m_db, HAM_DONT_CLEAR_LOG));
        BFC_ASSERT_EQUAL(0, 
                ham_open(m_db, BFC_OPATH(".test"), 
                        HAM_ENABLE_TRANSACTIONS|HAM_AUTO_RECOVERY));
        m_env=db_get_env(m_db);

        /* verify that the journal is empty */
        verifyJournalIsEmpty();

        /* now verify that the committed transaction was re-played from
         * the journal */
        for (int i=0; i<100; i++) {
            key.data=&i;
            key.size=sizeof(i);
            if (i&1)
                BFC_ASSERT_EQUAL(HAM_KEY_NOT_FOUND, 
                            ham_find(m_db, 0, &key, &rec, 0));
            else
                BFC_ASSERT_EQUAL(0, 
                            ham_find(m_db, 0, &key, &rec, 0));
        }
    }

    void recoverEraseTest(void)
    {
        ham_txn_t *txn;
        LogEntry vec[200];
        unsigned p=0;
        ham_key_t key={0};
        ham_record_t rec={0};
        journal_t *log;
        ham_u64_t lsn=2;

        /* create a transaction with many keys that are inserted, mostly
         * duplicates */
        BFC_ASSERT_EQUAL(0, ham_txn_begin(&txn, m_db, 0));
        vec[p++]=LogEntry(lsn++, txn_get_id(txn), 
                    JOURNAL_ENTRY_TYPE_TXN_BEGIN, 0xf000);
        for (int i=0; i<100; i++) {
            int val=i%10; 
            key.data=&val;
            key.size=sizeof(val);
            BFC_ASSERT_EQUAL(0, 
                        ham_insert(m_db, txn, &key, &rec, HAM_DUPLICATE));
            vec[p++]=LogEntry(lsn++, txn_get_id(txn), 
                        JOURNAL_ENTRY_TYPE_INSERT, 0xf000);
        }
        /* now delete them all */
        for (int i=0; i<10; i++) {
            key.data=&i;
            key.size=sizeof(i);
            BFC_ASSERT_EQUAL(0, 
                        ham_erase(m_db, txn, &key, 0));
            vec[p++]=LogEntry(lsn++, txn_get_id(txn), 
                        JOURNAL_ENTRY_TYPE_ERASE, 0xf000);
        }
        /* commit the txn */
        vec[p++]=LogEntry(lsn++, txn_get_id(txn), 
                    JOURNAL_ENTRY_TYPE_TXN_COMMIT, 0);
        BFC_ASSERT_EQUAL(0, ham_txn_commit(txn, 0));

        /* backup the journal files; then re-create the Environment from the 
         * journal */
        BFC_ASSERT_EQUAL(0, ham_close(m_db, HAM_DONT_CLEAR_LOG));
        BFC_ASSERT_EQUAL(0, ham_open(m_db, BFC_OPATH(".test"), 0));
        m_env=db_get_env(m_db);
        BFC_ASSERT_EQUAL(0, journal_open(m_env, 0, &log));
        env_set_journal(m_env, log);
        BFC_ASSERT(log!=0);
        compareJournal(log, vec, p);
        BFC_ASSERT_EQUAL(0, ham_close(m_db, HAM_DONT_CLEAR_LOG));
        BFC_ASSERT_EQUAL(0, 
                ham_open(m_db, BFC_OPATH(".test"), 
                        HAM_ENABLE_TRANSACTIONS
                        |HAM_AUTO_RECOVERY));
        m_env=db_get_env(m_db);

        /* verify that the journal is empty */
        verifyJournalIsEmpty();

        /* now verify that the committed transaction was re-played from
         * the journal; the database must be empty */
        ham_u64_t keycount;
        BFC_ASSERT_EQUAL(0, ham_get_key_count(m_db, 0, 0, &keycount));
        BFC_ASSERT_EQUAL(0ull, keycount);
    }

    void lsnOverflowTest(void)
    {
        journal_t *log=env_get_journal(m_env);
        journal_set_lsn(log, 0xffffffffffffffffull-1);
        ham_txn_t *txn;

        /* this one must work */
        BFC_ASSERT_EQUAL(0, ham_txn_begin(&txn, m_db, 0));
        /* this one must fail */
        BFC_ASSERT_EQUAL(HAM_LIMITS_REACHED, ham_txn_commit(txn, 0));

        /* and now it has to work again */
        journal_set_lsn(log, 3);
    }

};


BFC_REGISTER_FIXTURE(JournalTest);
