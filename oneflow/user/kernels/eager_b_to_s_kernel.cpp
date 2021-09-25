/*
Copyright 2020 The OneFlow Authors. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/
#include "oneflow/user/kernels/communicate_util.h"
#include "oneflow/core/device/nccl_util.h"
#include "oneflow/core/common/container_util.h"
#include "oneflow/core/framework/framework.h"
#include "oneflow/core/kernel/new_kernel_util.h"
#include "oneflow/core/job/parallel_desc.h"
#include "oneflow/core/control/global_process_ctx.h"
#include "oneflow/core/job/nd_sbp_util.h"
#include "oneflow/core/register/tensor_slice_copier.h"
#include "oneflow/core/framework/placement_sbp_util.h"

namespace oneflow {

namespace {

Maybe<Symbol<cfg::NdSbp>> GetAllSplitNdSbp(int64_t axis, int64_t ndim) {
  cfg::NdSbp split_nd_sbp;
  for (int64_t i = 0; i < ndim; ++i) {
    split_nd_sbp.mutable_sbp_parallel()->Add()->mutable_split_parallel()->set_axis(axis);
  }
  return SymbolOf(split_nd_sbp);
}

auto* CachedGetAllSplitNdSbp = DECORATE(&GetAllSplitNdSbp, ThreadLocal);

Maybe<Symbol<cfg::NdSbp>> GetAllBroadcastNdSbp(int64_t ndim) {
  cfg::NdSbp split_nd_sbp;
  for (int64_t i = 0; i < ndim; ++i) {
    split_nd_sbp.mutable_sbp_parallel()->Add()->mutable_broadcast_parallel();
  }
  return SymbolOf(split_nd_sbp);
}

auto* CachedGetAllBroadcastNdSbp = DECORATE(&GetAllBroadcastNdSbp, ThreadLocal);

class EagerBToSOpKernelState final : public user_op::OpKernelState {
 public:
  explicit EagerBToSOpKernelState(user_op::KernelInitContext* ctx) { Init(ctx); }
  ~EagerBToSOpKernelState() override = default;

  MemoryCopier* memory_copier() const { return memory_copier_.get(); }

  const std::vector<std::pair<int64_t, std::shared_ptr<TensorSliceCopier>>>&
  sorted_elem_cnt2in_tensor_slice_copier_pair() const {
    return sorted_elem_cnt2in_tensor_slice_copier_pair_;
  }

  const std::vector<std::pair<int64_t, std::shared_ptr<TensorSliceCopier>>>&
  sorted_elem_cnt2out_tensor_slice_copier_pair() const {
    return sorted_elem_cnt2out_tensor_slice_copier_pair_;
  }

  const std::vector<std::pair<int64_t, int64_t>>& sorted_p2p_pair() const {
    return sorted_p2p_pair_;
  }

 private:
  void Init(user_op::KernelInitContext* ctx) {
    const std::string& in_parallel_conf_txt = ctx->Attr<std::string>("in_parallel_conf");
    const std::string& out_parallel_conf_txt = ctx->Attr<std::string>("out_parallel_conf");
    const int64_t out_split_axis = ctx->Attr<int64_t>("out_split_axis");
    const Shape& shape = ctx->Attr<Shape>("shape");
    DeviceType device_type = ctx->device_type();
    DataType data_type = ctx->TensorDesc4ArgNameAndIndex("in", 0)->data_type();
    Symbol<ParallelDesc> in_parallel_desc = CHECK_JUST(TxtStringToPlacement(in_parallel_conf_txt));
    Symbol<ParallelDesc> out_parallel_desc =
        CHECK_JUST(TxtStringToPlacement(out_parallel_conf_txt));
    int64_t out_parallel_num = out_parallel_desc->parallel_num();

    for (int64_t out_parallel_id = 0; out_parallel_id < out_parallel_num; ++out_parallel_id) {
      int64_t dst = CHECK_JUST(out_parallel_desc->MachineId4ParallelId(out_parallel_id));
      int64_t src = -1;
      const TensorSliceView& out_slice = GetTensorSliceView4ParallelId(
          *out_parallel_desc->hierarchy(),
          *CHECK_JUST(
              CachedGetAllSplitNdSbp(out_split_axis, out_parallel_desc->hierarchy()->NumAxes())),
          shape, out_parallel_id);
      CHECK(!out_slice.IsEmpty());
      TensorSliceView in_slice;
      TensorSliceView intersection;
      {
        if (in_parallel_desc->ContainingMachineId(dst)) {
          src = dst;
          int64_t src_device_id = GlobalProcessCtx::LocalRank(src);
          int64_t in_parallel_id =
              CHECK_JUST(in_parallel_desc->ParallelId4MachineDeviceId(src, src_device_id));
          in_slice = GetTensorSliceView4ParallelId(
              *in_parallel_desc->hierarchy(),
              *CHECK_JUST(CachedGetAllBroadcastNdSbp(in_parallel_desc->hierarchy()->NumAxes())),
              shape, in_parallel_id);
          // copy to out_slice from in_slice if src == dst
          intersection = out_slice;
        } else {
          int64_t in_parallel_num = in_parallel_desc->parallel_num();
          int64_t in_parallel_id = out_parallel_id % in_parallel_num;
          src = CHECK_JUST(in_parallel_desc->MachineId4ParallelId(in_parallel_id));
          in_slice = GetTensorSliceView4ParallelId(
              *in_parallel_desc->hierarchy(),
              *CHECK_JUST(GetAllBroadcastNdSbp(in_parallel_desc->hierarchy()->NumAxes())), shape,
              in_parallel_id);
          intersection = out_slice.Intersect(in_slice);
        }
      }
      CHECK_NE(src, -1);
      CHECK(!in_slice.IsEmpty());
      CHECK(!intersection.IsEmpty());
      sorted_p2p_pair_.emplace_back(std::make_pair(src, dst));
      sorted_elem_cnt2in_tensor_slice_copier_pair_.emplace_back(
          std::make_pair(intersection.shape().elem_cnt(),
                         std::make_shared<TensorSliceCopier>(intersection, in_slice, data_type)));
      sorted_elem_cnt2out_tensor_slice_copier_pair_.emplace_back(
          std::make_pair(intersection.shape().elem_cnt(),
                         std::make_shared<TensorSliceCopier>(out_slice, intersection, data_type)));
    }
    memory_copier_.reset(NewDefaultMemoryCopier(device_type));
  }

  std::vector<std::pair<int64_t, std::shared_ptr<TensorSliceCopier>>>
      sorted_elem_cnt2in_tensor_slice_copier_pair_;
  std::vector<std::pair<int64_t, std::shared_ptr<TensorSliceCopier>>>
      sorted_elem_cnt2out_tensor_slice_copier_pair_;
  std::vector<std::pair<int64_t, int64_t>> sorted_p2p_pair_;
  std::unique_ptr<MemoryCopier> memory_copier_;
};

size_t InferEagerBToSKernelTmpBufferSize(user_op::InferContext* ctx) {
  const user_op::TensorDesc& in_tensor = ctx->InputTensorDesc("in", 0);
  const Shape& shape = ctx->Attr<Shape>("shape");
  const std::string& out_parallel_conf_txt = ctx->Attr<std::string>("out_parallel_conf");
  Symbol<ParallelDesc> out_parallel_desc = CHECK_JUST(TxtStringToPlacement(out_parallel_conf_txt));
  int64_t out_parallel_num = out_parallel_desc->parallel_num();
  size_t tensor_byte_size =
      shape.elem_cnt() / out_parallel_num * GetSizeOfDataType(in_tensor.data_type());
  return tensor_byte_size;
}

}  // namespace

template<DeviceType device_type>
class EagerBToSKernel final : public user_op::OpKernel {
 public:
  EagerBToSKernel() = default;
  ~EagerBToSKernel() override = default;

  std::shared_ptr<user_op::OpKernelState> CreateOpKernelState(
      user_op::KernelInitContext* ctx) const override {
    return std::make_shared<EagerBToSOpKernelState>(ctx);
  }

 private:
  void Compute(user_op::KernelComputeContext* ctx, user_op::OpKernelState* state) const override {
    auto* kernel_state = dynamic_cast<EagerBToSOpKernelState*>(state);
    CHECK(kernel_state != nullptr);
    const user_op::Tensor* in = ctx->Tensor4ArgNameAndIndex("in", 0);
    user_op::Tensor* out = ctx->Tensor4ArgNameAndIndex("out", 0);
    user_op::Tensor* tmp_buffer = ctx->Tensor4ArgNameAndIndex("tmp_buffer", 0);
    const void* in_ptr = in->dptr();
    void* out_ptr = out->mut_dptr();
    void* tmp_buffer_ptr = tmp_buffer->mut_dptr();

    const auto& sorted_elem_cnt2in_tensor_slice_copier_pair =
        kernel_state->sorted_elem_cnt2in_tensor_slice_copier_pair();
    const auto& sorted_elem_cnt2out_tensor_slice_copier_pair =
        kernel_state->sorted_elem_cnt2out_tensor_slice_copier_pair();
    const auto& sorted_p2p_pair = kernel_state->sorted_p2p_pair();
    MemoryCopier* memory_copier = kernel_state->memory_copier();
    CHECK_EQ(sorted_elem_cnt2in_tensor_slice_copier_pair.size(), sorted_p2p_pair.size());
    CHECK_EQ(sorted_elem_cnt2out_tensor_slice_copier_pair.size(), sorted_p2p_pair.size());

    for (int64_t i = 0; i < sorted_p2p_pair.size(); ++i) {
      const auto& p2p_pair = sorted_p2p_pair.at(i);
      int64_t src = p2p_pair.first;
      int64_t dst = p2p_pair.second;
      if (src == dst && src == GlobalProcessCtx::Rank()) {
        const auto& elem_cnt2tensor_slice_copier_pair =
            sorted_elem_cnt2in_tensor_slice_copier_pair.at(i);
        const auto& tensor_slice_copier = elem_cnt2tensor_slice_copier_pair.second;
        tensor_slice_copier->Copy(ctx->device_ctx(), *memory_copier, out_ptr, in_ptr);
        continue;
      }
      if (GlobalProcessCtx::Rank() == src) {
        const auto& elem_cnt2tensor_slice_copier_pair =
            sorted_elem_cnt2in_tensor_slice_copier_pair.at(i);
        const auto& elem_cnt = elem_cnt2tensor_slice_copier_pair.first;
        const auto& tensor_slice_copier = elem_cnt2tensor_slice_copier_pair.second;
        tensor_slice_copier->Copy(ctx->device_ctx(), *memory_copier, tmp_buffer_ptr, in_ptr);
        CHECK_JUST(Send<device_type>(reinterpret_cast<const void*>(tmp_buffer_ptr), elem_cnt,
                                     in->data_type(), dst, ctx->device_ctx()));
      }
      if (GlobalProcessCtx::Rank() == dst) {
        const auto& elem_cnt2tensor_slice_copier_pair =
            sorted_elem_cnt2out_tensor_slice_copier_pair.at(i);
        const auto& elem_cnt = elem_cnt2tensor_slice_copier_pair.first;
        const auto& tensor_slice_copier = elem_cnt2tensor_slice_copier_pair.second;
        CHECK_JUST(
            Recv<device_type>(tmp_buffer_ptr, elem_cnt, out->data_type(), src, ctx->device_ctx()));
        tensor_slice_copier->Copy(ctx->device_ctx(), *memory_copier, out_ptr,
                                  reinterpret_cast<const void*>(tmp_buffer_ptr));
      }
    }
  }
  bool AlwaysComputeWhenAllOutputsEmpty() const override { return false; }
};

#define REGISTER_EAGER_B_TO_S_KERNEL(device)              \
  REGISTER_USER_KERNEL("eager_b_to_s")                    \
      .SetCreateFn<EagerBToSKernel<device>>()             \
      .SetIsMatchedHob(user_op::HobDeviceTag() == device) \
      .SetInferTmpSizeFn(InferEagerBToSKernelTmpBufferSize);

REGISTER_EAGER_B_TO_S_KERNEL(DeviceType::kCPU)
#if defined(WITH_CUDA) && HAS_GPU_SEND_RECV
REGISTER_EAGER_B_TO_S_KERNEL(DeviceType::kGPU)
#endif

}  // namespace oneflow