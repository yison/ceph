// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
//
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
  *
 * Copyright (C) 2015 XSky <haomai@xsky.com>
 *
 * Author: Haomai Wang <haomaiwang@gmail.com>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include <unistd.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <fstream>
#include <functional>
#include <map>
#include <thread>
#include <boost/intrusive/slist.hpp>
#include <boost/lockfree/queue.hpp>

#include <spdk/nvme.h>
#include <rte_lcore.h>

#include "include/intarith.h"
#include "include/stringify.h"
#include "include/types.h"
#include "include/compat.h"
#include "common/errno.h"
#include "common/debug.h"
#include "common/perf_counters.h"

#include "NVMEDevice.h"

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_bdev
#undef dout_prefix
#define dout_prefix *_dout << "bdev"

// static constexpr uint16_t data_buffer_default_num = 1024;
// static constexpr uint16_t data_buffer_default_num = 2048;
// static constexpr uint16_t data_buffer_default_num = 8192;
static constexpr uint16_t data_buffer_default_num = 16384;

static constexpr uint32_t data_buffer_size = 8192;

static constexpr uint16_t inline_segment_num = 32;

static constexpr uint32_t max_task_queue_size = 1024;
// static constexpr uint16_t max_task_ops = 128;
// static constexpr uint16_t max_task_ops = 512;
// static constexpr uint16_t max_task_ops = 64;

static constexpr uint32_t submit_limit = 256;

static thread_local int queue_id = -1;

static void io_complete(void *t, const struct spdk_nvme_cpl *completion);

static int dpdk_thread_adaptor(void *f)
{
  (*static_cast<std::function<void ()>*>(f))();
  return 0;
}

struct IORequest {
  uint16_t cur_seg_idx = 0;
  uint16_t nseg;
  uint32_t cur_seg_left = 0;
  void *inline_segs[inline_segment_num];
  void **extra_segs = nullptr;
};

namespace bi = boost::intrusive;
struct data_cache_buf : public bi::slist_base_hook<bi::link_mode<bi::normal_link>>
{};

struct Task;

class SharedDriverQueueData {
  SharedDriverData *driver;
  spdk_nvme_ctrlr *ctrlr;
  spdk_nvme_ns *ns;
  uint32_t block_size;
  uint32_t core_id;
  uint32_t queue_id;
  uint32_t max_queue_depth;
  struct spdk_nvme_qpair *qpair;
  std::function<void ()> run_func;
  bool aio_stop = false;

  void _aio_thread();
  int alloc_buf_from_pool(Task *t, bool write);

  std::atomic_bool queue_empty;
  ceph::mutex queue_lock = ceph::make_mutex("SharedDriverQueueData::queue_lock");
  ceph::condition_variable queue_cond;
//   std::queue<Task*> task_queue;
  boost::lockfree::queue<Task*> task_queue;

  ceph::mutex flush_lock = ceph::make_mutex("SharedDriverQueueData::flush_lock");
  ceph::condition_variable flush_cond;
  std::atomic_int flush_waiters;
  std::set<uint64_t> flush_waiter_seqs;
  uint32_t low_water_threshold;

  public:
    std::deque<Task*> submit_queue;
    uint32_t current_queue_depth = 0;
    // TODO: queue_op_seq, completed_op_seq will overflow!!!
//     std::atomic_ulong completed_op_seq, queue_op_seq;
    std::atomic_ulong inflight_ops;
    bi::slist<data_cache_buf, bi::constant_time_size<true>> data_buf_list;
    void _aio_handle(Task *t, IOContext *ioc);
    bool is_overlap(std::deque<Task*>::iterator t_iter);
    void remove_from_submit_queue(Task *t);

    SharedDriverQueueData(SharedDriverData *driver, spdk_nvme_ctrlr *c, spdk_nvme_ns *ns, uint64_t block_size,
                          uint32_t core_id, uint32_t queue_id)
      : driver(driver),
        ctrlr(c),
        ns(ns),
        block_size(block_size),
        core_id(core_id),
        queue_id(queue_id),
        run_func([this]() {_aio_thread(); }),
        queue_empty(false),
        task_queue(max_task_queue_size),
        flush_waiters(0),
        inflight_ops(0) {
        // completed_op_seq(0),
        // queue_op_seq(0) {

    dout(1) << __func__ << "@@: SharedDriverQueueData queue_id=" << queue_id << dendl;
    struct spdk_nvme_io_qpair_opts opts = {};
    spdk_nvme_ctrlr_get_default_io_qpair_opts(ctrlr, &opts, sizeof(opts));
    opts.qprio = SPDK_NVME_QPRIO_URGENT;
    // usable queue depth should minus 1 to aovid overflow.
//     max_queue_depth = opts.io_queue_size - 1;
    max_queue_depth = 1024;
    low_water_threshold = 512;
    qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, &opts, sizeof(opts));
    ceph_assert(qpair != NULL);

