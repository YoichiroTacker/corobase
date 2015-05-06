#pragma once
#include <unordered_map>

#include "abstract_db.h"
#include "../txn_btree.h"
#include "../dbcore/sm-log.h"

namespace private_ {
  struct ndbtxn {
    abstract_db::TxnProfileHint hint;
    char buf[0];
  } PACKED;

  STATIC_COUNTER_DECL(scopedperf::tsc_ctr, ndb_get_probe0, ndb_get_probe0_cg)
  STATIC_COUNTER_DECL(scopedperf::tsc_ctr, ndb_put_probe0, ndb_put_probe0_cg)
  STATIC_COUNTER_DECL(scopedperf::tsc_ctr, ndb_insert_probe0, ndb_insert_probe0_cg)
  STATIC_COUNTER_DECL(scopedperf::tsc_ctr, ndb_scan_probe0, ndb_scan_probe0_cg)
  STATIC_COUNTER_DECL(scopedperf::tsc_ctr, ndb_remove_probe0, ndb_remove_probe0_cg)
  STATIC_COUNTER_DECL(scopedperf::tsc_ctr, ndb_dtor_probe0, ndb_dtor_probe0_cg)
}

class ndb_wrapper : public abstract_db {
protected:
  typedef private_::ndbtxn ndbtxn;

public:

  ndb_wrapper(const char *logdir, size_t segsize, size_t bufsize);
  ~ndb_wrapper() { RCU::rcu_deregister(); }

  virtual ssize_t txn_max_batch_size() const OVERRIDE { return 100; }

  virtual size_t
  sizeof_txn_object(uint64_t txn_flags) const;

  virtual void *new_txn(
      uint64_t txn_flags,
      str_arena &arena,
      void *buf,
      TxnProfileHint hint);
  virtual rc_t commit_txn(void *txn);
  virtual void abort_txn(void *txn);
  virtual void print_txn_debug(void *txn) const;

  virtual abstract_ordered_index *
  open_index(const std::string &name,
             size_t value_size_hint,
             bool mostly_append);

  virtual void
  close_index(abstract_ordered_index *idx);

};

class ndb_ordered_index : public abstract_ordered_index {
    friend class sm_log;    // for recover_index()
protected:
  typedef private_::ndbtxn ndbtxn;

public:
  ndb_ordered_index(const std::string &name, FID fid, size_t value_size_hint, bool mostly_append);
  virtual rc_t get(
      void *txn,
      const varstr &key,
      varstr &value, size_t max_bytes_read);
  virtual rc_t put(
      void *txn,
      const varstr &key,
      const varstr &value);
  virtual rc_t put(
      void *txn,
      varstr &&key,
      varstr &&value);
  virtual rc_t
  insert(void *txn,
         const varstr &key,
         const varstr &value);
  virtual rc_t
  insert(void *txn,
         varstr &&key,
         varstr &&value);
  virtual rc_t scan(
      void *txn,
      const varstr &start_key,
      const varstr *end_key,
      scan_callback &callback,
      str_arena *arena);
  virtual rc_t rscan(
      void *txn,
      const varstr &start_key,
      const varstr *end_key,
      scan_callback &callback,
      str_arena *arena);
  virtual rc_t remove(
      void *txn,
      const varstr &key);
  virtual rc_t remove(
      void *txn,
      varstr &&key);
  virtual size_t size() const;
  virtual std::map<std::string, uint64_t> clear();
private:
  std::string name;
  txn_btree btr;
};
