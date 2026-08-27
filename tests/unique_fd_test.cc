// Test cases 91-94 from task.md.

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <type_traits>
#include <utility>

#include <gtest/gtest.h>

#include "base/unique_fd.h"

using namespace shardkv;

namespace {

// A cheap descriptor to own. /dev/null always opens and closing it costs
// nothing.
int openScratch() {
  const int fd = ::open("/dev/null", O_RDONLY);
  EXPECT_GE(fd, 0);
  return fd;
}

bool isOpen(int fd) { return ::fcntl(fd, F_GETFD) != -1; }

}  // namespace

// 91
TEST(UniqueFdTest, ClosesOnDestruction) {
  int raw = -1;
  {
    UniqueFd fd(openScratch());
    raw = fd.get();
    ASSERT_GE(raw, 0);
    EXPECT_TRUE(isOpen(raw));
  }
  errno = 0;
  EXPECT_FALSE(isOpen(raw));
  EXPECT_EQ(errno, EBADF);
}

// 92
TEST(UniqueFdTest, MoveTransfersOwnership) {
  int raw = -1;
  {
    UniqueFd source(openScratch());
    raw = source.get();
    ASSERT_GE(raw, 0);

    UniqueFd target(std::move(source));
    EXPECT_EQ(target.get(), raw);
    EXPECT_EQ(source.get(), -1) << "a moved-from UniqueFd owns nothing";
    EXPECT_TRUE(isOpen(raw)) << "moving must not close";
  }
  EXPECT_FALSE(isOpen(raw)) << "closed exactly once, by the new owner";
}

// 93 -- a descriptor has exactly one owner.
//
// Asserted at run time on the same traits a static_assert would use, rather
// than with a static_assert. A static_assert cannot be a failing test: it
// breaks the build instead of reporting, so the suite could not be run at all
// while the type is unwritten. The guarantee is identical; only the moment it
// is checked differs.
TEST(UniqueFdTest, IsNotCopyable) {
  EXPECT_FALSE(std::is_copy_constructible_v<UniqueFd>)
      << "UniqueFd must not be copy constructible";
  EXPECT_FALSE(std::is_copy_assignable_v<UniqueFd>)
      << "UniqueFd must not be copy assignable";
}

// 94
TEST(UniqueFdTest, ReleaseGivesUpOwnership) {
  int raw = -1;
  {
    UniqueFd fd(openScratch());
    raw = fd.release();
    ASSERT_GE(raw, 0);
    EXPECT_EQ(fd.get(), -1);
  }
  EXPECT_TRUE(isOpen(raw)) << "release means the caller owns it now";
  ::close(raw);
}