    // allocate spdk dma memory
    for (uint16_t i = 0; i < data_buffer_default_num; i++) {
      void *b = spdk_dma_zmalloc(data_buffer_size, CEPH_PAGE_SIZE, NULL);
      if (!b) {
        derr << __func__ << " failed to create memory pool for nvme data buffer" << dendl;
        ceph_assert(b);
      }
      data_buf_list.push_front(*reinterpret_cast<data_cache_buf *>(b));
    }
  }

  ~SharedDriverQueueData() {
    dout(1) << __func__ << "@@: ~SharedDriverQueueData" << dendl;
    if (qpair) {
      spdk_nvme_ctrlr_free_io_qpair(qpair);
    }

    data_buf_list.clear_and_dispose(spdk_dma_free);
  }

  void queue_task(Task *t, uint64_t ops = 1);

  void flush_wait() {
    if (inflight_ops.load() > 0) {
      // TODO: this may contains read op
      dout(5) << __func__ << " existed inflight ops " << left << dendl;
      std::unique_lock l{flush_lock};
      ++flush_waiters;
      while (inflight_ops.load() > 0) {
        flush_cond.wait(l);
      }
      --flush_waiters;
    }
  }

  void start() {
    dout(1) << __func__ << " start thread on core id=" << core_id << dendl;
    int r = rte_eal_remote_launch(dpdk_thread_adaptor, static_cast<void*>(&run_func),
                                  core_id);
    ceph_assert(r == 0);
  }

  void stop() {
    {
      std::unique_lock l(queue_lock);
      aio_stop = true;
//       queue_cond.notify_all();
      queue_cond.wait(l);
    }
    dout(1) << __func__ << " stop thread on core id=" << core_id << dendl;
    int r = rte_eal_wait_lcore(core_id);
    ceph_assert(r == 0);
    aio_stop = false;
  }
};

class SharedDriverData {
  unsigned id;
  spdk_nvme_transport_id trid;
  spdk_nvme_ctrlr *ctrlr;
  spdk_nvme_ns *ns;
  uint32_t block_size = 0;
  uint64_t size = 0;
  uint32_t queue_number;
  std::vector<SharedDriverQueueData*> queues;

  void _aio_start() {
    for (auto &&it : queues) {
      it->start();
    }
  }
  void _aio_stop() {
    for (auto &&it : queues) {
      it->stop();
    }
  }

  public:
  std::vector<NVMEDevice*> registered_devices;
  friend class SharedDriverQueueData;
  SharedDriverData(unsigned id_, const spdk_nvme_transport_id& trid_,
                   spdk_nvme_ctrlr *c, spdk_nvme_ns *ns_)
      : id(id_),
        trid(trid_),
        ctrlr(c),
        ns(ns_) {
    block_size = spdk_nvme_ns_get_extended_sector_size(ns);
    size = spdk_nvme_ns_get_size(ns);
    int i;
    RTE_LCORE_FOREACH_SLAVE(i) {
      queues.push_back(new SharedDriverQueueData(this, ctrlr, ns, block_size, i, queue_number++));
   }

   _aio_start();
  }

  ~SharedDriverData() {
    _aio_stop();
    for (auto p : queues) {
      delete p;
   }
  }

  bool is_equal(const spdk_nvme_transport_id& trid2) const {
    return spdk_nvme_transport_id_compare(&trid, &trid2) == 0;
  }

  SharedDriverQueueData *get_queue(uint32_t i) {
//     dout(1) << __func__ << "@1 i=" << i
//                         << " queue_number="
//                         << queue_number
//                         << " i % queue_number="
//                         << i%queue_number
//                         << " queues.size="
//                         << queues.size()
//                         << dendl;
    return queues.at(i % queue_number);
  }

  void register_device(NVMEDevice *device) {
    // in case of registered_devices, we stop thread now.
    // Because release is really a rare case, we could bear this
    _aio_stop();
    registered_devices.push_back(device);
    _aio_start();
  }

  void remove_device(NVMEDevice *device) {
    _aio_stop();
    std::vector<NVMEDevice*> new_devices;
    for (auto &&it : registered_devices) {
      if (it != device)
        new_devices.push_back(it);
    }
    registered_devices.swap(new_devices);
    _aio_start();
  }

  uint32_t get_block_size() {
    return block_size;
  }
  uint64_t get_size() {
    return size;
  }
};

