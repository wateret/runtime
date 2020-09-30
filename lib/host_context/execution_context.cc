/*
 * Copyright 2020 The TensorFlow Runtime Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//===- execution_context.cc -------------------------------------*- C++ -*-===//
//
// This file implements ExecutionContext.
//
//===----------------------------------------------------------------------===//

#include "tfrt/host_context/execution_context.h"

#include "tfrt/host_context/concurrent_work_queue.h"
#include "tfrt/host_context/host_context.h"

namespace tfrt {

RequestContext::~RequestContext() {
  if (auto cancel_value = GetCancelAsyncValue()) {
    cancel_value->DropRef();
  }
}

void RequestContext::Cancel() {
  // Create an AsyncValue in error state for cancel.
  auto* error_value = MakeErrorAsyncValueRef(host_, "Cancelled").release();

  ErrorAsyncValue* expected_value = nullptr;
  // Use memory_order_release for the success case so that error_value is
  // visible to other threads when they load with memory_order_acquire. For the
  // failure case, we do not care about expected_value, so we can use
  // memory_order_relaxed.
  if (!cancel_value_.compare_exchange_strong(expected_value, error_value,
                                             std::memory_order_release,
                                             std::memory_order_relaxed)) {
    error_value->DropRef();
  }
}

Expected<RCReference<RequestContext>> RequestContextBuilder::build() && {
  auto& cwq = host_->work_queue();
  if (auto error = cwq.InitRequest(this)) return std::move(error);

  return TakeRef(
      new RequestContext(host_, resource_context_, std::move(context_data_)));
};

RCReference<RequestContext> RequestContext::Create(
    HostContext* host, ResourceContext* resource_context) {
  auto req_ctx = RequestContextBuilder(host, resource_context).build();
  if (req_ctx) return std::move(*req_ctx);

  // TODO(tfrt-dev): Refactor all the clients to handle the failures properly.
  llvm_unreachable("Failed to build RequestContext.");
}

}  // namespace tfrt
