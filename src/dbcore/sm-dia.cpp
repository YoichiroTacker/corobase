#include "../ermia.h"
#include "sm-dia.h"
#include "sm-coroutine.h"
#include <vector>
#include <tuple>
#include <map>

namespace ermia {
namespace dia {

std::vector<IndexThread *> index_threads;

void SendGetRequest(ermia::transaction *t, OrderedIndex *index, const varstr *key, OID *oid, rc_t *rc) {
  // FIXME(tzwang): find the right index thread using some partitioning scheme
  uint32_t worker_id = 0;
  switch (ermia::config::benchmark[0]) {
    case 'y':
      worker_id = (uint32_t)(*((uint64_t*)(*key).data()) >> 32);
      ALWAYS_ASSERT(rc->_val == RC_INVALID);
      index_threads[worker_id % index_threads.size()]->AddRequest(t, index, key, oid, Request::kTypeGet, rc);
      break;
    
    default:
      LOG(FATAL) << "Not implemented";
      break;
  }
}

void SendInsertRequest(ermia::transaction *t, OrderedIndex *index, const varstr *key, OID *oid, rc_t *rc) {
  // FIXME(tzwang): find the right index thread using some partitioning scheme
  switch (ermia::config::benchmark[0]) {
    case 'y': {
      uint32_t worker_id = (uint32_t)(*((uint64_t*)(*key).data()) >> 32);
      index_threads[worker_id%index_threads.size()]->AddRequest(t, index, key, oid, Request::kTypeInsert, rc);
      }
      break;

    default:
      LOG(FATAL) << "Not implemented";
      break;
  }
}

// Prepare the extra index threads needed by DIA. The other compute threads
// should already be initialized by sm-config.
void Initialize() {
  LOG_IF(FATAL, thread::cpu_cores.size() == 0) << "No logical thread information available";

  // Need [config::worker_threads] number of logical threads, each corresponds to
  // to a physical worker thread
  for (uint32_t i = 0; i < ermia::config::worker_threads; ++i) {
    index_threads.emplace_back(new IndexThread());
  }

  for (auto t : index_threads) {
    while (!t->TryImpersonate()) {}
    t->Start();
  }
}


// The actual index access goes here
void IndexThread::MyWork(char *) {
  LOG(INFO) << "Index thread started";
  request_handler();
}

void IndexThread::SerialHandler() {
  if (ermia::config::dia_req_coalesce) {
    while (true) {
      thread_local std::map<uint64_t, std::vector<int> > coalesced_requests;
      coalesced_requests.clear();
      uint32_t pos = queue.getPos();
      int dequeueSize = 0;

      // Group requests by key
      for (int i = 0; i < kBatchSize; ++i) {
        Request *req = queue.GetRequestByPos(pos + i, false);
        if (!req) {
          break;
        }
        ermia::transaction *t = req->transaction;
        ALWAYS_ASSERT(t);
        ALWAYS_ASSERT(!((uint64_t)t & (1UL << 63)));  // make sure we got a ready transaction
        ALWAYS_ASSERT(req->type != Request::kTypeInvalid);
        ASSERT(req->oid_ptr);

        uint64_t current_key = *((uint64_t*)(*req->key).data());
        if (coalesced_requests.count(current_key)) {
          coalesced_requests[current_key].push_back(i);
        } else {
          std::vector<int> offsets;
          offsets.push_back(i);
          coalesced_requests.insert(std::make_pair(current_key, offsets));
        }
        ++dequeueSize;
      }

      // Handle requests for each key
      for (auto iter = coalesced_requests.begin(); iter!= coalesced_requests.end(); ++iter) {
        std::vector<int> &offsets = iter->second;

        // Must store results locally (instead of using the first request's rc
        // and oid as they might get reused by the application (benchmark).
        ermia::OID oid = 0;
        rc_t rc = {RC_INVALID};

        // Record if we have previously done an insert for the key. If we have
        // insert_ok == true then that means subsequent reads will always succeed
        // automatically. This can save us future calls into the index for reads.
        //
        // Note: here we don't deal with deletes which is handled
        // by the upper layer version chain traversal ops done after index ops.
        bool insert_ok = false;

        // Handle each request for the same key - for the first one we issue a
        // request, the latter ones will use the previous result; in case of the
        // read-insert-read pattern, we issue the insert when we see it.
        for (int i = 0; i < offsets.size(); ++i) {
          Request *req = queue.GetRequestByPos(pos + offsets[i], true);
          ALWAYS_ASSERT(req);
          ALWAYS_ASSERT(req->type != Request::kTypeInvalid);
          ASSERT(req->oid_ptr);
          *req->rc = rc_t{RC_INVALID};

          switch (req->type) {
            case Request::kTypeGet:
              if (insert_ok || rc._val != RC_INVALID) {
                // Two cases here that allow us to fill in the results directly:
                // 1. Previously inserted the key
                // 2. Previously read this key
                ASSERT((!insert_ok && rc._val != RC_INVALID) || (insert_ok && oid > 0 && rc._val == RC_TRUE));
              } else {
                // Haven't done any insert or read
                req->index->GetOID(*req->key, rc, req->transaction->GetXIDContext(), oid);
                // Now subsequent reads (before any insert) will use the result
                // here, and if there is an latter insert it will automatically
                // fail
              }

              // Fill in results
              ALWAYS_ASSERT(rc._val != RC_INVALID);
              volatile_write(*req->oid_ptr, oid);
              volatile_write(req->rc->_val, rc._val);
              break;

            case Request::kTypeInsert:
              if (insert_ok) {
                volatile_write(req->rc->_val, RC_FALSE);
                ASSERT(rc._val == RC_TRUE);
              } else {
                // Either we haven't done any insert or a previous insert failed.
                if (rc._val == RC_TRUE) {
                  // No insert before and previous reads succeeded: fail this
                  // insert
                  volatile_write(req->rc->_val, RC_FALSE);
                } else {
                  // Previous insert failed or previous reads returned false
                  insert_ok = req->index->InsertIfAbsent(req->transaction, *req->key, *req->oid_ptr);
                  // Now if insert_ok becomes true, then subsequent reads will
                  // also succeed; otherwise, subsequent reads will automatically
                  // fail without having to issue new read requests (rc will be
                  // RC_INVALID).
                  if (insert_ok) {
                    rc._val = RC_TRUE;
                    oid = *req->oid_ptr;  // Store the OID for future reads
                  } else {
                    rc._val = RC_FALSE;
                  }
                  volatile_write(req->rc->_val, rc._val);
                }
              }
              break;
            default:
              LOG(FATAL) << "Wrong request type";
          }
        }
      }

      for (int i = 0; i < dequeueSize; ++i) {
        queue.Dequeue();
      }
    }
  } else {
    while (true) {
      Request &req = queue.GetNextRequest();
      ermia::transaction *t = volatile_read(req.transaction);
      ALWAYS_ASSERT(t);
      ALWAYS_ASSERT(req.type != Request::kTypeInvalid);
      *req.rc = rc_t{RC_INVALID};
      ASSERT(req.oid_ptr);
      switch (req.type) {
        // Regardless the request is for record read or update, we only need to get
        // the OID, i.e., a Get operation on the index. For updating OID, we need
        // to use the Put interface
        case Request::kTypeGet:
          req.index->GetOID(*req.key, *req.rc, req.transaction->GetXIDContext(), *req.oid_ptr);
          break;
        case Request::kTypeInsert:
          if (req.index->InsertIfAbsent(req.transaction, *req.key, *req.oid_ptr)) {
            volatile_write(req.rc->_val, RC_TRUE);
          } else {
            volatile_write(req.rc->_val, RC_FALSE);
          }
          break;
        default:
          LOG(FATAL) << "Wrong request type";
      }
      queue.Dequeue();
    }
  }
}

void IndexThread::CoroutineHandler() {
  if (ermia::config::dia_req_coalesce) {
    while (true) {
      thread_local std::vector<ermia::dia::generator<bool> *> coroutines;
      coroutines.clear();
      thread_local std::map<uint64_t, std::vector<int>> coalesced_requests;
      coalesced_requests.clear();
      // Must store results locally (instead of using the first request's rc)
      // as they might get reused by the application (benchmark).
      thread_local rc_t tls_rcs[kBatchSize];
      memset(tls_rcs, 0, sizeof(rc_t) * kBatchSize); // #define RC_INVALID 0x0
      uint32_t pos = queue.getPos();
      int dequeueSize = 0;

      // Group requests by key and push the first request of each key 
      // to the scheduler of coroutines
      for (int i = 0; i < kBatchSize; ++i){
        Request *req = queue.GetRequestByPos(pos + i, false);
        if (!req) {
          break;
        }
        ermia::transaction *t = req->transaction;
        ALWAYS_ASSERT(t);
        ALWAYS_ASSERT(!((uint64_t)t & (1UL << 63)));  // make sure we got a ready transaction
        ALWAYS_ASSERT(req->type != Request::kTypeInvalid);
        *req->rc = rc_t{RC_INVALID};
        ASSERT(req->oid_ptr);

        uint64_t current_key = *((uint64_t*)(*req->key).data());
        if (coalesced_requests.count(current_key)) {
          coalesced_requests[current_key].push_back(i);
        } else {
          std::vector<int> offsets = {i};
          coalesced_requests.insert(std::make_pair(current_key, offsets));
        }

        switch (req->type) {
          case Request::kTypeGet:
            coroutines.push_back(new ermia::dia::generator<bool>(req->index->coro_GetOID(*req->key, tls_rcs[i], t->GetXIDContext(), *req->oid_ptr)));
            break;
          case Request::kTypeInsert:
            coroutines.push_back(new ermia::dia::generator<bool>(req->index->coro_InsertIfAbsent(t, *req->key, tls_rcs[i], *req->oid_ptr)));
            break;
          default:
            LOG(FATAL) << "Wrong request type";
        }
        ++dequeueSize;
      }

      // Issued the requests in the scheduler
      while (coroutines.size()){
        for (auto it = coroutines.begin(); it != coroutines.end();) {
          if ((*it)->advance()){
            ++it;
          }else{
            delete (*it);
            it = coroutines.erase(it);
          }
        }
      }

      // Handle requests for each key
      for (auto iter = coalesced_requests.begin(); iter!= coalesced_requests.end(); ++iter) {
        std::vector<int> &offsets = iter->second;
        // The first request'rc has been stored locally in an array.
        // These variables are only used for logical judgement.
        ermia::OID oid = 0;
        rc_t rc = {RC_INVALID};
        bool insert_ok = false;

        for (int i = 0; i < offsets.size(); ++i){
          Request *req = queue.GetRequestByPos(pos + offsets[i], true);
          ALWAYS_ASSERT(req);
          ALWAYS_ASSERT(req->type != Request::kTypeInvalid);
          ASSERT(req->oid_ptr);
          if (i) {
            *req->rc = rc_t{RC_INVALID};
            switch (req->type) {
              case Request::kTypeGet:
              // At least one get or insert has been done before by coroutines.
              // The previous result has been stored locally. Just fill in return
              // code.
                ALWAYS_ASSERT(rc._val != RC_INVALID);
                volatile_write(*req->oid_ptr, oid);
                volatile_write(req->rc->_val, rc._val);
                break;
              case Request::kTypeInsert:
                if (insert_ok) {
                  volatile_write(req->rc->_val, RC_FALSE);
                  ASSERT(rc._val == RC_TRUE);
                } else {
                  // Either we haven't done any insert or a previous insert failed.
                  if (rc._val == RC_TRUE) {
                    // No insert before and previous reads succeeded: fail this
                    // insert
                    volatile_write(req->rc->_val, RC_FALSE);
                  } else {
                    // Previous insert failed or previous reads returned false
                    insert_ok = req->index->InsertIfAbsent(req->transaction, *req->key, *req->oid_ptr);
                    // Now if insert_ok becomes true, then subsequent reads will
                    // also succeed; otherwise, subsequent reads will automatically
                    // fail without having to issue new read requests (rc will be
                    // RC_INVALID).
                    if (insert_ok) {
                      rc._val = RC_TRUE;
                      oid = *req->oid_ptr;  // Store the OID for future reads
                    } else {
                      rc._val = RC_FALSE;
                    }
                    volatile_write(req->rc->_val, rc._val);
                  }
                }
                break;
              default:
                LOG(FATAL) << "Wrong request type";
            }
          } else {
            // The first request of each key has been done by coroutines.
            // Just store the results locally and fill in return codes.
            rc = tls_rcs[offsets[0]];
            if (rc._val == RC_TRUE) {
              oid = *req->oid_ptr;
              if (req->type == Request::kTypeInsert)
                insert_ok = true;
            }
            volatile_write(req->rc->_val, rc._val);
          }
        }
      }

      for (int i = 0; i < dequeueSize; ++i)
        queue.Dequeue();
    }
  } else {
    while (true) {
      thread_local std::vector<ermia::dia::generator<bool> *> coroutines;
      coroutines.clear();
      uint32_t pos = queue.getPos();
      for (int i = 0; i < kBatchSize; ++i){
        Request *req = queue.GetRequestByPos(pos + i, false);
        if (!req) {
          break;
        }
        ermia::transaction *t = req->transaction;
        ALWAYS_ASSERT(t);
        ALWAYS_ASSERT(!((uint64_t)t & (1UL << 63)));  // make sure we got a ready transaction
        ALWAYS_ASSERT(req->type != Request::kTypeInvalid);
        *req->rc = rc_t{RC_INVALID};
        ASSERT(req->oid_ptr);

        switch (req->type) {
          // Regardless the request is for record read or update, we only need to get
          // the OID, i.e., a Get operation on the index. For updating OID, we need
          // to use the Put interface
          case Request::kTypeGet:
            coroutines.push_back(new ermia::dia::generator<bool>(req->index->coro_GetOID(*req->key, *req->rc, t->GetXIDContext(), *req->oid_ptr)));
            break;
          case Request::kTypeInsert:
            coroutines.push_back(new ermia::dia::generator<bool>(req->index->coro_InsertIfAbsent(t, *req->key, *req->rc, *req->oid_ptr)));
            break;
          default:
            LOG(FATAL) << "Wrong request type";
        }
      }

      int dequeueSize = coroutines.size();
      while (coroutines.size()){
        for (auto it = coroutines.begin(); it != coroutines.end();) {
          if ((*it)->advance()){
            ++it;
          }else{
            delete (*it);
            it = coroutines.erase(it);
          }
        }
      }

      for (int i = 0; i < dequeueSize; ++i)
        queue.Dequeue();
    }
  }
}

}  // namespace dia
}  // namespace ermia