struct Task {
  NVMEDevice *device;
  IOContext *ctx = nullptr;
  IOCommand command;
  uint64_t offset;
  uint64_t len;
  bufferlist bl;
  std::function<void()> fill_cb;
  Task *next = nullptr;
  int64_t return_code;
  Task *primary = nullptr;
  ceph::coarse_real_clock::time_point start;
  IORequest io_request = {};
  ceph::condition_variable cond;
  SharedDriverQueueData *queue = nullptr;
  // reference count by subtasks.
  int ref = 0;
  bool submitted;
  bool overlap;
  bool init;
  Task(NVMEDevice *dev, IOCommand c, uint64_t off, uint64_t l, int64_t rc = 0,
       Task *p = nullptr)
    : device(dev), command(c), offset(off), len(l),
      return_code(rc), primary(p),
      start(ceph::coarse_real_clock::now()) {
        if (primary) {
          primary->ref++;
          return_code = primary->return_code;
        }
        submitted = false;
        overlap = false;
        init = true;
     }
  ~Task() {
    if (primary)
      primary->ref--;
    ceph_assert(!io_request.nseg);
  }
  void release_segs(SharedDriverQueueData *queue_data) {
    if (io_request.extra_segs) {
      for (uint16_t i = 0; i < io_request.nseg; i++) {
        auto buf = reinterpret_cast<data_cache_buf *>(io_request.extra_segs[i]);
        queue_data->data_buf_list.push_front(*buf);
      }
      delete io_request.extra_segs;
    } else if (io_request.nseg) {
      for (uint16_t i = 0; i < io_request.nseg; i++) {
        auto buf = reinterpret_cast<data_cache_buf *>(io_request.inline_segs[i]);
        queue_data->data_buf_list.push_front(*buf);
      }
    }
    ctx->total_nseg -= io_request.nseg;
    io_request.nseg = 0;
  }

  void copy_to_buf(char *buf, uint64_t off, uint64_t len) {
    uint64_t copied = 0;
    uint64_t left = len;
    void **segs = io_request.extra_segs ? io_request.extra_segs : io_request.inline_segs;
    uint16_t i = 0;
    while (left > 0) {
      char *src = static_cast<char*>(segs[i++]);
      uint64_t need_copy = std::min(left, data_buffer_size-off);
      memcpy(buf+copied, src+off, need_copy);
      off = 0;
      left -= need_copy;
      copied += need_copy;
    }
  }
};

static void data_buf_reset_sgl(void *cb_arg, uint32_t sgl_offset)
{
  Task *t = static_cast<Task*>(cb_arg);
  uint32_t i = sgl_offset / data_buffer_size;
  uint32_t offset = i * data_buffer_size;
  ceph_assert(i <= t->io_request.nseg);

  for (; i < t->io_request.nseg; i++) {
    offset += data_buffer_size;
    if (offset > sgl_offset) {
      if (offset > t->len)
        offset = t->len;
      break;
    }
  }

  t->io_request.cur_seg_idx = i;
  t->io_request.cur_seg_left = offset - sgl_offset;
  return ;
}

static int data_buf_next_sge(void *cb_arg, void **address, uint32_t *length)
{
  uint32_t size;
  void *addr;
  Task *t = static_cast<Task*>(cb_arg);
  if (t->io_request.cur_seg_idx >= t->io_request.nseg) {
    *length = 0;
    *address = 0;
    return 0;
  }

  addr = t->io_request.extra_segs ? t->io_request.extra_segs[t->io_request.cur_seg_idx] : t->io_request.inline_segs[t->io_request.cur_seg_idx];

  size = data_buffer_size;
  if (t->io_request.cur_seg_idx == t->io_request.nseg - 1) {
      uint64_t tail = t->len % data_buffer_size;
      if (tail) {
        size = (uint32_t) tail;
      }
  }
 
  if (t->io_request.cur_seg_left) {
    *address = (void *)((uint64_t)addr + size - t->io_request.cur_seg_left);
    *length = t->io_request.cur_seg_left;
    t->io_request.cur_seg_left = 0;
  } else {
    *address = addr;
    *length = size;
  }
  
  t->io_request.cur_seg_idx++;
  return 0;
}

void SharedDriverQueueData::queue_task(Task *t, uint64_t ops) {
  inflight_ops +=ops;
  task_queue.push(t);
}

int SharedDriverQueueData::alloc_buf_from_pool(Task *t, bool write)
{
  uint64_t count = t->len / data_buffer_size;
  if (t->len % data_buffer_size)
    ++count;
  void **segs;
  if (count > data_buf_list.size())
    return -ENOMEM;
  if (count <= inline_segment_num) {
    segs = t->io_request.inline_segs;
  } else {
    t->io_request.extra_segs = new void*[count];
    segs = t->io_request.extra_segs;
  }
  for (uint16_t i = 0; i < count; i++) {
    ceph_assert(!data_buf_list.empty());
    segs[i] = &data_buf_list.front();
    ceph_assert(segs[i] != nullptr);
    data_buf_list.pop_front();
  }
  t->io_request.nseg = count;
  t->ctx->total_nseg += count;
  if (write) {
    auto blp = t->bl.begin();
    uint32_t len = 0;
    uint16_t i = 0;
    for (; i < count - 1; ++i) {
      blp.copy(data_buffer_size, static_cast<char*>(segs[i]));
      len += data_buffer_size;
    }
    blp.copy(t->bl.length() - len, static_cast<char*>(segs[i]));
  }

  return 0;
}

bool SharedDriverQueueData::is_overlap(std::deque<Task*>::iterator t_iter) {
  Task *t = *t_iter;
  uint64_t start = t->offset;
  uint64_t end = t->offset + t->len;
  uint64_t _start = 0;
  uint64_t _end = 0;
  std::deque<Task*>::iterator iter;
  for (iter = submit_queue.begin(); iter != t_iter; ++iter) {
    _start = (*iter)->offset;
    _end = (*iter)->offset + (*iter)->len;
    if (std::max<uint64_t>(start, _start) < std::min<uint64_t>(end, _end)) {
      dout(5) << " @@@"
              << " start=" << start
              << " end=" << end
              << " _start=" << _start
              << " _end=" << _end
              << dendl;
      return true;
    }
  }
  return false;
}

