#include "libfatbat/logging.hpp"
//
#include "test/operation_context.hpp"

// ------------------------------------------------------------------
MAKE_LOGGER(testutil_log, "TestUtil")

// create a lambda we can use as a callback function that verifies the data in the buffer is correct
auto verify_buffer = [](void const* buffer, std::size_t message_size, rank_type this_rank,
                         std::uint8_t expected, char const* msg, rank_type remote_rank,
                         tag_type /*tag*/)    // NOLINT(readability-function-cognitive-complexity)
{
  LIBFATBAT_TRACE(testutil_log, "{:<20} {}", msg, libfatbat::log::mem_crc32(buffer, message_size));

  // verify the RMA/MSG buffer content: every byte must match the expected value
  auto* data = static_cast<uint8_t const*>(buffer);
  for (std::size_t i = 0; i < static_cast<std::size_t>(message_size); ++i)
  {
    if (data[i] != static_cast<uint8_t>(expected))
    {
      LIBFATBAT_ERROR(testutil_log,
          "{:<20} rank {} Buffer validation failed: src {} index {} value {} expected {}", msg,
          this_rank, remote_rank, i, data[i], static_cast<uint8_t>(expected));
      throw std::runtime_error("Buffer validation failed");
    }
  }
};