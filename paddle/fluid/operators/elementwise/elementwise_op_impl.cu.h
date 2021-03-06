/* Copyright (c) 2021 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */
#pragma once

#include "paddle/fluid/framework/tensor.h"
#include "paddle/fluid/platform/device_context.h"
#include "paddle/fluid/platform/enforce.h"
#include "paddle/fluid/platform/float16.h"

#ifdef __HIPCC__
#define ELEMENTWISE_BLOCK_SIZE 256
#else
#define ELEMENTWISE_BLOCK_SIZE 512
#endif

namespace paddle {
namespace operators {

enum ElementwiseType { kUnary = 1, kBinary = 2 };

template <typename T, int Size>
struct alignas(sizeof(T) * Size) CudaAlignedVector {
  T val[Size];
};

template <typename T>
int GetVectorizedSizeImpl(const T *pointer) {
  uint64_t address = reinterpret_cast<uint64_t>(pointer);
  constexpr int vec4 =
      std::alignment_of<CudaAlignedVector<T, 4>>::value;  // NOLINT
  constexpr int vec2 =
      std::alignment_of<CudaAlignedVector<T, 2>>::value;  // NOLINT
  if (address % vec4 == 0) {
    return 4;
  } else if (address % vec2 == 0) {
    return 2;
  }
  return 1;
}

template <typename InT, typename OutT>
int GetVectorizedSize(const std::vector<const framework::Tensor *> &ins,
                      const std::vector<framework::Tensor *> &outs) {
  int vec_size = 4;
  for (auto iter = ins.begin(); iter != ins.end(); ++iter) {
    vec_size =
        std::min<int>(vec_size, GetVectorizedSizeImpl((*iter)->data<InT>()));
  }
  for (auto iter = outs.begin(); iter != outs.end(); ++iter) {
    vec_size =
        std::min<int>(vec_size, GetVectorizedSizeImpl((*iter)->data<OutT>()));
  }
  return vec_size;
}

template <ElementwiseType ET, int VecSize, typename InT, typename OutT>
struct ElementwiseDataWrapper {
  OutT *out;
  const InT *in0;
  const InT *in1;
  __device__ ElementwiseDataWrapper(OutT *out, const InT *in0,
                                    const InT *in1 = nullptr)
      : out(out), in0(in0), in1(in1) {}

  using InVecType = CudaAlignedVector<InT, VecSize>;
  using OutVecType = CudaAlignedVector<OutT, VecSize>;

  inline __device__ void load_vector(InVecType args[], int idx) {
    const InVecType *x_vec = reinterpret_cast<const InVecType *>(in0);
    args[0] = x_vec[idx];
    if (ET == ElementwiseType::kBinary) {
      const InVecType *y_vec = reinterpret_cast<const InVecType *>(in1);
      args[1] = y_vec[idx];
    }
  }

  inline __device__ void load_scalar(InT args[], int idx) {
    args[0] = in0[idx];
    if (ET == ElementwiseType::kBinary) {
      args[1] = in1[idx];
    }
  }

  inline __device__ void store_vector(OutVecType res, int idx) {
    OutVecType *out_vec = reinterpret_cast<OutVecType *>(out);
    out_vec[idx] = res;
  }