void SharedDriverQueueData::remove_from_submit_queue(Task *t) {
   std::deque<Task*>::iterator it;
   for (it=submit_queue.begin(); it!=submit_queue.end(); ++it) {
     if (t == *it) {
       break;
     }
   }
   if(it != submit_queue.end()) {
     submit_queue.erase(it);
   }
}

void SharedDriverQueueData::_aio_thread()
{
  dout(1) << __func__ << " start" << dendl;

  Task *t = nullptr;
  int r = 0;
  bool no_slots = false;
  bool trigger_low_water = false;
  uint64_t lba_off, lba_count;
  uint32_t max_io_completion = (uint32_t)g_conf().get_val<uint64_t>("bluestore_spdk_max_io_completion");

  while (true) {
    bool inflight = inflight_ops.load();
 again:
    dout(40) << __func__ << " polling" << dendl;
    if (inflight) {
      r = spdk_nvme_qpair_process_completions(qpair, max_io_completion);
      if (r < 0) {
        ceph_abort();
      } else if (trigger_low_water && (r == 0)) {
        goto again;
      } else if (no_slots && (current_queue_depth > low_water_threshold)) {
        goto again;
      }
    }

    for (std::deque<Task*>::iterator it=submit_queue.begin(); it!=submit_queue.end(); ++it) {
      if (current_queue_depth >= max_queue_depth) {
        dout(1) << __func__ << " @3 no slots!" << dendl;
        // no slots
        no_slots = true;
        goto again;
      }
      no_slots = false;

      if ((*it)->submitted) {
        continue;
      } else {
        //check overlap
        if(is_overlap(it)) {
          (*it)->overlap = true;
          continue;
        } else {
          t = *it;
          t->queue = this;
          lba_off = t->offset / block_size;
          lba_count = t->len / block_size;
          switch (t->command) {
            case IOCommand::WRITE_COMMAND:
            {
              dout(20) << __func__ << " write command issued " << lba_off << "~" << lba_count << dendl;
              r = alloc_buf_from_pool(t, true);
              if (r < 0) {
                dout(1) << __func__ << " @2 alloc_buf_from_pool failed!" << dendl;
                goto again;
              }
              r = spdk_nvme_ns_cmd_writev(
                  ns, qpair, lba_off, lba_count, io_complete, t, 0,
                  data_buf_reset_sgl, data_buf_next_sge);
              if (r < 0) {
                derr << __func__ << " failed to do write command: " << cpp_strerror(r) << dendl;
                t->ctx->nvme_task_first = t->ctx->nvme_task_last = nullptr;
                t->release_segs(this);
                delete t;
                ceph_abort();
              }
              t->submitted = true;

              IOContext *ctx = t->ctx;
              if (ctx->priv) {
                if (!--ctx->num_running) {
                  t->device->aio_callback(t->device->aio_callback_priv, ctx->priv);
                }
              } else {
                ctx->try_aio_wake();
              }
              current_queue_depth++;
              break;
            }
            case IOCommand::READ_COMMAND:
            {
              dout(20) << __func__ << " read command issued " << lba_off << "~" << lba_count << dendl;
              r = alloc_buf_from_pool(t, false);
              if (r < 0) {
                goto again;
              }

              r = spdk_nvme_ns_cmd_readv(
                  ns, qpair, lba_off, lba_count, io_complete, t, 0,
                  data_buf_reset_sgl, data_buf_next_sge);
              if (r < 0) {
                derr << __func__ << " failed to read: " << cpp_strerror(r) << dendl;
                t->release_segs(this);
                delete t;
                ceph_abort();
              }
              t->submitted = true;
              current_queue_depth++;
              break;
            }
          }
        }
      }
    }
    if (current_queue_depth >= low_water_threshold) {
      dout(1) << __func__ << " @4 trigger low water threshold!" << dendl;
      trigger_low_water = true;
    } else {
      trigger_low_water = false;
    }

    if(submit_queue.size() < max_queue_depth) {
      std::queue<Task*> q;
      Task * item = nullptr;
      while (task_queue.pop(item)) {
          q.push(item);
      }
      int q_size = q.size();
      for (int i = 0; i < q_size; ++i ) {
        Task * entry = q.front();
        q.pop();
        while(entry) {
          entry->ctx->nvme_task_last = entry->ctx->nvme_task_first = nullptr;
          submit_queue.push_back(entry);
          entry = entry->next;
        }
      }
    }
    if (inflight) {
      dout(1) << __func__ << " @5 submit_queue_size=" << submit_queue.size()
                          << " current_queue_depth=" << current_queue_depth
                          << dendl;
    }

    if (!inflight) {
      // be careful, here we need to let each thread reap its own, currently it is done
      // by only one dedicatd dpdk thread
      if(!queue_id) {
        for (auto &&it : driver->registered_devices)
          it->reap_ioc();
      }

      if (aio_stop) {
        std::lock_guard l(queue_lock);
        if (task_queue.empty()) {
          queue_cond.notify_all();
          break;
        }
      }
    }
  }
  assert(data_buf_mempool.size() == data_buffer_default_num);
  dout(1) << __func__ << " end" << dendl;
}

