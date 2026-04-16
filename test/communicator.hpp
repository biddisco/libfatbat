/*
 * libfatbat
 *
 * Copyright (c) 2024-2025, ETH Zurich
 * All rights reserved.
 *
 * Please, refer to the LICENSE file in the root directory.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#pragma once

#include <cstdint>
#include <functional>
//
#include <boost/lockfree/queue.hpp>
#include <hwmalloc/heap.hpp>
//
#include "libfatbat/controller_base.hpp"
#include "libfatbat/memory_region.hpp"
//
#include "controller.hpp"
#include "operation_context.hpp"

// --------------------------------------------------------------------
inline auto comm_log = libfatbat::log::create("Comm");

// --------------------------------------------------------------------
// A convenience memory context to manage memory regions
// we use this to connect hwmalloc with our code
// --------------------------------------------------------------------
struct memory_context
{
  using heap_type = hwmalloc::heap<memory_context>;
  using region_type = libfatbat::memory_segment;
  using domain_type = region_type::provider_domain;
  // using device_region_type = libfatbat::memory_segment;

  test_controller* m_controller;
  domain_type* m_domain;

  memory_context(test_controller* controller)
    : m_controller(controller)
    , m_domain(m_controller->get_domain())
  {
  }

  ~memory_context() {}

  region_type make_region(void* const ptr, std::size_t size, int device_id)
  {
    if (m_controller->get_mrbind())
    {
      void* endpoint = m_controller->get_rx_endpoint().get_ep();
      return region_type(m_domain, ptr, size, true, endpoint, device_id);
    }
    else
    {
      return region_type(m_domain, ptr, size, false, nullptr, device_id);
    }
  }
};

// --------------------------------------------------------------------
// needed by hwmalloc heap creation code to register memory segments
// --------------------------------------------------------------------
inline memory_context::region_type register_memory(
    memory_context& c, void* const ptr, std::size_t size)
{
  return c.make_region(ptr, size, -2);
}

// --------------------------------------------------------------------
// a lockfree queue to hold completions
// --------------------------------------------------------------------
struct communicator
{
  //
  using segment_type = libfatbat::memory_segment;
  using region_type = segment_type::handle_type;

  using callback_queue = boost::lockfree::queue<operation_context*,
      boost::lockfree::fixed_sized<false>, boost::lockfree::allocator<std::allocator<void>>>;

  constexpr static std::size_t max_callback_queue_size_ = 256;

  public:
  test_controller* m_controller;
  libfatbat::endpoint_wrapper m_tx_endpoint;
  libfatbat::endpoint_wrapper m_rx_endpoint;
  //
  callback_queue queue_cache;
  callback_queue m_send_cb_queue;
  callback_queue m_recv_cb_queue;
  //
  rank_type m_rank = -1;
  rank_type m_size = -1;

  // --------------------------------------------------------------------
  communicator(test_controller* controller, rank_type rank, rank_type size)
    : m_controller(controller)
    , queue_cache(2 * max_callback_queue_size_)
    , m_send_cb_queue(max_callback_queue_size_)
    , m_recv_cb_queue(max_callback_queue_size_)
    , m_rank(rank)
    , m_size(size)
  {
    m_tx_endpoint = m_controller->get_tx_endpoint();
    m_rx_endpoint = m_controller->get_rx_endpoint();
    // fill cache with empty request objects taken from the heap so that we can avoid allocations at runtime
    for (int i = 0; i < 2 * max_callback_queue_size_; ++i)
    {
      queue_cache.push(new operation_context());
    }
  }

  // --------------------------------------------------------------------
  ~communicator()
  {    //
    clear_callback_queues();
  }

  // --------------------------------------------------------------------
  inline operation_context* make_operation_context(request_callback_type&& cb)
  {
    operation_context* request;
    while (!queue_cache.pop(request))
    {
      LIBFATBAT_ERROR(
          comm_log, "{:<20} {}", "make_operation_context", "unable to get request from cache");
    }
    request->m_callback = std::move(cb);
    return request;
  }

  // --------------------------------------------------------------------
  rank_type rank() const { return m_rank; }
  rank_type size() const { return m_size; }

  // --------------------------------------------------------------------
  // generate a tag with 0xRRRRRRRRtttttttt rank, tag.
  // original tag can be 32bits, then we add 32bits of rank info.
  // Note - this tag setting should not be used without unique context info
  inline std::uint64_t make_tag64(std::uint32_t tag, /*std::uint32_t rank, */ std::uintptr_t ctxt)
  {
    return (
        ((ctxt & 0x0000'0000'00FF'FFFF) << 24) | ((std::uint64_t(tag) & 0x0000'0000'00FF'FFFF)));
  }

  // --------------------------------------------------------------------
  template <typename Func, typename... Args>
  inline void execute_fi_function(Func F, char const* msg, Args&&... args)
  {
    bool ok = false;
    while (!ok)
    {
      ssize_t ret = F(std::forward<Args>(args)...);
      if (ret == 0) { return; }
      else if (ret == -FI_EAGAIN)
      {
        LIBFATBAT_TRACE(comm_log, "{:<20} Reposting : {}", "FI_EAGAIN",
            msg);    // , std::forward<Args>(args)...);
        // no point stressing the system
        m_controller->poll_for_work_completions(this);
      }
      else if (ret == -FI_ENOENT)
      {
        // if a node has failed, we can in principle recover
        // @TODO : put something better here to recover from error
        LIBFATBAT_ERROR(comm_log, "{:<20}", "No destination endpoint, terminating.");
        std::terminate();
      }
      else if (ret) { throw libfatbat::fabric_error(int(ret), msg); }
    }
  }

  // --------------------------------------------------------------------
  // this takes a pinned memory region and sends it
  void send_tagged_region(region_type const& send_region, std::size_t size, fi_addr_t dst_addr_,
      uint64_t tag_, operation_context* ctxt)
  {
    LIBFATBAT_DEBUG(comm_log, "{:<20} {:02} {} tag {} context {:p} tx endpoint {:p}",
        "send_tagged_region", dst_addr_, send_region, tag_, (void*) (ctxt),
        (void*) (m_tx_endpoint.get_ep()));
    execute_fi_function(fi_tsend, "fi_tsend", m_tx_endpoint.get_ep(), send_region.get_address(),
        size, send_region.get_local_key(), dst_addr_, tag_, ctxt);
  }

  // --------------------------------------------------------------------
  // this takes a pinned memory region and sends it using inject instead of send
  void inject_tagged_region(
      region_type const& send_region, std::size_t size, fi_addr_t dst_addr_, uint64_t tag_)
  {
    LIBFATBAT_DEBUG(comm_log, "{:<20} {} {} tag {} tx endpoint {:p}", "inject tagged", dst_addr_,
        send_region, tag_, (void*) (m_tx_endpoint.get_ep()));
    execute_fi_function(fi_tinject, "fi_tinject", m_tx_endpoint.get_ep(), send_region.get_address(),
        size, dst_addr_, tag_);
  }

  // --------------------------------------------------------------------
  // the receiver posts a single receive buffer to the queue, attaching
  // itself as the context, so that when a message is received
  // the owning receiver is called to handle processing of the buffer
  void recv_tagged_region(region_type const& recv_region, std::size_t size, fi_addr_t src_addr_,
      uint64_t tag_, operation_context* ctxt)
  {
    LIBFATBAT_DEBUG(comm_log, "{:<20} {:02} {} tag {} context {:p} rx endpoint {:p}",
        "recv_tagged_region", src_addr_, recv_region, tag_, (void*) (ctxt),
        (void*) (m_rx_endpoint.get_ep()));
    constexpr uint64_t ignore = 0;
    execute_fi_function(fi_trecv, "fi_trecv", m_rx_endpoint.get_ep(), recv_region.get_address(),
        size, recv_region.get_local_key(), src_addr_, tag_, ignore, ctxt);
    // if (l.owns_lock()) l.unlock();
  }

  // --------------------------------------------------------------------
  void read_remote(region_type const& recv_region, std::size_t size, fi_addr_t rem_rank_,
      void* remote_addr, uint64_t remote_key, operation_context* ctxt)
  {
    m_controller->reads_posted_++;
    LIBFATBAT_DEBUG(comm_log,
        "{:<20} {:02} {} context {:p} rx endpoint {:p} size {:#10x} rem_addr {:p} rem_key {:#08x}",
        "read_remote", rem_rank_, recv_region, (void*) (ctxt), (void*) (m_tx_endpoint.get_ep()),
        size, remote_addr, remote_key);
    execute_fi_function(fi_read, "fi_read", m_tx_endpoint.get_ep(), recv_region.get_address(), size,
        recv_region.get_local_key(), rem_rank_, (uint64_t) (remote_addr), remote_key, ctxt);
  }

  // --------------------------------------------------------------------
  operation_context* read(memory_context::heap_type::pointer const& ptr, std::size_t size,
      rank_type dst, void* remote_addr, uint64_t remote_key, request_callback_type&& cb)
  {
    LIBFATBAT_SCOPE(comm_log, "{} {}", (void*) (this), __func__);

#if LIBFATBAT_ENABLE_DEVICE
    auto const& reg = ptr.on_device() ? ptr.device_handle() : ptr.handle();
#else
    auto const& reg = ptr.handle();
#endif

    if (cb) { cb = std::bind(std::move(cb), dst, 0); }
    auto request = make_operation_context(std::move(cb));

    read_remote(reg, size, fi_addr_t(dst), remote_addr, remote_key, request);
    return request;
  }

  // --------------------------------------------------------------------
  operation_context* send(memory_context::heap_type::pointer const& ptr, std::size_t size,
      rank_type dst, tag_type tag, request_callback_type&& cb)
  {
    LIBFATBAT_SCOPE(comm_log, "{} {}", (void*) (this), __func__);
    std::uint64_t stag = make_tag64(tag, 0);    // this->m_context->get_context_tag());

#if LIBFATBAT_ENABLE_DEVICE
    auto const& reg = ptr.on_device() ? ptr.device_handle() : ptr.handle();
#else
    auto const& reg = ptr.handle();
#endif

    m_controller->sends_posted_++;

    // use optimized inject if msg is very small
    if (size <= m_controller->get_tx_inject_size())
    {
      // @todo check reached_recursion_depth() : auto inc = recursion();
      // inject will return immediately, so we do not pass a context, instead return a "ready state"
      inject_tagged_region(reg, size, fi_addr_t(dst), stag);
      // invoke the callback right away
      m_controller->sends_complete_++;
      if (cb) cb(dst, tag);
      return nullptr;
    }

    if (cb) { cb = std::bind(std::move(cb), dst, tag); }
    // construct request which is also an operation context
    auto request = make_operation_context(std::move(cb));

    LIBFATBAT_DEBUG(comm_log,
        "{:<20} thisrank {} src/dst {} reg:{} tag {} stag {:#08x} addr {} size {} reg "
        "size {:06} op_ctx {:p} req {:p}",
        "send", rank(), dst, reg, tag, stag, (void*) (reg.get_address()), size, reg.get_size(),
        (void*) request, (void*) request);
#if LIBFATBAT_ENABLE_DEVICE
    if (!ptr.on_device())
    {
      LIBFATBAT_DEBUG(comm_log, "{:<20} mem {}", "send region CRC32",
          libfatbat::log::mem_crc32(reg.get_address(), size));
    }
#endif

    send_tagged_region(reg, size, fi_addr_t(dst), stag, request);
    return request;
  }

  // --------------------------------------------------------------------
  operation_context* recv(memory_context::heap_type::pointer& ptr, std::size_t size, rank_type src,
      tag_type tag, request_callback_type&& cb)
  {
    LIBFATBAT_SCOPE(comm_log, "{} {}", (void*) (this), __func__);
    std::uint64_t stag = make_tag64(tag, 0);    // this->m_context->get_context_tag());

#if LIBFATBAT_ENABLE_DEVICE
    auto const& reg = ptr.on_device() ? ptr.device_handle() : ptr.handle();
#else
    auto const& reg = ptr.handle();
#endif

    m_controller->recvs_posted_++;
    auto request = make_operation_context(std::move(cb));

    LIBFATBAT_DEBUG(comm_log,
        "{:<20} thisrank {} src/dst {} tag {} stag {:#08x} addr {:p} size {:#06x} reg "
        "size {:#06x} op_ctx {:p} req {:p}",
        "recv", rank(), src, tag, stag, (void*) (reg.get_address()), size, reg.get_size(),
        (void*) request, (void*) request);

#if LIBFATBAT_ENABLE_DEVICE
    if (!ptr.on_device())
    {
      LIBFATBAT_DEBUG(comm_log, "{:<20} mem {}", "recv region CRC32",
          libfatbat::log::mem_crc32(reg.get_address(), size));
    }
#endif

    recv_tagged_region(reg, size, fi_addr_t(src), stag, request);
    return request;
  }

  // --------------------------------------------------------------------
  // progress function that can be called at application level
  void progress()
  {
    m_controller->poll_for_work_completions(this);
    clear_callback_queues();
  }

  // --------------------------------------------------------------------
  void clear_callback_queues()
  {
    // work through ready callbacks, which were pushed to the queue
    // (by other threads)
    m_send_cb_queue.consume_all([](operation_context* req) {
      LIBFATBAT_SCOPE(comm_log, "{} {:p}", "m_send_cb_queue.consume_all", (void*) (req));
      req->invoke_cb();
    });

    m_recv_cb_queue.consume_all([](operation_context* req) {
      LIBFATBAT_SCOPE(comm_log, "{} {:p}", "m_recv_cb_queue.consume_all", (void*) (req));
      req->invoke_cb();
    });
  }
};
