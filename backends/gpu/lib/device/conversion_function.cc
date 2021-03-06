// Copyright 2020 The TensorFlow Runtime Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//===- device/conversion_function.cc --------------------------------------===//
//
// This file implements GPU tensor conversion functions for copying between gpu
// and host.
//
//===----------------------------------------------------------------------===//

#include "tfrt/gpu/device/conversion_function.h"

#include "tfrt/gpu/device/device.h"
#include "tfrt/gpu/memory/gpu_allocator.h"
#include "tfrt/gpu/stream/stream_wrapper.h"
#include "tfrt/gpu/tensor/dense_gpu_tensor.h"
#include "tfrt/host_context/async_dispatch.h"
#include "tfrt/host_context/async_value_ref.h"
#include "tfrt/host_context/execution_context.h"
#include "tfrt/host_context/host_context.h"
#include "tfrt/support/error_util.h"
#include "tfrt/tensor/conversion_registry.h"
#include "tfrt/tensor/conversion_utils.h"
#include "tfrt/tensor/dense_host_tensor.h"
#include "tfrt/tensor/tensor_metadata.h"

namespace tfrt {
namespace gpu {

using gpu::stream::EventFlags;
using gpu::stream::EventRecord;
using gpu::stream::EventSynchronize;
using gpu::stream::OwningEvent;
using gpu::stream::Pointer;

AsyncValueRef<DenseHostTensor> ConvertDenseGpuTensorToDenseHostTensor(
    stream::CurrentContext current_context, stream::Stream stream,
    const DenseGpuTensor& gpu_tensor, HostContext* host) {
  llvm::Optional<DenseHostTensor> result_or_error =
      DenseHostTensor::CreateUninitialized(gpu_tensor.metadata(), host);
  if (!result_or_error) {
    return MakeErrorAsyncValueRef(host, "cannot allocate result tensor");
  }
  DenseHostTensor result = std::move(*result_or_error);

  Pointer<void> memcpy_dst(result.data(), current_context.platform());
  Pointer<void> memcpy_src = gpu_tensor.buffer().pointer();

  size_t size_in_bytes = gpu_tensor.metadata().GetHostSizeInBytes();
  llvm::Error memcpy_error =
      MemcpyAsync(current_context, /*dst=*/memcpy_dst, /*src=*/memcpy_src,
                  size_in_bytes, stream);
  if (memcpy_error) {
    return MakeErrorAsyncValueRef(host,
                                  "failed to enqueue host to device memcpy: " +
                                      toString(std::move(memcpy_error)));
  }

  llvm::Expected<OwningEvent> event_or_error =
      EventCreate(current_context, EventFlags::DISABLE_TIMING);
  if (!event_or_error) {
    return MakeErrorAsyncValueRef(
        host, "could not create event to wait for host to device memcpy: " +
                  toString(event_or_error.takeError()));
  }

  OwningEvent event = std::move(*event_or_error);

  llvm::Error event_record_error = EventRecord(event.get(), stream);
  if (event_record_error) {
    return MakeErrorAsyncValueRef(
        host, "could not enqueue event to wait for host to device memcpy: " +
                  toString(std::move(event_record_error)));
  }

  return EnqueueBlockingWork(
      host,
      [event = std::move(event),
       result = std::move(result)]() mutable -> Expected<DenseHostTensor> {
        if (EventSynchronize(event.get())) {
          return MakeStringError("could not wait for event");
        } else {
          return std::move(result);
        }
      });
}

static AsyncValueRef<DenseHostTensor>
DenseGpuTensorToDenseHostTensorConversionFn(const DenseGpuTensor& tensor,
                                            const GpuDevice& src,
                                            const CpuDevice& dst,
                                            const ExecutionContext& exec_ctx) {
  return ConvertDenseGpuTensorToDenseHostTensor(
      src.CreateContext(), src.stream(), tensor, exec_ctx.host());
}

Expected<DenseGpuTensor> ConvertDenseHostTensorToDenseGpuTensor(
    stream::CurrentContext current_context, stream::Stream stream,
    GpuAllocator* allocator, const DenseHostTensor& tensor, HostContext* host) {
  size_t size_in_bytes = tensor.metadata().GetHostSizeInBytes();

  llvm::Expected<RCReference<gpu::GpuBuffer>> buffer_or_error =
      allocator->Allocate(
          /*size=*/size_in_bytes, stream);
  if (!buffer_or_error) return buffer_or_error.takeError();
  RCReference<gpu::GpuBuffer> buffer = std::move(*buffer_or_error);

  Pointer<const void> memcpy_src(tensor.data(), current_context.platform());
  if (auto error = MemcpyAsync(current_context, /*dst=*/buffer->pointer(),
                               /*src=*/memcpy_src, size_in_bytes, stream))
    return std::move(error);

  llvm::Expected<OwningEvent> event_or_error =
      EventCreate(current_context, EventFlags::DISABLE_TIMING);
  if (!event_or_error) return event_or_error.takeError();

  OwningEvent event = std::move(*event_or_error);

  if (auto error = EventRecord(event.get(), stream)) return std::move(error);

  // The underlying buffer of `tensor` needs to live until the memcpy is done.
  bool work_enqueued = EnqueueBlockingWork(
      host, [tensor = tensor.CopyRef(), event = std::move(event)] {
        // FIXME(sanjoy): How do we handle an error from EventSynchronize here?
        llvm::ExitOnError die_if_error;
        die_if_error(EventSynchronize(event.get()));
      });
  if (!work_enqueued) {
    return MakeStringError(
        "could not enqueue work to synchronize after issuing host to device "
        "memcpy in CreateDenseTensorOp");
  }
  return gpu::DenseGpuTensor(tensor.metadata(), std::move(buffer));
}

static Expected<DenseGpuTensor> DenseHostTensorToDenseGpuTensorConversionFn(
    const DenseHostTensor& tensor, const CpuDevice& src, const GpuDevice& dst,
    const ExecutionContext& exec_ctx) {
  return ConvertDenseHostTensorToDenseGpuTensor(dst.CreateContext(),
                                                dst.stream(), dst.allocator(),
                                                tensor, exec_ctx.host());
}

void RegisterGpuTensorConversionFn(TensorConversionFnRegistry* registry) {
  registry->AddTensorConversionFn(
      TFRT_CONVERSION(DenseHostTensorToDenseGpuTensorConversionFn));
  registry->AddTensorConversionFn(
      TFRT_CONVERSION(DenseGpuTensorToDenseHostTensorConversionFn));
}

}  // namespace gpu
}  // namespace tfrt
