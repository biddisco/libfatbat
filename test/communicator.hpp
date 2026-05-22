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
#include "libfatbat/memory_segment.hpp"
//
#include "test/controller.hpp"
#include "test/operation_context.hpp"

// --------------------------------------------------------------------
MAKE_LOGGER(comm_log, "Comm")

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
    else { return region_type(m_domain, ptr, size, false, nullptr, device_id); }
  }
};

// --------------------------------------------------------------------
// needed by hwmalloc heap creation code to register memory segments
// --------------------------------------------------------------------
inline memory_context::region_type register_memory(
    memory_context& c, void* const ptr, std::size_t size)
{
  LIBFATBAT_SCOPE(libfatbat::memrgn_log, "{} {} {:#10x} {:05}", __func__, ptr, size, -2);
  return c.make_region(ptr, size, -2);
}

inline memory_context::region_type register_device_memory(
    memory_context& c, int device_id, void* ptr, std::size_t size)
{
  LIBFATBAT_SCOPE(libfatbat::memrgn_log, "{} {} {:#10x} {:05}", __func__, ptr, size, device_id);
  return c.make_region(ptr, size, device_id);
}

// --------------------------------------------------------------------
//
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
  }

  // --------------------------------------------------------------------
  ~communicator()
  {    //
    clear_callback_queues();
  }

  // --------------------------------------------------------------------
  inline operation_context* make_operation_context(request_callback_type&& cb)
  {
    return operation_context::acquire(std::move(cb));
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
  void write_remote(region_type const& send_region, std::size_t size, fi_addr_t rem_rank_,
      uint64_t remote_addr, uint64_t remote_key, operation_context* ctxt)
  {
    m_controller->writes_posted_++;
    LIBFATBAT_DEBUG(comm_log,
        "{:<20} {:02} {} context {:p} tx endpoint {:p} size {:#10x} rem_addr {:#018x} rem_key "
        "{:#08x}",
        "write_remote", rem_rank_, send_region, (void*) (ctxt), (void*) (m_tx_endpoint.get_ep()),
        size, remote_addr, remote_key);
    execute_fi_function(fi_write, "fi_write", m_tx_endpoint.get_ep(), send_region.get_address(),
        size, send_region.get_local_key(), rem_rank_, remote_addr, remote_key, ctxt);
  }

  // --------------------------------------------------------------------
  operation_context* read(memory_context::heap_type::pointer const& ptr, std::size_t size,
      rank_type dst, void* remote_addr, uint64_t remote_key, request_callback_type&& cb)
  {
    LIBFATBAT_SCOPE(comm_log, "{} {}", (void*) (this), __func__);

#ifdef LIBFATBAT_HAVE_GPU_SUPPORT
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
  operation_context* write(memory_context::heap_type::pointer const& ptr, std::size_t size,
      rank_type dst, uint64_t remote_addr, uint64_t remote_key, request_callback_type&& cb)
  {
    LIBFATBAT_SCOPE(comm_log, "{} {}", (void*) (this), __func__);

#ifdef LIBFATBAT_HAVE_GPU_SUPPORT
    auto const& reg = ptr.on_device() ? ptr.device_handle() : ptr.handle();
#else
    auto const& reg = ptr.handle();
#endif

    if (cb) { cb = std::bind(std::move(cb), dst, 0); }
    auto request = make_operation_context(std::move(cb));

    write_remote(reg, size, fi_addr_t(dst), remote_addr, remote_key, request);
    return request;
  }

  // --------------------------------------------------------------------
  void write_data_remote(region_type const& send_region, std::size_t size, fi_addr_t rem_rank_,
      uint64_t remote_addr, uint64_t remote_key, uint64_t imm_data, operation_context* ctxt)
  {
    m_controller->writes_posted_++;
    LIBFATBAT_DEBUG(comm_log,
        "{:<20} {:02} {} context {:p} tx endpoint {:p} size {:#10x} rem_addr {:#018x} rem_key "
        "{:#08x} imm_data {:#016x}",
        "write_data_remote", rem_rank_, send_region, (void*) (ctxt),
        (void*) (m_tx_endpoint.get_ep()), size, remote_addr, remote_key, imm_data);
    execute_fi_function(fi_writedata, "fi_writedata", m_tx_endpoint.get_ep(),
        send_region.get_address(), size, send_region.get_local_key(), imm_data, rem_rank_,
        remote_addr, remote_key, ctxt);
  }

  // --------------------------------------------------------------------
  operation_context* write_data(memory_context::heap_type::pointer const& ptr, std::size_t size,
      rank_type dst, uint64_t remote_addr, uint64_t remote_key, uint64_t imm_data,
      request_callback_type&& cb)
  {
    LIBFATBAT_SCOPE(comm_log, "{} {}", (void*) (this), __func__);

#ifdef LIBFATBAT_HAVE_GPU_SUPPORT
    auto const& reg = ptr.on_device() ? ptr.device_handle() : ptr.handle();
#else
    auto const& reg = ptr.handle();
#endif
    if (cb) { cb = std::bind(std::move(cb), dst, 0); }
    auto request = make_operation_context(std::move(cb));
    write_data_remote(reg, size, fi_addr_t(dst), remote_addr, remote_key, imm_data, request);
    return request;
  }

  // --------------------------------------------------------------------
  // Write with remote CQ data and delivery-complete semantics.
  operation_context* write_data_delivery(memory_context::heap_type::pointer const& ptr,
      std::size_t size, rank_type dst, uint64_t remote_addr, uint64_t remote_key, uint64_t imm_data,
      request_callback_type&& cb)
  {
    LIBFATBAT_SCOPE(comm_log, "{} {}", (void*) (this), __func__);

#ifdef LIBFATBAT_HAVE_GPU_SUPPORT
    auto const& reg = ptr.on_device() ? ptr.device_handle() : ptr.handle();
#else
    auto const& reg = ptr.handle();
#endif

    if (cb) { cb = std::bind(std::move(cb), dst, 0); }
    auto request = make_operation_context(std::move(cb));

    struct iovec iov = {reg.get_address(), size};
    void* desc = reg.get_local_key();
    struct fi_rma_iov rma_iov = {remote_addr, size, remote_key};
    struct fi_msg_rma msg = {};
    msg.msg_iov = &iov;
    msg.desc = &desc;
    msg.iov_count = 1;
    msg.addr = fi_addr_t(dst);
    msg.rma_iov = &rma_iov;
    msg.rma_iov_count = 1;
    msg.context = request;
    msg.data = imm_data;

    m_controller->writes_posted_++;
    LIBFATBAT_DEBUG(comm_log,
        "{:<20} dst {} size {} remote_addr {:#018x} remote_key {:#08x} imm_data {:#016x} context "
        "{:p}",
        "write_data_delivery", dst, size, remote_addr, remote_key, imm_data, (void*) request);

    bool delivery_mode_supported = true;
    while (true)
    {
      ssize_t ret = fi_writemsg(
          m_tx_endpoint.get_ep(), &msg, uint64_t(FI_REMOTE_CQ_DATA | FI_DELIVERY_COMPLETE));
      if (ret == 0) { return request; }
      if (ret == -FI_EAGAIN)
      {
        m_controller->poll_for_work_completions(this);
        continue;
      }
      if (ret == -FI_EBADFLAGS || ret == -FI_EOPNOTSUPP || ret == -FI_ENOPROTOOPT ||
          ret == -FI_ENOSYS || ret == -FI_EINVAL)
      {
        delivery_mode_supported = false;
        LIBFATBAT_WARN(comm_log,
            "{:<20} provider rejected FI_DELIVERY_COMPLETE flags, falling back to fi_writedata",
            "write_data_delivery");
        break;
      }
      if (ret == -FI_ENOENT)
      {
        LIBFATBAT_ERROR(comm_log, "{:<20}", "No destination endpoint, terminating.");
        std::terminate();
      }
      throw libfatbat::fabric_error(int(ret), "fi_writemsg");
    }

    if (!delivery_mode_supported)
    {
      execute_fi_function(fi_writedata, "fi_writedata", m_tx_endpoint.get_ep(), reg.get_address(),
          size, reg.get_local_key(), imm_data, fi_addr_t(dst), remote_addr, remote_key, request);
    }
    return request;
  }

  // --------------------------------------------------------------------
  // Perform an inject_write (RMA write, no completion, no context)
  void inject_write(
      void const* buf, std::size_t size, rank_type dst, uint64_t remote_addr, uint64_t remote_key)
  {
    LIBFATBAT_DEBUG(comm_log, "{:<20} dst {} size {} remote_addr {:#018x} remote_key {:#08x}",
        "inject_write", dst, size, remote_addr, remote_key);
    execute_fi_function(fi_inject_write, "fi_inject_write", m_tx_endpoint.get_ep(), buf, size, dst,
        remote_addr, remote_key);
  }

  // --------------------------------------------------------------------
  // Small control-path signal write.
  // We intentionally use fi_inject_write here because some providers reject
  // fi_writemsg(FI_INJECT|FI_FENCE) for RMA message ops.
  operation_context* inject_write_fenced(std::uint64_t flag, rank_type dst, uint64_t remote_addr,
      uint64_t remote_key, request_callback_type&& cb)
  {
    LIBFATBAT_SCOPE(comm_log, "{} {}", (void*) (this), __func__);
    LIBFATBAT_DEBUG(comm_log, "{:<20} dst {} size {} remote_addr {:#018x} remote_key {:#08x}",
        "inject_write_fenced", dst, sizeof(std::uint64_t), remote_addr, remote_key);
    execute_fi_function(fi_inject_write, "fi_inject_write", m_tx_endpoint.get_ep(), &flag,
        sizeof(std::uint64_t), fi_addr_t(dst), remote_addr, remote_key);
    if (cb) { cb(dst, 0); }
    return nullptr;
  }

  // --------------------------------------------------------------------
  // Preserved legacy variant using fi_writemsg(FI_INJECT|FI_FENCE).
  // Some providers (e.g. CXI) may reject this with -FI_EINVAL.
  operation_context* inject_write_fenced_writemsg_legacy(std::uint64_t flag, rank_type dst,
      uint64_t remote_addr, uint64_t remote_key, request_callback_type&& cb)
  {
    LIBFATBAT_SCOPE(comm_log, "{} {}", (void*) (this), __func__);

    if (cb) { cb = std::bind(std::move(cb), dst, 0); }
    auto request = make_operation_context(std::move(cb));

    struct iovec iov = {.iov_base = &flag, .iov_len = sizeof(std::uint64_t)};
    struct fi_rma_iov rma_iov = {remote_addr, sizeof(std::uint64_t), remote_key};
    struct fi_msg_rma msg = {
        .msg_iov = &iov,
        .desc = nullptr,
        .iov_count = 1,
        .addr = fi_addr_t(dst),
        .rma_iov = &rma_iov,
        .rma_iov_count = 1,
        .context = request,
        .data = 0,
    };

    m_controller->writes_posted_++;
    LIBFATBAT_DEBUG(comm_log,
        "{:<20} dst {} size {} remote_addr {:#018x} remote_key {:#08x} context {:p}",
        "write_fenced_legacy", dst, sizeof(std::uint64_t), remote_addr, remote_key,
        (void*) request);
    execute_fi_function(
        fi_writemsg, "fi_writemsg", m_tx_endpoint.get_ep(), &msg, uint64_t(FI_INJECT | FI_FENCE));
    return request;
  }

  // --------------------------------------------------------------------
  operation_context* send(memory_context::heap_type::pointer const& ptr, std::size_t size,
      rank_type dst, tag_type tag, request_callback_type&& cb)
  {
    LIBFATBAT_SCOPE(comm_log, "{} {}", (void*) (this), __func__);
    std::uint64_t stag = make_tag64(tag, 0);    // this->m_context->get_context_tag());

#ifdef LIBFATBAT_HAVE_GPU_SUPPORT
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
#ifdef LIBFATBAT_HAVE_GPU_SUPPORT
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

#ifdef LIBFATBAT_HAVE_GPU_SUPPORT
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

#ifdef LIBFATBAT_HAVE_GPU_SUPPORT
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
  // these queues were added for the ghex/oomph framework and should be abstracted into
  // a more general callback management system, they are unused currently.
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
