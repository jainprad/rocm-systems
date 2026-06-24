// Copyright (c) 2026 Advanced Micro Devices, Inc.
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr const char *kKfdPath = "/dev/kfd";
constexpr const char *kTopologyNodesPath = "/sys/devices/virtual/kfd/kfd/topology/nodes";
constexpr uint32_t kGuestGpuId = 38144;
constexpr int kChildCount = 4;

bool read_gpu_id(const std::string &path, uint32_t *gpu_id) {
  FILE *file = fopen(path.c_str(), "r");
  if (!file)
    return false;

  unsigned parsed = 0;
  const int scanned = fscanf(file, "%u", &parsed);
  fclose(file);
  if (scanned != 1)
    return false;

  *gpu_id = static_cast<uint32_t>(parsed);
  return true;
}

bool guest_gpu_is_visible() {
  DIR *dir = opendir(kTopologyNodesPath);
  if (!dir)
    return false;

  bool found = false;
  while (auto *entry = readdir(dir)) {
    if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0)
      continue;

    uint32_t gpu_id = 0;
    std::string gpu_id_path = std::string(kTopologyNodesPath) + "/" + entry->d_name + "/gpu_id";
    if (read_gpu_id(gpu_id_path, &gpu_id) && gpu_id == kGuestGpuId) {
      found = true;
      break;
    }
  }

  closedir(dir);
  return found;
}

int child_process() {
  int fd = open(kKfdPath, O_RDWR | O_CLOEXEC);
  if (fd < 0)
    return 2;

  const bool visible = guest_gpu_is_visible();
  close(fd);
  return visible ? 0 : 3;
}

TEST(GuestKfdMultiprocessTest, ForkedChildrenDoNotRemoveParentOverlay) {
  if (access(kKfdPath, R_OK | W_OK) != 0)
    GTEST_SKIP() << kKfdPath << " is not available: " << std::strerror(errno);

  int parent_fd = open(kKfdPath, O_RDWR | O_CLOEXEC);
  ASSERT_GE(parent_fd, 0);
  ASSERT_TRUE(guest_gpu_is_visible()) << "guest GPU was not visible before fork";

  std::vector<pid_t> children;
  children.reserve(kChildCount);
  for (int i = 0; i < kChildCount; ++i) {
    pid_t pid = fork();
    ASSERT_GE(pid, 0) << "fork failed: " << std::strerror(errno);

    if (pid == 0)
      _exit(child_process());

    children.push_back(pid);
  }

  for (pid_t pid : children) {
    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status)) << "child " << pid << " did not exit normally";
    EXPECT_EQ(WEXITSTATUS(status), 0) << "child " << pid << " failed";
  }

  EXPECT_TRUE(guest_gpu_is_visible()) << "parent guest topology disappeared after forked children";
  close(parent_fd);
}

} // namespace