#define dout_subsys ceph_subsys_bdev
#undef dout_prefix
#define dout_prefix *_dout << "bdev "

class NVMEManager {
 public:
  struct ProbeContext {
    spdk_nvme_transport_id trid;
    NVMEManager *manager;
    SharedDriverData *driver;
    bool done;
  };

 private:
  ceph::mutex lock = ceph::make_mutex("NVMEManager::lock");
  bool stopping = false;
  std::vector<SharedDriverData*> shared_driver_datas;
  std::thread dpdk_thread;
  ceph::mutex probe_queue_lock = ceph::make_mutex("NVMEManager::probe_queue_lock");
  ceph::condition_variable probe_queue_cond;
  std::list<ProbeContext*> probe_queue;
  spdk_nvme_ctrlr * ctrlr;
 public:
  NVMEManager() {}
  ~NVMEManager() {
    if (!dpdk_thread.joinable())
      return;
    {
      std::lock_guard guard(probe_queue_lock);
      stopping = true;
      probe_queue_cond.notify_all();
    }
    dpdk_thread.join();
    for (auto driver : shared_driver_datas) {
      delete driver;
    }
    spdk_nvme_detach(ctrlr);
  }

  int try_get(const spdk_nvme_transport_id& trid, SharedDriverData **driver);
  void register_ctrlr(const spdk_nvme_transport_id& trid, spdk_nvme_ctrlr *c, SharedDriverData **driver) {
    ctrlr = c;
    ceph_assert(ceph_mutex_is_locked(lock));
    spdk_nvme_ns *ns;
    int num_ns = spdk_nvme_ctrlr_get_num_ns(c);
    ceph_assert(num_ns >= 1);
    if (num_ns > 1) {
      dout(0) << __func__ << " namespace count larger than 1, currently only use the first namespace" << dendl;
    }
    ns = spdk_nvme_ctrlr_get_ns(c, 1);
    if (!ns) {
      derr << __func__ << " failed to get namespace at 1" << dendl;
      ceph_abort();
    }
    dout(1) << __func__ << " successfully attach nvme device at" << trid.traddr << dendl;

    // only support one device per osd now!
    ceph_assert(shared_driver_datas.empty());
    // index 0 is occurred by master thread
    shared_driver_datas.push_back(new SharedDriverData(shared_driver_datas.size()+1, trid, c, ns));
    *driver = shared_driver_datas.back();
  }
};

static NVMEManager manager;

static bool probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid, struct spdk_nvme_ctrlr_opts *opts)
{
  NVMEManager::ProbeContext *ctx = static_cast<NVMEManager::ProbeContext*>(cb_ctx);

  if (trid->trtype != SPDK_NVME_TRANSPORT_PCIE) {
    dout(0) << __func__ << " only probe local nvme device" << dendl;
    return false;
  }

  dout(0) << __func__ << " found device at: "
	  << "trtype=" << spdk_nvme_transport_id_trtype_str(trid->trtype) << ", "
          << "traddr=" << trid->traddr << dendl;
  if (spdk_nvme_transport_id_compare(&ctx->trid, trid)) {
    dout(0) << __func__ << " device traddr (" << ctx->trid.traddr << ") not match " << trid->traddr << dendl;
    return false;
  }

  opts->io_queue_size = UINT16_MAX;

  return true;
}

static void attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
                      struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
  auto ctx = static_cast<NVMEManager::ProbeContext*>(cb_ctx);
  ctx->manager->register_ctrlr(ctx->trid, ctrlr, &ctx->driver);
}

static int hex2dec(unsigned char c)
{
  if (isdigit(c))
    return c - '0';
  else if (isupper(c))
    return c - 'A' + 10;
  else
    return c - 'a' + 10;
}

static int find_first_bitset(const string& s)
{
  auto e = s.rend();
  if (s.compare(0, 2, "0x") == 0 ||
      s.compare(0, 2, "0X") == 0) {
    advance(e, -2);
  }
  auto p = s.rbegin();
  for (int pos = 0; p != e; ++p, pos += 4) {
    if (!isxdigit(*p)) {
      return -EINVAL;
    }
    if (int val = hex2dec(*p); val != 0) {
      return pos + ffs(val);
    }
  }
  return 0;
}

