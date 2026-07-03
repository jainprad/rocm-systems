////////////////////////////////////////////////////////////////////////////////
//
// Copyright © Advanced Micro Devices, Inc., or its affiliates.
// SPDX-License-Identifier: NCSA
//
////////////////////////////////////////////////////////////////////////////////

#include <cerrno>
#include <cstring>
#include <algorithm>
#include <vector>

#include "core/inc/amd_drm_driver.h"
#include "core/inc/amd_aql_queue.h"
#include "core/inc/amd_gpu_agent.h"

namespace rocr {
namespace AMD {

DrmDriver::DrmDriver(std::string devnode_name)
    : KfdDriver(core::DriverType::DRM, std::move(devnode_name)) {}

hsa_status_t DrmDriver::DiscoverDriver(std::unique_ptr<core::Driver>& driver) {
  auto tmp_driver = std::unique_ptr<core::Driver>(new DrmDriver("/dev/kfd"));

  if (core::Runtime::runtime_singleton_->flag().enable_drm()) {
    if (tmp_driver->Open() == HSA_STATUS_SUCCESS) {
      driver = std::move(tmp_driver);
      return HSA_STATUS_SUCCESS;
    }
  }

  return HSA_STATUS_ERROR;
}

hsa_status_t DrmDriver::CreateQueue(const CreateQueueInParams *queueIn, CreateQueueOutParams *queueOut) {
  if (!queueIn || !queueOut) {
    debug_print("queueIn or queueOut is NULL!\n");
    return HSA_STATUS_ERROR_INVALID_ARGUMENT;
  }

  auto * drmQueueIn = static_cast<const DrmDriver::CreateQueueInParams *>(queueIn);
  auto * drmQueueOut = static_cast<DrmDriver::CreateQueueOutParams *>(queueOut);

  GpuAgent & gpu_agent = const_cast<GpuAgent&>(drmQueueIn->gpu_agent);
  const queue_type type = drmQueueIn->type;

  void *mqd_in = NULL;
  uint32_t ip_type = 0;

  __u32 doorbell_handle = 0;
  __u32 doorbell_offset = 0;
  uint64_t *queueDoorbellPtr = NULL;
  __u32 queue_id = 0;
  __u32 flags = 0;

  assert(drmQueueIn);
  assert(drmQueueOut);

  // Struct used to pass some parameters for the queue
  struct drm_amdgpu_userq_mqd_compute_gfx11 compute_mqd = { 0 };
  struct drm_amdgpu_userq_mqd_sdma_gfx11 sdma_mqd = { 0 };

  if (type == AQL_QUEUE) {
    compute_mqd.eop_va = drmQueueIn->extra_va;
    compute_mqd.ctx_save_area_addr = drmQueueIn->cwsr_va;
    compute_mqd.ctx_save_area_size = drmQueueIn->cwsr_size;
    fprintf(stderr, "DEBUG DRM: Creating compute queue with eop_va=0x%llx, cwsr_va=0x%llx, cwsr_size=%u\n",
            (unsigned long long)compute_mqd.eop_va, (unsigned long long)compute_mqd.ctx_save_area_addr, compute_mqd.ctx_save_area_size);
    mqd_in = &compute_mqd;
    flags = AMDGPU_USERQ_CREATE_FLAGS_QUEUE_SECURE;
    ip_type = AMDGPU_HW_IP_COMPUTE;
  } else if (type == SDMA_QUEUE) {
    sdma_mqd.csa_va = drmQueueIn->extra_va;
    mqd_in = &sdma_mqd;
    flags = 0;
    ip_type = AMDGPU_HW_IP_DMA;
  } else {
    debug_print("Invalid queue type! (type: %u)\n", type);
    return HSA_STATUS_ERROR_INVALID_ARGUMENT;
  }
  if (AllocateDoorbellOffset(gpu_agent, type, &doorbell_handle, &doorbell_offset, &queueDoorbellPtr) != HSA_STATUS_SUCCESS) {
    debug_print("Failed to allocate doorbell offset!\n");
    return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
  }

  int result = amdgpu_create_userqueue(gpu_agent.libDrmDev(),
                                       ip_type,
                                       doorbell_handle,
                                       doorbell_offset,
                                       (uint64_t) drmQueueIn->ring_buf,
                                       drmQueueIn->ring_buf_size,
                                       drmQueueIn->wptr_addr,
                                       drmQueueIn->rptr_addr,
                                       mqd_in,
                                       flags,
                                       &queue_id);

  if (result != 0) {
    debug_print("Failed to create user queue! Error code: %d (%s)\n", result, strerror(-result));
    FreeDoorbellOffset(gpu_agent, doorbell_offset);
    return HSA_STATUS_ERROR;
  }

  // Success, fill out result paramaters
  drmQueueOut->queue_id = queue_id;
  drmQueueOut->doorbell_ptr = queueDoorbellPtr;
  drmQueueOut->doorbell_offset = doorbell_offset;

  return HSA_STATUS_SUCCESS;
}

hsa_status_t DrmDriver::DestroyQueue(const DestroyQueueInParams *queueIn) {
  if (!queueIn) {
    debug_print("queueIn is NULL!\n");
    return HSA_STATUS_ERROR_INVALID_ARGUMENT;
  }

  auto * drmQueueIn = static_cast<const DrmDriver::DestroyQueueInParams *>(queueIn);

  FreeDoorbellOffset(drmQueueIn->gpu_agent, drmQueueIn->doorbell_offset);

  if (amdgpu_free_userqueue(drmQueueIn->gpu_agent.libDrmDev(), drmQueueIn->queue_id) != 0) {
    debug_print("Failed to free userqueue!\n");
    return HSA_STATUS_ERROR;
  }

  return HSA_STATUS_SUCCESS;
}

hsa_status_t DrmDriver::ModifyQueue(const ModifyQueueInParams *queueIn) {
  if (!queueIn) {
    debug_print("DrmDriver::ModifyQueue(); queueIn is NULL!\n");
    return HSA_STATUS_ERROR_INVALID_ARGUMENT;
  }

  auto * drmQueueIn = static_cast<const DrmDriver::ModifyQueueInParams *>(queueIn);

  GpuAgent & gpu_agent = const_cast<GpuAgent&>(drmQueueIn->gpu_agent);
  const queue_type type = drmQueueIn->type;

  void *mqd_in = NULL;
  uint32_t ip_type = 0;

  // Struct used to pass modification parameters for the queue
  struct drm_amdgpu_userq_mqd_compute_gfx11 compute_mqd = { 0 };

  if (type != AQL_QUEUE) {
    debug_print("Only AQL queues can be modified (type: %u)\n", type);
    return HSA_STATUS_ERROR_INVALID_ARGUMENT;
  }

  compute_mqd.eop_va = drmQueueIn->extra_va;

  // Set optional modification parameters if provided
  if (drmQueueIn->cu_mask && drmQueueIn->cu_mask_count > 0) {
    compute_mqd.cu_mask_ptr = (uint64_t)drmQueueIn->cu_mask;
    compute_mqd.cu_mask_count = drmQueueIn->cu_mask_count;
  }

  compute_mqd.queue_percentage = drmQueueIn->queue_percentage;
  compute_mqd.hqd_queue_priority = drmQueueIn->hqd_queue_priority;
  compute_mqd.pm4_target_xcc = drmQueueIn->pm4_target_xcc;

  mqd_in = &compute_mqd;
  ip_type = AMDGPU_HW_IP_COMPUTE;

  int result = amdgpu_modify_userqueue(gpu_agent.libDrmDev(),
                                       ip_type,
                                       drmQueueIn->queue_id,
                                       (uint64_t)drmQueueIn->ring_buf,
                                       drmQueueIn->ring_buf_size,
                                       drmQueueIn->wptr_addr,
                                       drmQueueIn->rptr_addr,
                                       mqd_in);

  if (result != 0) {
    debug_print("Failed to modify user queue!\n");
    return HSA_STATUS_ERROR;
  }

  return HSA_STATUS_SUCCESS;
}

void DrmDriver::ReleaseResources(core::Agent &agent) {
  FreeDoorbellMemory(agent);
}

hsa_status_t DrmDriver::AllocateDoorbellMemory(core::Agent &agent) {
  auto &gpu_agent = static_cast<GpuAgent &>(agent);

  /*
   * Doorbell memory is allocated per process.
   *
   * Allocation is done with a lazy approach, it will be done only when first queue is created.
   *
   */
  if (!db_info_by_agent[&agent]) {
    struct amdgpu_bo_alloc_request req = {0};
    amdgpu_bo_handle buf_bo_handle = { 0 };
    __u32 buf_handle = 0;
    uint64_t *cpuPtr = NULL;

    // Numbers are hardcoded for now, this will be replaced with INFO ioctl once it is available.
    const uint32_t first_non_cp = 0x100;
    const uint32_t last_non_cp = 0x190;
    const uint32_t first_sdma = 0x100;
    const uint32_t last_sdma = 0x100 + 10 * 2 - 1;

    req.alloc_size = DOORBELL_SIZE;
    req.preferred_heap =  AMDGPU_GEM_DOMAIN_DOORBELL;

    if (amdgpu_bo_alloc(gpu_agent.libDrmDev(), &req, &buf_bo_handle) != 0)
    {
      debug_print("Failed to allocate doorbell memory!\n");
      return HSA_STATUS_ERROR;
    }

    if (amdgpu_bo_cpu_map(buf_bo_handle, (void **) &cpuPtr) != 0)
    {
      debug_print("Failed to cpu map doorbell memory!\n");
      amdgpu_bo_free(buf_bo_handle);
      return HSA_STATUS_ERROR;
    }

    if (amdgpu_bo_export(buf_bo_handle, amdgpu_bo_handle_type_kms, &buf_handle) != 0)
    {
      debug_print("amdgpu_bo_export failed!\n");

      amdgpu_bo_cpu_unmap(buf_bo_handle);
      amdgpu_bo_free(buf_bo_handle);
      return HSA_STATUS_ERROR;
    }

    // success, create object and setup all fields.
    doorbell_info_t *db_info = new doorbell_info_t;
    memset(db_info, 0, sizeof(*db_info));
    db_info->doorbell_bo_handle = buf_bo_handle;
    db_info->doorbell_handle = buf_handle;
    db_info->doorbellCpuPtr = cpuPtr;
    db_info_by_agent[&agent] = db_info;

    db_info->ranges[AQL_QUEUE][0] = { 0, first_non_cp - 1 };
    db_info->ranges[AQL_QUEUE][1] = { last_non_cp + 1, DOORBELL_PER_PAGE - 1};
    db_info->ranges[SDMA_QUEUE][0] = { first_sdma, last_sdma };
    db_info->ranges[SDMA_QUEUE][1] = { 1, 0 };  // using (start > end) to indicate empty range

  }

  return HSA_STATUS_SUCCESS;
}

void DrmDriver::FreeDoorbellMemory(core::Agent &agent) {
  doorbell_info_t *db_info = db_info_by_agent[&agent];

  if (db_info) {
    if (db_info->doorbellCpuPtr) {
      amdgpu_bo_cpu_unmap(db_info->doorbell_bo_handle);
      db_info->doorbellCpuPtr = NULL;
    }

    if (db_info->doorbell_handle)
    {
      amdgpu_bo_free(db_info->doorbell_bo_handle);
      db_info->doorbell_handle = 0;
    }

    db_info_by_agent.erase(&agent);
    delete db_info;
  }
}

hsa_status_t DrmDriver::AllocateDoorbellOffset(core::Agent &agent, queue_type type, __u32 *doorbell_handle, uint32_t *offset, uint64_t **cpuPtr) {
  doorbell_info_t *db_info = db_info_by_agent[&agent];

  if (!db_info) {
    hsa_status_t ret = AllocateDoorbellMemory(agent);
    if (ret == HSA_STATUS_SUCCESS) {
      db_info = db_info_by_agent[&agent];
    } else {
      return ret;
    }
  }

  assert(db_info);
  assert(db_info->doorbell_handle);
  assert(db_info->doorbellCpuPtr);
  assert((type == AQL_QUEUE) || (type == SDMA_QUEUE));

  /*
   * Each page has a range reserved for SDMA and other non-CP engines.
   * The range is the same on each page.
   * We currently have only 2 pages of doorbell per process, code is however written
   * to be general and to work if number of pages is increased.
   *
   */
  for (uint32_t page = 0; page < (DOORBELL_PER_PROCESS / DOORBELL_PER_PAGE); page++) {
    for (uint32_t r = 0; r < MAX_RANGES_PER_QUEUE_TYPE; r++) {
      const uint32_t start = db_info->ranges[type][r].start + page * DOORBELL_PER_PAGE;
      const uint32_t end = db_info->ranges[type][r].end + page * DOORBELL_PER_PAGE;

      assert(start < DOORBELL_PER_PROCESS);
      assert(end < DOORBELL_PER_PROCESS);

      for (int i = start; i < end; i++) {
        if ((db_info->doorbell_bitmap[i / DOORBELL_BITMAP_BIT_SIZE] & (1 << (i % DOORBELL_BITMAP_BIT_SIZE))) == 0) {
          db_info->doorbell_bitmap[i / DOORBELL_BITMAP_BIT_SIZE] |= (1 << (i % DOORBELL_BITMAP_BIT_SIZE));

          *doorbell_handle = db_info->doorbell_handle;
          *offset = i;
          *cpuPtr = &db_info->doorbellCpuPtr[i];
          return HSA_STATUS_SUCCESS;
        }
      }
    }
  }

  debug_warning("Exceeded available doorbells for process!\n");
  return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
}

void DrmDriver::FreeDoorbellOffset(core::Agent &agent, uint32_t offset) {
  doorbell_info_t *db_info = db_info_by_agent[&agent];

  if (offset < DOORBELL_PER_PROCESS)
    db_info->doorbell_bitmap[offset / DOORBELL_BITMAP_BIT_SIZE] &= ~(1 << (offset % DOORBELL_BITMAP_BIT_SIZE));
  else
    assert(false && "Invalid offset");
}

hsa_status_t DrmDriver::GetUserQueueMetadata(core::Agent &agent, queue_type type, struct drm_amdgpu_info_uq_metadata *info) {
  if (!info)
    return HSA_STATUS_ERROR_INVALID_ARGUMENT;

  GpuAgent &gpu_agent = static_cast<GpuAgent &>(agent);
  uint32_t ip_type = 0;

  if (type == AQL_QUEUE) {
    ip_type = AMDGPU_HW_IP_COMPUTE;
  } else if (type == SDMA_QUEUE) {
    ip_type = AMDGPU_HW_IP_DMA;
  } else {
    debug_print("Invalid queue type! (type: %u)\n", type);
    return HSA_STATUS_ERROR_INVALID_ARGUMENT;
  }

  int result = amdgpu_query_uq_fw_area_info(gpu_agent.libDrmDev(), ip_type, 0, info);

  return (result == 0) ? HSA_STATUS_SUCCESS : HSA_STATUS_ERROR;
}

uint32_t DrmDriver::MapHsaPriorityToHqd(HSA_QUEUE_PRIORITY hsa_priority) {
  // Use the shared priority mapping from libhsakmt
  // Maps HSA_QUEUE_PRIORITY (-3 to +3) to hardware priority (0-15)
  // Mapping: -3→0, -2→3, -1→5, 0→7, +1→9, +2→11, +3→15
  return hsaKmtMapPriorityToHw(hsa_priority);
}

// KFD fallback implementations - used when DRM queue creation falls back to KFD.
hsa_status_t DrmDriver::UpdateQueue(HSA_QUEUEID queue_id, uint32_t queue_pct,
                                    HSA::hsa_amd_queue_priority_internal_t priority, void* queue_addr,
                                    uint64_t queue_size_bytes, HsaEvent* event) const {
  // Delegate to KFD for queues created via KFD fallback when DRM is unsupported on this GPU.
  if (HSAKMT_CALL(hsaKmtUpdateQueue(queue_id, queue_pct, (HSA_QUEUE_PRIORITY)priority,
                                    queue_addr, queue_size_bytes, event)) != HSAKMT_STATUS_SUCCESS) {
    return HSA_STATUS_ERROR;
  }
  return HSA_STATUS_SUCCESS;
}

hsa_status_t DrmDriver::SetQueueCUMask(HSA_QUEUEID queue_id, uint32_t cu_mask_count,
                                       uint32_t* queue_cu_mask) const {
  // Delegate to KFD for queues created via KFD fallback when DRM is unsupported on this GPU.
  if (HSAKMT_CALL(hsaKmtSetQueueCUMask(queue_id, cu_mask_count, queue_cu_mask)) != HSAKMT_STATUS_SUCCESS) {
    return HSA_STATUS_ERROR;
  }
  return HSA_STATUS_SUCCESS;
}

hsa_status_t DrmDriver::AllocQueueGWS(HSA_QUEUEID queue_id, uint32_t num_gws,
                                      uint32_t* first_gws) const {
  // Stub: GWS (Global Wave Sync) allocation not yet supported in DRM mode
  // May require new kernel ioctl support
  return HSA_STATUS_ERROR_INVALID_ARGUMENT;
}

// Legacy DestroyQueue: used for KFD-created queues when DRM queue creation falls back to KFD.
hsa_status_t DrmDriver::DestroyQueue(HSA_QUEUEID queue_id) const {
  if (HSAKMT_CALL(hsaKmtDestroyQueue(queue_id)) != HSAKMT_STATUS_SUCCESS) {
    return HSA_STATUS_ERROR;
  }
  return HSA_STATUS_SUCCESS;
}

namespace {

// Flags that ROCr's SVM path (hsaKmt convention) can set/query and their DRM
// boolean-attribute counterparts. HSA_SVM_FLAG_GPU_ALWAYS_MAPPED has no DRM
// equivalent and is intentionally omitted.
struct SvmFlagMap {
  uint32_t hsa_flag;
  uint32_t drm_attr;
};

constexpr SvmFlagMap kSvmFlagMap[] = {
    {HSA_SVM_FLAG_HOST_ACCESS, AMDGPU_SVM_ATTR_HOST_ACCESS},
    {HSA_SVM_FLAG_COHERENT, AMDGPU_SVM_ATTR_COHERENT},
    {HSA_SVM_FLAG_EXT_COHERENT, AMDGPU_SVM_ATTR_EXT_COHERENT},
    {HSA_SVM_FLAG_HIVE_LOCAL, AMDGPU_SVM_ATTR_HIVE_LOCAL},
    {HSA_SVM_FLAG_GPU_RO, AMDGPU_SVM_ATTR_GPU_RO},
    {HSA_SVM_FLAG_GPU_EXEC, AMDGPU_SVM_ATTR_GPU_EXEC},
    {HSA_SVM_FLAG_GPU_READ_MOSTLY, AMDGPU_SVM_ATTR_GPU_READ_MOSTLY},
};

// Union of flags that GetSvmAttrib reconstructs from SET_FLAGS/CLR_FLAGS.
constexpr uint32_t kKnownSvmFlags =
    HSA_SVM_FLAG_HOST_ACCESS | HSA_SVM_FLAG_COHERENT | HSA_SVM_FLAG_EXT_COHERENT |
    HSA_SVM_FLAG_HIVE_LOCAL | HSA_SVM_FLAG_GPU_RO | HSA_SVM_FLAG_GPU_READ_MOSTLY;

bool IsGpuNode(uint32_t node, const std::vector<uint32_t>& gpu_nodes) {
  return std::find(gpu_nodes.begin(), gpu_nodes.end(), node) != gpu_nodes.end();
}

// A stable, non-zero, non-UNDEFINED value that identifies a GPU to the kernel's
// SVM location field. The kernel treats any value other than SYSMEM(0) and
// UNDEFINED(0xffffffff) as "this GPU/VRAM", so the DRM render minor is used to
// honor the "write the DRM render id" convention.
uint32_t DrmRenderIdForNode(uint32_t node_id) {
  HsaNodeProperties props{};
  if (HSAKMT_CALL(hsaKmtGetNodeProperties(node_id, &props)) == HSAKMT_STATUS_SUCCESS &&
      props.DrmRenderMinor > 0)
    return static_cast<uint32_t>(props.DrmRenderMinor);
  // Fallback: any stable non-zero, non-UNDEFINED value denotes "this GPU".
  return node_id != 0 ? node_id : 1u;
}

}  // namespace

hsa_status_t DrmDriver::SvmSetAttr(void* base, size_t size, const HSA_SVM_ATTRIBUTE* attribs,
                                   size_t count) {
  const auto& gpu_nodes = core::Runtime::runtime_singleton_->gpu_ids();
  if (gpu_nodes.empty()) return HSA_STATUS_ERROR;

  // Aggregate flag bitmasks once; they apply to every device.
  uint32_t set_flags = 0, clr_flags = 0;
  for (size_t i = 0; i < count; ++i) {
    if (attribs[i].type == HSA_SVM_ATTR_SET_FLAGS) set_flags |= attribs[i].value;
    else if (attribs[i].type == HSA_SVM_ATTR_CLR_FLAGS) clr_flags |= attribs[i].value;
  }

  // "One svm one gpu": translate the array relative to each GPU's fd and issue
  // a per-device ioctl so every GPU's independent SVM context is updated.
  for (uint32_t node : gpu_nodes) {
    void* handle = nullptr;
    if (GetDeviceHandle(node, &handle) != HSA_STATUS_SUCCESS || handle == nullptr) continue;
    amdgpu_device_handle dev = reinterpret_cast<amdgpu_device_handle>(handle);
    const uint32_t render_id = DrmRenderIdForNode(node);

    std::vector<drm_amdgpu_svm_attribute> da;
    da.reserve(count + sizeof(kSvmFlagMap) / sizeof(kSvmFlagMap[0]));

    for (size_t i = 0; i < count; ++i) {
      const uint32_t value = attribs[i].value;
      switch (attribs[i].type) {
        case HSA_SVM_ATTR_PREFERRED_LOC:
        case HSA_SVM_ATTR_PREFETCH_LOC: {
          const uint32_t drm_type = (attribs[i].type == HSA_SVM_ATTR_PREFERRED_LOC)
                                        ? AMDGPU_SVM_ATTR_PREFERRED_LOC
                                        : AMDGPU_SVM_ATTR_PREFETCH_LOC;
          uint32_t drm_value;
          if (value == node) {
            drm_value = render_id;
          } else if (value == INVALID_NODEID) {
            drm_value = AMDGPU_SVM_LOCATION_UNDEFINED;
          } else if (IsGpuNode(value, gpu_nodes)) {
            // Targets a different GPU; leave this device's context untouched.
            continue;
          } else {
            drm_value = AMDGPU_SVM_LOCATION_SYSMEM;
          }
          da.push_back({drm_type, drm_value});
          break;
        }
        case HSA_SVM_ATTR_ACCESS:
        case HSA_SVM_ATTR_ACCESS_IN_PLACE:
        case HSA_SVM_ATTR_NO_ACCESS: {
          // Access is per-gpuid in the KFD model; only apply the entry that
          // targets this device.
          if (value != node) continue;
          uint32_t policy = AMDGPU_SVM_ACCESS_ALLOW_MIGRATE;
          if (attribs[i].type == HSA_SVM_ATTR_ACCESS_IN_PLACE)
            policy = AMDGPU_SVM_ACCESS_IN_PLACE;
          else if (attribs[i].type == HSA_SVM_ATTR_NO_ACCESS)
            policy = AMDGPU_SVM_ACCESS_INACCESSIBLE;
          da.push_back({AMDGPU_SVM_ATTR_ACCESS, policy});
          break;
        }
        case HSA_SVM_ATTR_GRANULARITY:
          da.push_back({AMDGPU_SVM_ATTR_GRANULARITY, value});
          break;
        case HSA_SVM_ATTR_SET_FLAGS:
        case HSA_SVM_ATTR_CLR_FLAGS:
          // Handled below in aggregate.
          break;
        default:
          break;
      }
    }

    for (const auto& fm : kSvmFlagMap) {
      if (set_flags & fm.hsa_flag)
        da.push_back({fm.drm_attr, 1});
      else if (clr_flags & fm.hsa_flag)
        da.push_back({fm.drm_attr, 0});
    }

    if (da.empty()) continue;

    if (amdgpu_svm_set_attr(dev, reinterpret_cast<uint64_t>(base), size,
                            static_cast<uint32_t>(da.size()), da.data()) != 0)
      return HSA_STATUS_ERROR;
  }

  return HSA_STATUS_SUCCESS;
}

hsa_status_t DrmDriver::SvmGetAttr(void* base, size_t size, HSA_SVM_ATTRIBUTE* attribs,
                                   size_t count) {
  const auto& gpu_nodes = core::Runtime::runtime_singleton_->gpu_ids();
  if (gpu_nodes.empty())
    return HSA_STATUS_ERROR;

  auto device_for = [&](uint32_t node) -> amdgpu_device_handle {
    void* handle = nullptr;
    if (GetDeviceHandle(node, &handle) != HSA_STATUS_SUCCESS) return nullptr;
    return reinterpret_cast<amdgpu_device_handle>(handle);
  };

  // Reconstruct the current flag bitmask from a representative device so that
  // SET_FLAGS/CLR_FLAGS slots match what GetSvmAttrib expects to read back.
  auto query_flags = [&]() -> uint32_t {
    uint32_t flags = 0;
    amdgpu_device_handle dev = device_for(gpu_nodes[0]);
    if (dev == nullptr) return flags;
    for (const auto& fm : kSvmFlagMap) {
      if (fm.hsa_flag & ~kKnownSvmFlags) continue;
      drm_amdgpu_svm_attribute at{fm.drm_attr, 0};
      if (amdgpu_svm_get_attr(dev, reinterpret_cast<uint64_t>(base), size, 1, &at) == 0 &&
          at.value)
        flags |= fm.hsa_flag;
    }
    return flags;
  };

  bool flags_valid = false;
  uint32_t cur_flags = 0;

  for (size_t i = 0; i < count; ++i) {
    switch (attribs[i].type) {
      case HSA_SVM_ATTR_PREFERRED_LOC:
      case HSA_SVM_ATTR_PREFETCH_LOC: {
        const uint32_t drm_type = (attribs[i].type == HSA_SVM_ATTR_PREFERRED_LOC)
                                      ? AMDGPU_SVM_ATTR_PREFERRED_LOC
                                      : AMDGPU_SVM_ATTR_PREFETCH_LOC;
        uint32_t result = INVALID_NODEID;
        bool sysmem_seen = false;
        for (uint32_t node : gpu_nodes) {
          amdgpu_device_handle dev = device_for(node);
          if (dev == nullptr) continue;
          drm_amdgpu_svm_attribute at{drm_type, 0};
          if (amdgpu_svm_get_attr(dev, reinterpret_cast<uint64_t>(base), size, 1, &at) != 0)
            continue;
          if (at.value == AMDGPU_SVM_LOCATION_UNDEFINED) continue;
          if (at.value == AMDGPU_SVM_LOCATION_SYSMEM) {
            sysmem_seen = true;
            continue;
          }
          // Not sysmem and not undefined => the range lives on this GPU. Map
          // back to the KFD node-id convention regardless of the raw value.
          result = node;
          break;
        }
        if (result == INVALID_NODEID && sysmem_seen) result = 0;
        attribs[i].value = result;
        break;
      }
      case HSA_SVM_ATTR_GRANULARITY: {
        amdgpu_device_handle dev = device_for(gpu_nodes[0]);
        drm_amdgpu_svm_attribute at{AMDGPU_SVM_ATTR_GRANULARITY, 0};
        if (dev != nullptr &&
            amdgpu_svm_get_attr(dev, reinterpret_cast<uint64_t>(base), size, 1, &at) == 0)
          attribs[i].value = at.value;
        break;
      }
      case HSA_SVM_ATTR_ACCESS:
      case HSA_SVM_ATTR_ACCESS_IN_PLACE:
      case HSA_SVM_ATTR_NO_ACCESS: {
        const uint32_t node = attribs[i].value;
        amdgpu_device_handle dev = device_for(node);
        drm_amdgpu_svm_attribute at{AMDGPU_SVM_ATTR_ACCESS, 0};
        if (dev != nullptr &&
            amdgpu_svm_get_attr(dev, reinterpret_cast<uint64_t>(base), size, 1, &at) == 0) {
          switch (at.value) {
            case AMDGPU_SVM_ACCESS_IN_PLACE:
              attribs[i].type = HSA_SVM_ATTR_ACCESS_IN_PLACE;
              break;
            case AMDGPU_SVM_ACCESS_INACCESSIBLE:
              attribs[i].type = HSA_SVM_ATTR_NO_ACCESS;
              break;
            case AMDGPU_SVM_ACCESS_ALLOW_MIGRATE:
            default:
              attribs[i].type = HSA_SVM_ATTR_ACCESS;
              break;
          }
        }
        break;
      }
      case HSA_SVM_ATTR_SET_FLAGS: {
        if (!flags_valid) { cur_flags = query_flags(); flags_valid = true; }
        attribs[i].value = cur_flags;
        break;
      }
      case HSA_SVM_ATTR_CLR_FLAGS: {
        if (!flags_valid) { cur_flags = query_flags(); flags_valid = true; }
        attribs[i].value = kKnownSvmFlags & ~cur_flags;
        break;
      }
      default:
        break;
    }
  }

  return HSA_STATUS_SUCCESS;
}

hsa_status_t DrmDriver::SvmPrefetch(void* base, size_t size, uint32_t dst_node) {
  const auto& gpu_nodes = core::Runtime::runtime_singleton_->gpu_ids();
  if (gpu_nodes.empty()) return HSA_STATUS_ERROR;

  if (IsGpuNode(dst_node, gpu_nodes)) {
    // Migrate to a specific GPU: prefetch on that GPU's fd only.
    void* handle = nullptr;
    if (GetDeviceHandle(dst_node, &handle) != HSA_STATUS_SUCCESS || handle == nullptr)
      return HSA_STATUS_ERROR;

    drm_amdgpu_svm_attribute attr{AMDGPU_SVM_ATTR_PREFETCH_LOC, DrmRenderIdForNode(dst_node)};
    if (amdgpu_svm_set_attr(reinterpret_cast<amdgpu_device_handle>(handle),
                            reinterpret_cast<uint64_t>(base), size, 1, &attr) != 0)
      return HSA_STATUS_ERROR;

    return HSA_STATUS_SUCCESS;
  }

  // Prefetch to system memory: every GPU's independent SVM context must be
  // updated, so issue the ioctl on each device's fd.
  drm_amdgpu_svm_attribute attr{AMDGPU_SVM_ATTR_PREFETCH_LOC, AMDGPU_SVM_LOCATION_SYSMEM};

  for (uint32_t node : gpu_nodes) {
    void* handle = nullptr;
    if (GetDeviceHandle(node, &handle) != HSA_STATUS_SUCCESS || handle == nullptr)
      continue;

    amdgpu_svm_set_attr(reinterpret_cast<amdgpu_device_handle>(handle),
                        reinterpret_cast<uint64_t>(base), size, 1, &attr);
  }
  return HSA_STATUS_SUCCESS;
}


} // namespace AMD
} // namespace rocr
