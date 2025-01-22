/* Copyright 2024 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "xla/backends/cpu/runtime/kernel.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

#include "absl/base/attributes.h"
#include "absl/base/optimization.h"
#include "absl/container/inlined_vector.h"
#include "absl/status/status.h"
#include "absl/types/span.h"
#include "xla/backends/cpu/runtime/kernel_c_api.h"
#include "xla/backends/cpu/runtime/work_queue.h"
#include "xla/stream_executor/device_memory.h"
#include "xla/tsl/concurrency/async_value_ref.h"
#include "xla/tsl/platform/logging.h"
#include "xla/util.h"

#define EIGEN_USE_THREADS
#include "unsupported/Eigen/CXX11/Tensor"

namespace xla::cpu {

using LaunchEvent = Kernel::LaunchEvent;

// Non-reference-counted async value ref for host kernels executed inline.
static tsl::AsyncValueRef<LaunchEvent> OkLaunchEvent() {
  static tsl::AsyncValueOwningRef<LaunchEvent>* event = [] {
    auto* storage = new tsl::internal::AsyncValueStorage<LaunchEvent>();
    return new tsl::AsyncValueOwningRef<LaunchEvent>(
        tsl::MakeAvailableAsyncValueRef<LaunchEvent>(*storage));
  }();
  return event->AsRef();
}

static absl::InlinedVector<XLA_CPU_KernelArg, 8> ConvertBuffersToKernelArgs(
    absl::Span<const Kernel::DeviceMemoryBase> buffers) {
  absl::InlinedVector<XLA_CPU_KernelArg, 8> args(buffers.size());
  for (size_t i = 0; i < buffers.size(); ++i) {
    args[i].data = const_cast<void*>(buffers[i].opaque());
    args[i].size = buffers[i].size();
  }
  return args;
}

namespace {
// A kernel parallel task that is used to parallelize host kernel execution.
class KernelParallelTask {
 public:
  KernelParallelTask(XLA_CPU_Kernel* kernel, Kernel::ThreadDim thread_dims,
                     absl::Span<const XLA_CPU_KernelArg> args);

  // Invokes a host kernel for a given task index.
  absl::Status operator()(size_t task_index) const;

 private:
  // Converts linear task index in [0, num_tasks) to (x, y, z) coordinate. We
  // assume that `x` is the fastest iterating dimension.
  XLA_CPU_KernelThread Delinearize(uint64_t task_index) const;

  XLA_CPU_Kernel* kernel_;
  XLA_CPU_KernelThreadDim thread_dims_;
  absl::InlinedVector<XLA_CPU_KernelArg, 8> args_;
};
}  // namespace

KernelParallelTask::KernelParallelTask(XLA_CPU_Kernel* kernel,
                                       Kernel::ThreadDim thread_dims,
                                       absl::Span<const XLA_CPU_KernelArg> args)
    : kernel_(kernel),
      thread_dims_({thread_dims.x, thread_dims.y, thread_dims.z}),
      args_(args.begin(), args.end()) {}

ABSL_ATTRIBUTE_ALWAYS_INLINE absl::Status KernelParallelTask::operator()(
    uint64_t task_index) const {
  size_t num_tasks = thread_dims_.x * thread_dims_.y * thread_dims_.z;
  DCHECK_LT(task_index, num_tasks) << "Task index out of range";  // Crash OK

  XLA_CPU_KernelThread kernel_thread = Delinearize(task_index);
  XLA_CPU_KernelCallFrame call_frame = {&thread_dims_, &kernel_thread,
                                        args_.size(), args_.data()};

  XLA_CPU_KernelError* error = (*kernel_)(&call_frame);

  if (ABSL_PREDICT_TRUE(error == nullptr)) {
    return absl::OkStatus();
  } else {
    return Internal("Failed to call host kernel: x=%d, y=%d, z=%d",
                    kernel_thread.x, kernel_thread.y, kernel_thread.z);
  }
}

XLA_CPU_KernelThread KernelParallelTask::Delinearize(
    uint64_t task_index) const {
  uint64_t stride_z = thread_dims_.y * thread_dims_.x;
  uint64_t stride_y = thread_dims_.x;

  uint64_t z = task_index / stride_z;
  task_index = task_index % stride_z;

  uint64_t y = task_index / stride_y;
  task_index = task_index % stride_y;

  uint64_t x = task_index;

  return XLA_CPU_KernelThread{x, y, z};
}

Kernel::Kernel(unsigned arity, XLA_CPU_Kernel* kernel)
    : function_(std::make_unique<KernelFunctionPtr>(kernel)),
      kernel_(function_->kernel()),
      arity_(arity) {}

absl::Status Kernel::Launch(const ThreadDim& thread_dims,
                            absl::Span<const DeviceMemoryBase> buffers) const {
  return Launch(thread_dims, ConvertBuffersToKernelArgs(buffers));
}

absl::Status Kernel::Launch(const ThreadDim& thread_dims,
                            absl::Span<const XLA_CPU_KernelArg> args) const {
  XLA_CPU_KernelThreadDim kernel_thread_dims = {
      thread_dims.x,
      thread_dims.y,
      thread_dims.z,
  };

  for (uint64_t z = 0; z < thread_dims.z; ++z) {
    for (uint64_t y = 0; y < thread_dims.y; ++y) {
      for (uint64_t x = 0; x < thread_dims.x; ++x) {
        XLA_CPU_KernelThread kernel_thread = {x, y, z};

        XLA_CPU_KernelCallFrame call_frame = {
            &kernel_thread_dims, &kernel_thread, args.size(), args.data()};

        XLA_CPU_KernelError* error = (*kernel_)(&call_frame);

        if (ABSL_PREDICT_FALSE(error != nullptr)) {
          return absl::InternalError("Failed to call host kernel");
        }
      }
    }
  }

  return absl::OkStatus();
}

tsl::AsyncValueRef<LaunchEvent> Kernel::Launch(
    const ThreadDim& thread_dims, absl::Span<const DeviceMemoryBase> buffers,
    const Eigen::ThreadPoolDevice* device) const {
  return Launch(thread_dims, ConvertBuffersToKernelArgs(buffers), device);
}

tsl::AsyncValueRef<LaunchEvent> Kernel::Launch(
    const ThreadDim& thread_dims, absl::Span<const XLA_CPU_KernelArg> args,
    const Eigen::ThreadPoolDevice* device) const {
  size_t num_tasks = thread_dims.x * thread_dims.y * thread_dims.z;
  CHECK_GT(num_tasks, 0) << "Number of tasks must be positive";  // Crash Ok

  // Short-circuit launch with a single task and run it in the caller thread.
  if (ABSL_PREDICT_TRUE(num_tasks == 1)) {
    absl::Status launched = Launch(thread_dims, args);
    return ABSL_PREDICT_TRUE(launched.ok())
               ? OkLaunchEvent()
               : tsl::MakeErrorAsyncValueRef(std::move(launched));
  }

  // Do not create more workers than the number of threads in the thread pool.
  size_t num_workers =
      std::min<size_t>(std::min<size_t>(num_tasks, device->numThreadsInPool()),
                       std::numeric_limits<uint16_t>::max());

  return Worker::Parallelize(device, num_workers, num_tasks,
                             KernelParallelTask(kernel_, thread_dims, args));
}

}  // namespace xla::cpu