int NVMEManager::try_get(const spdk_nvme_transport_id& trid, SharedDriverData **driver)
{
  std::lock_guard l(lock);
  for (auto &&it : shared_driver_datas) {
    if (it->is_equal(trid)) {
      *driver = it;
      return 0;
    }
  }

  struct spdk_pci_addr pci_addr;
  int rc = spdk_pci_addr_parse(&pci_addr, trid.traddr);
  if (rc < 0) {
    derr << __func__ << " invalid transport address: " << trid.traddr << dendl;
    return -ENOENT;
  }
  auto coremask_arg = g_conf().get_val<std::string>("bluestore_spdk_coremask");
  int m_core_arg = find_first_bitset(coremask_arg);
  // at least one core is needed for using spdk
  if (m_core_arg <= 0) {
    derr << __func__ << " invalid bluestore_spdk_coremask, "
	 << "at least one core is needed" << dendl;
    return -ENOENT;
  }
  m_core_arg -= 1;

  uint32_t mem_size_arg = (uint32_t)g_conf().get_val<Option::size_t>("bluestore_spdk_mem");

  if (!dpdk_thread.joinable()) {
    dpdk_thread = std::thread(
      [this, coremask_arg, m_core_arg, mem_size_arg, pci_addr]() {
        struct spdk_env_opts opts;
        struct spdk_pci_addr addr = pci_addr;
        int r;

        spdk_env_opts_init(&opts);
        opts.name = "nvme-device-manager";
        opts.core_mask = coremask_arg.c_str();
        opts.master_core = m_core_arg;
        opts.mem_size = mem_size_arg;
        opts.pci_whitelist = &addr;
        opts.num_pci_addr = 1;
        spdk_env_init(&opts);

        spdk_nvme_retry_count = g_ceph_context->_conf->bdev_nvme_retry_count;
        if (spdk_nvme_retry_count < 0)
          spdk_nvme_retry_count = SPDK_NVME_DEFAULT_RETRY_COUNT;

        std::unique_lock l(probe_queue_lock);
        while (!stopping) {
          if (!probe_queue.empty()) {
            ProbeContext* ctxt = probe_queue.front();
            probe_queue.pop_front();
            r = spdk_nvme_probe(NULL, ctxt, probe_cb, attach_cb, NULL);
            if (r < 0) {
              ceph_assert(!ctxt->driver);
              derr << __func__ << " device probe nvme failed" << dendl;
            }
            ctxt->done = true;
            probe_queue_cond.notify_all();
          } else {
            probe_queue_cond.wait(l);
          }
        }
        for (auto p : probe_queue)
          p->done = true;
        probe_queue_cond.notify_all();
      }
    );
  }

  ProbeContext ctx{trid, this, nullptr, false};
  {
    std::unique_lock l(probe_queue_lock);
    probe_queue.push_back(&ctx);
    while (!ctx.done) {
      probe_queue_cond.wait(l);
    }
  }
  if (!ctx.driver) {
    return -1;
  }
  *driver = ctx.driver;

  return 0;
}

void io_complete(void *t, const struct spdk_nvme_cpl *completion)
{
  Task *task = static_cast<Task*>(t);
  IOContext *ctx = task->ctx;
  SharedDriverQueueData *queue = task->queue;

  ceph_assert(queue != NULL);
  ceph_assert(ctx != NULL);
//   ++queue->completed_op_seq;
  --queue->inflight_ops;
  --queue->current_queue_depth;
  queue->remove_from_submit_queue(task);
  if (task->command == IOCommand::WRITE_COMMAND) {
    ceph_assert(!spdk_nvme_cpl_is_error(completion));
    dout(20) << __func__ << " write/zero op successfully, left "
        //      << queue->queue_op_seq - queue->completed_op_seq << dendl;
             << queue->inflight_ops << dendl;
    // check waiting count before doing callback (which may
    // destroy this ioc).
//     if (ctx->priv) {
//       if (!--ctx->num_running) {
//         task->device->aio_callback(task->device->aio_callback_priv, ctx->priv);
//       }
//     } else {
//       ctx->try_aio_wake();
//     }
    task->release_segs(queue);
    delete task;
  } else if (task->command == IOCommand::READ_COMMAND) {
    ceph_assert(!spdk_nvme_cpl_is_error(completion));
    dout(20) << __func__ << " read op successfully" << dendl;
    task->fill_cb();
    task->release_segs(queue);
    // read submitted by AIO
    if (!task->return_code) {
      if (ctx->priv) {
	if (!--ctx->num_running) {
          task->device->aio_callback(task->device->aio_callback_priv, ctx->priv);
	}
      } else {
        ctx->try_aio_wake();
      }
      delete task;
    } else {
      if (Task* primary = task->primary; primary != nullptr) {
        delete task;
        if (!primary->ref)
          primary->return_code = 0;
      } else {
	  task->return_code = 0;
      }
      ctx->try_aio_wake();
    }
  } else {
//     ceph_assert(task->command == IOCommand::FLUSH_COMMAND);
//     ceph_assert(!spdk_nvme_cpl_is_error(completion));
//     dout(10) << __func__ << " flush op successfully" << dendl;
//     ctx->try_aio_wake();
//     task->return_code = 0;
  }
}

// ----------------
#undef dout_prefix
#define dout_prefix *_dout << "bdev(" << name << ") "

NVMEDevice::NVMEDevice(CephContext* cct, aio_callback_t cb, void *cbpriv)
  :   BlockDevice(cct, cb, cbpriv),
      driver(nullptr)
{
}

