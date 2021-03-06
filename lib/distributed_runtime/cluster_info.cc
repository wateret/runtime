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

//===- cluster_info.cc ----------------------------------------------------===//
//
// Defines the cluster info type.
//
//===----------------------------------------------------------------------===//

#include "tfrt/distributed_runtime/cluster_info.h"

#include "llvm/ADT/StringExtras.h"
#include "tfrt/distributed_runtime/distributed_context.h"
#include "tfrt/distributed_runtime/task_name_util.h"
#include "tfrt/support/error_util.h"
#include "tfrt/support/random_util.h"

namespace tfrt {
namespace {
TaskHandle GetNewTaskHandle() {
  TaskHandle handle;
  do {
    handle = TaskHandle(random::New64());
  } while (handle == TaskHandle::kInvalidTaskHandle);
  return handle;
}
}  // namespace

ClusterInfo::TaskInfo::TaskInfo(string_view job, int task_id, string_view addr)
    : handle(GetNewTaskHandle()),
      name(StrCat("/job:", job, "/task:", task_id)),
      address(addr) {}

// TODO(haoyuzhang): avoid string parsing in ClusterInfo initialization when
// ClusterConfiguration is defined with separate job names and task ids.
ClusterInfo::ClusterInfo(const ClusterConfiguration& cluster_config) {
  for (const auto& task_addr : cluster_config.task_addresses) {
    // TODO(haoyuzhang): Get rid of name parsing when ClusterConfiguration is
    // moved to proto.
    std::string job_name;
    int task_id;
    DieIfError(
        TaskNameUtil::ParseTaskName(task_addr.first(), &job_name, &task_id));

    auto job_it = jobs_.try_emplace(job_name, JobInfo{job_name}).first;
    TaskInfo task_info(job_name, task_id, task_addr.second);
    auto task_inserted = job_it->second.tasks.try_emplace(task_id, task_info);
    assert(task_inserted.second && "Found duplicate tasks in ClusterInfo.");
    tasks_.try_emplace(task_info.handle, &task_inserted.first->second);
    if (task_addr.first() == cluster_config.task_name) {
      task_handle_ = task_info.handle;
    }
  }
}

Expected<TaskHandle> ClusterInfo::GetTaskHandle(string_view task_name) const {
  std::string job_name;
  int task_id;
  Error error = TaskNameUtil::ParseTaskName(task_name, &job_name, &task_id);
  if (error) return std::move(error);
  return GetTaskHandle(job_name, task_id);
}

Expected<TaskHandle> ClusterInfo::GetTaskHandle(string_view job_name,
                                                int task_id) const {
  const auto job_it = jobs_.find(job_name);
  if (job_it == jobs_.end()) {
    return llvm::make_error<TaskNotFoundErrorInfo>(StrCat(
        "Failed to get TaskHandle for job_name=", job_name,
        ", task_id=", task_id, ": the job ", job_name, " does not exist."));
  }
  const auto task_it = job_it->second.tasks.find(task_id);
  if (task_it == job_it->second.tasks.end()) {
    return llvm::make_error<TaskNotFoundErrorInfo>(StrCat(
        "Failed to get TaskHandle for job_name=", job_name,
        ", task_id=", task_id, ": the task ", task_id, " does not exist."));
  }
  return task_it->second.handle;
}

Expected<string_view> ClusterInfo::GetTaskName(TaskHandle task_handle) const {
  const auto it = tasks_.find(task_handle);
  if (it == tasks_.end()) {
    return llvm::make_error<TaskNotFoundErrorInfo>(
        StrCat("No task found with TaskHandle ", task_handle));
  }
  return it->second->name;
}

Expected<string_view> ClusterInfo::GetTaskAddress(
    TaskHandle task_handle) const {
  const auto it = tasks_.find(task_handle);
  if (it == tasks_.end()) {
    return llvm::make_error<TaskNotFoundErrorInfo>(
        StrCat("No task found with TaskHandle ", task_handle));
  }
  return it->second->address;
}
}  // namespace tfrt
