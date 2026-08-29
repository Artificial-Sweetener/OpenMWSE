#include <components/diagnostics/commandqueue.hpp>

#include <gtest/gtest.h>

namespace
{
    TEST(DiagnosticCommandQueue, EnforcesCapacityAndUniqueRequestIds)
    {
        Diagnostics::CommandQueue queue(1);
        EXPECT_EQ(queue.enqueue({ 1, Diagnostics::CommandType::Hello, {} }), Diagnostics::ErrorCode::None);
        EXPECT_EQ(queue.enqueue({ 2, Diagnostics::CommandType::Hello, {} }), Diagnostics::ErrorCode::QueueFull);
        ASSERT_EQ(queue.drain().size(), 1);
        EXPECT_EQ(queue.enqueue({ 1, Diagnostics::CommandType::Hello, {} }), Diagnostics::ErrorCode::InvalidFrame);
        EXPECT_FALSE(queue.consumeCancellation(1));
        EXPECT_EQ(queue.enqueue({ 1, Diagnostics::CommandType::Hello, {} }), Diagnostics::ErrorCode::None);
    }

    TEST(DiagnosticCommandQueue, CancelsPendingAndExecutingRequests)
    {
        Diagnostics::CommandQueue queue;
        ASSERT_EQ(queue.enqueue({ 1, Diagnostics::CommandType::Hello, {} }), Diagnostics::ErrorCode::None);
        EXPECT_TRUE(queue.cancel(1));
        EXPECT_TRUE(queue.drain().empty());

        ASSERT_EQ(queue.enqueue({ 2, Diagnostics::CommandType::Hello, {} }), Diagnostics::ErrorCode::None);
        ASSERT_EQ(queue.drain().size(), 1);
        EXPECT_TRUE(queue.cancel(2));
        EXPECT_TRUE(queue.consumeCancellation(2));
    }

    TEST(DiagnosticCommandQueue, ClosingDiscardsAndRejectsWork)
    {
        Diagnostics::CommandQueue queue;
        ASSERT_EQ(queue.enqueue({ 1, Diagnostics::CommandType::Hello, {} }), Diagnostics::ErrorCode::None);
        queue.close();
        EXPECT_TRUE(queue.isClosed());
        EXPECT_TRUE(queue.drain().empty());
        EXPECT_EQ(queue.enqueue({ 2, Diagnostics::CommandType::Hello, {} }), Diagnostics::ErrorCode::InvalidState);
    }
}