bool NVMEDevice::support(const std::string& path)
{
  char buf[PATH_MAX + 1];
  int r = ::readlink(path.c_str(), buf, sizeof(buf) - 1);
  if (r >= 0) {
    buf[r] = '\0';
    char *bname = ::basename(buf);
    if (strncmp(bname, SPDK_PREFIX, sizeof(SPDK_PREFIX)-1) == 0) {
      return true;
    }
  }
  return false;
}

int NVMEDevice::open(const string& p)
{
  dout(1) << __func__ << " path " << p << dendl;

  std::ifstream ifs(p);
  if (!ifs) {
    derr << __func__ << " unable to open " << p << dendl;
    return -1;
  }
  string val;
  std::getline(ifs, val);
  spdk_nvme_transport_id trid;
  if (int r = spdk_nvme_transport_id_parse(&trid, val.c_str()); r) {
    derr << __func__ << " unable to read " << p << ": " << cpp_strerror(r)
	 << dendl;
    return r;
  }
  if (int r = manager.try_get(trid, &driver); r < 0) {
    derr << __func__ << " failed to get nvme device with transport address " << trid.traddr << dendl;
    return r;
  }

  driver->register_device(this);
  block_size = driver->get_block_size();
  size = driver->get_size();
  name = trid.traddr;

  //nvme is non-rotational device.
  rotational = false;

  // round size down to an even block
  size &= ~(block_size - 1);

  dout(1) << __func__ << " size " << size << " (" << byte_u_t(size) << ")"
          << " block_size " << block_size << " (" << byte_u_t(block_size)
          << ")" << dendl;


  return 0;
}

void NVMEDevice::close()
{
  dout(1) << __func__ << dendl;

  name.clear();
  driver->remove_device(this);

  dout(1) << __func__ << " end" << dendl;
}

int NVMEDevice::collect_metadata(const string& prefix, map<string,string> *pm) const
{
  (*pm)[prefix + "rotational"] = "0";
  (*pm)[prefix + "size"] = stringify(get_size());
  (*pm)[prefix + "block_size"] = stringify(get_block_size());
  (*pm)[prefix + "driver"] = "NVMEDevice";
  (*pm)[prefix + "type"] = "nvme";
  (*pm)[prefix + "access_mode"] = "spdk";
  (*pm)[prefix + "nvme_serial_number"] = name;

  return 0;
}

void NVMEDevice::aio_submit(IOContext *ioc)
{
  dout(20) << __func__ << " ioc " << ioc << " pending "
           << ioc->num_pending.load() << " running "
           << ioc->num_running.load() << dendl;
  int pending = ioc->num_pending.load();
  Task *t = static_cast<Task*>(ioc->nvme_task_first);
  if (pending && t) {
    ioc->num_running += pending;
    ioc->num_pending -= pending;
    ceph_assert(ioc->num_pending.load() == 0);  // we should be only thread doing this
    // Only need to push the first entry
//     ioc->nvme_task_first = ioc->nvme_task_last = nullptr;

    if (queue_id == -1) {
      queue_id = ceph_gettid();
      dout(1) << "@@: queue_id=" << queue_id << dendl;
    }
    driver->get_queue(queue_id)->queue_task(t, pending);
//     thread_local SharedDriverQueueData queue_t = SharedDriverQueueData(this, driver);
//     queue_t._aio_handle(t, ioc);
  }
}

static void ioc_append_task(IOContext *ioc, Task *t)
{
  Task *first, *last;

  first = static_cast<Task*>(ioc->nvme_task_first);
  last = static_cast<Task*>(ioc->nvme_task_last);
  if (last)
    last->next = t;
  if (!first)
    ioc->nvme_task_first = t;
  ioc->nvme_task_last = t;
  ++ioc->num_pending;
}

static void write_split(
    NVMEDevice *dev,
    uint64_t off,
    bufferlist &bl,
    IOContext *ioc)
{
  uint64_t remain_len = bl.length(), begin = 0, write_size;
  Task *t;
  // This value may need to be got from configuration later.
//   uint64_t split_size = 131072; // 128KB.
  uint64_t split_size = (uint64_t)g_conf().get_val<uint64_t>("bluestore_spdk_io_split_size");

  while (remain_len > 0) {
    write_size = std::min(remain_len, split_size);
    t = new Task(dev, IOCommand::WRITE_COMMAND, off + begin, write_size);
    // TODO: if upper layer alloc memory with known physical address,
    // we can reduce this copy
    bl.splice(0, write_size, &t->bl);
    remain_len -= write_size;
    t->ctx = ioc;
    ioc_append_task(ioc, t);
    begin += write_size;
  }
}