  inline __device__ void store_scalar(OutT res, int idx) { out[idx] = res; }
};

template <ElementwiseType ET, int VecSize, typename InT, typename OutT,
          typename Functor>
__device__ void VectorizedKernelImpl(
    ElementwiseDataWrapper<ET, VecSize, InT, OutT> data, Functor func,
    int tid) {
  using InVecType = CudaAlignedVector<InT, VecSize>;
  using OutVecType = CudaAlignedVector<OutT, VecSize>;
  InVecType ins_vec[ET];
  OutVecType out_vec;
  InT *ins_ptr[ET];
  OutT *out_ptr;
#pragma unroll
  for (int i = 0; i < ET; ++i) {
    ins_ptr[i] = reinterpret_cast<InT *>(&(ins_vec[i]));
  }
  out_ptr = reinterpret_cast<OutT *>(&out_vec);

  // load
  data.load_vector(ins_vec, tid);

// compute
#pragma unroll
  for (int i = 0; i < VecSize; ++i) {
    InT ins[ET];
#pragma unroll
    for (int j = 0; j < ET; ++j) {
      ins[j] = ins_ptr[j][i];
    }
    out_ptr[i] = func(ins);
  }

  // store
  data.store_vector(out_vec, tid);
}

template <ElementwiseType ET, int VecSize, typename InT, typename OutT,
          typename Functor>
__device__ void ScalarKernelImpl(
    ElementwiseDataWrapper<ET, VecSize, InT, OutT> data, Functor func,
    int start, int remain) {
  InT ins[ET];
  OutT out;

  for (int i = 0; i < remain; ++i) {
    int idx = start + i;
    // load
    data.load_scalar(ins, idx);
    // compute
    out = func(ins);
    // store
    data.store_scalar(out, idx);
  }
}

template <ElementwiseType ET, int VecSize, typename InT, typename OutT,
          typename Functor>
__global__ void VectorizedKernel(const InT *__restrict__ in0,
                                 const InT *__restrict__ in1, OutT *out,
                                 int size, Functor func) {
  int tid = blockIdx.x * blockDim.x + threadIdx.x;
  int remain = size - VecSize * tid;
  remain = remain > 0 ? remain : 0;
  auto data = ElementwiseDataWrapper<ET, VecSize, InT, OutT>(out, in0, in1);
  if (remain >= VecSize) {
    VectorizedKernelImpl(data, func, tid);
  } else {
    ScalarKernelImpl(data, func, tid * VecSize, remain);
  }
}

template <ElementwiseType ET, typename InT, typename OutT, typename Functor>
__global__ void ScalarKernel(const InT *__restrict__ in0,
                             const InT *__restrict__ in1, OutT *out, int size,
                             Functor func) {
  auto data = ElementwiseDataWrapper<ET, 1, InT, OutT>(out, in0, in1);
  int tid = blockIdx.x * blockDim.x + threadIdx.x;
  int remain = tid < size ? 1 : 0;
  ScalarKernelImpl(data, func, tid, remain);
}

template <ElementwiseType ET, typename InT, typename OutT, typename Functor>
void LaunchElementwiseCudaKernel(
    const platform::CUDADeviceContext &ctx,
    const std::vector<const framework::Tensor *> &ins,
    std::vector<framework::Tensor *> *outs, Functor func) {
  // calculate the max vec_size for all ins and outs
  auto size = ins[0]->numel();
  int vec_size = GetVectorizedSize<InT, OutT>(ins, *outs);
  int block_size = ELEMENTWISE_BLOCK_SIZE;
  int grid_size =
      ((size + vec_size - 1) / vec_size + block_size - 1) / block_size;
  const InT *in0 = ins[0]->data<InT>();
  const InT *in1 =
      (ET == ElementwiseType::kBinary) ? ins[1]->data<InT>() : nullptr;
  OutT *out = (*outs)[0]->data<OutT>();
  // cuda kernel
  auto stream = ctx.stream();

  switch (vec_size) {
    case 4:
      VectorizedKernel<ET, 4><<<grid_size, block_size, 0, stream>>>(
          in0, in1, out, size, func);
      break;
    case 2:
      VectorizedKernel<ET, 2><<<grid_size, block_size, 0, stream>>>(
          in0, in1, out, size, func);
      break;
    case 1:
      ScalarKernel<ET><<<grid_size, block_size, 0, stream>>>(in0, in1, out,
                                                             size, func);
      break;
    default:
      PADDLE_THROW(platform::errors::Unimplemented(
          "Unsupported vectorized size: %d !", vec_size));
      break;
  }
}

}  // namespace operators
}  // namespace paddle