static void make_read_tasks(
    NVMEDevice *dev,
    uint64_t aligned_off,
    IOContext *ioc, char *buf, uint64_t aligned_len, Task *primary,
    uint64_t orig_off, uint64_t orig_len)
{
  // This value may need to be got from configuration later.
//   uint64_t split_size = 131072; // 128KB.
  uint64_t split_size = (uint64_t)g_conf().get_val<uint64_t>("bluestore_spdk_io_split_size");
  uint64_t tmp_off = orig_off - aligned_off, remain_orig_len = orig_len;
  auto begin = aligned_off;
  const auto aligned_end = begin + aligned_len;

  for (; begin < aligned_end; begin += split_size) {
    auto read_size = std::min(aligned_end - begin, split_size);
    auto tmp_len = std::min(remain_orig_len, read_size - tmp_off);
    Task *t = nullptr;

    if (primary && (aligned_len <= split_size)) {
      t = primary;
    } else {
      t = new Task(dev, IOCommand::READ_COMMAND, begin, read_size, 0, primary);
    }

    t->ctx = ioc;

    // TODO: if upper layer alloc memory with known physical address,
    // we can reduce this copy
    t->fill_cb = [buf, t, tmp_off, tmp_len]  {
      t->copy_to_buf(buf, tmp_off, tmp_len);
    };

    ioc_append_task(ioc, t);
    remain_orig_len -= tmp_len;
    buf += tmp_len;
    tmp_off = 0;
  }
}

int NVMEDevice::aio_write(
    uint64_t off,
    bufferlist &bl,
    IOContext *ioc,
    bool buffered,
    int write_hint)
{
  uint64_t len = bl.length();
  dout(20) << __func__ << " " << off << "~" << len << " ioc " << ioc
           << " buffered " << buffered << dendl;
  ceph_assert(is_valid_io(off, len));

  write_split(this, off, bl, ioc);
  dout(5) << __func__ << " " << off << "~" << len << dendl;

  return 0;
}

int NVMEDevice::write(uint64_t off, bufferlist &bl, bool buffered, int write_hint)
{
  uint64_t len = bl.length();
  dout(20) << __func__ << " " << off << "~" << len << " buffered "
           << buffered << dendl;
  ceph_assert(off % block_size == 0);
  ceph_assert(len % block_size == 0);
  ceph_assert(len > 0);
  ceph_assert(off < size);
  ceph_assert(off + len <= size);

  IOContext ioc(cct, NULL);
  write_split(this, off, bl, &ioc);
  dout(5) << __func__ << " " << off << "~" << len << dendl;
  aio_submit(&ioc);
  ioc.aio_wait();
  return 0;
}

int NVMEDevice::read(uint64_t off, uint64_t len, bufferlist *pbl,
                     IOContext *ioc,
                     bool buffered)
{
  dout(5) << __func__ << " " << off << "~" << len << " ioc " << ioc << dendl;
  ceph_assert(is_valid_io(off, len));

  Task t(this, IOCommand::READ_COMMAND, off, len, 1);
  bufferptr p = buffer::create_small_page_aligned(len);
  char *buf = p.c_str();

  // for sync read, need to control IOContext in itself
  IOContext read_ioc(cct, nullptr);
  make_read_tasks(this, off, &read_ioc, buf, len, &t, off, len);
  dout(5) << __func__ << " " << off << "~" << len << dendl;
  aio_submit(&read_ioc);
  read_ioc.aio_wait();

  pbl->push_back(std::move(p));
  return t.return_code;
}

int NVMEDevice::aio_read(
    uint64_t off,
    uint64_t len,
    bufferlist *pbl,
    IOContext *ioc)
{
  dout(10) << __func__ << " " << off << "~" << len << " ioc " << ioc << dendl;
  ceph_assert(is_valid_io(off, len));
  bufferptr p = buffer::create_small_page_aligned(len);
  pbl->append(p);
  char* buf = p.c_str();

  make_read_tasks(this, off, ioc, buf, len, NULL, off, len);
  dout(5) << __func__ << " " << off << "~" << len << dendl;
  return 0;
}

int NVMEDevice::read_random(uint64_t off, uint64_t len, char *buf, bool buffered)
{
  ceph_assert(len > 0);
  ceph_assert(off < size);
  ceph_assert(off + len <= size);

  uint64_t aligned_off = p2align(off, block_size);
  uint64_t aligned_len = p2roundup(off+len, block_size) - aligned_off;
  dout(5) << __func__ << " " << off << "~" << len
          << " aligned " << aligned_off << "~" << aligned_len << dendl;
  IOContext ioc(g_ceph_context, nullptr);
  Task t(this, IOCommand::READ_COMMAND, aligned_off, aligned_len, 1);

  make_read_tasks(this, aligned_off, &ioc, buf, aligned_len, &t, off, len);
  aio_submit(&ioc);
  ioc.aio_wait();

  return t.return_code;
}

int NVMEDevice::flush()
{
  dout(10) << __func__ << " start" << dendl;

//   Task * t = new Task(this, IOCommand::FLUSH_COMMAND, 0, 0, 1);
//   IOContext ioc(cct, nullptr);
//   t->ctx = &ioc;
//   t->queue = nullptr;
//   ioc_append_task(&ioc, t);
//   aio_submit(&ioc);
//   ioc.aio_wait();
//   delete t;
//   dout(10) << __func__ << " end" << dendl;
  return 0;
}

int NVMEDevice::invalidate_cache(uint64_t off, uint64_t len)
{
  dout(5) << __func__ << " " << off << "~" << len << dendl;
  return 0;
}
