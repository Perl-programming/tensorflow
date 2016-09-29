/* Copyright 2016 Google Inc. All Rights Reserved.

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

// See docs in ../ops/ctc_ops.cc.

#include "tensorflow/contrib/warpctc/kernels/warpctc_ops.h"
#include "tensorflow/contrib/warpctc/kernels/warp-ctc/include/ctc.h"

#include "third_party/eigen3/unsupported/Eigen/CXX11/Tensor"
namespace tensorflow {

typedef Eigen::GpuDevice GPUDevice;

template<>
class WarpCTCLossOp<GPUDevice> : public OpKernel {
  typedef Eigen::Map<const Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic,
                                         Eigen::RowMajor> >
      InputMap;
  typedef Eigen::Map<
      Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor> >
      OutputMap;

 public:
  explicit WarpCTCLossOp(OpKernelConstruction* ctx) : OpKernel(ctx) {
    OP_REQUIRES_OK(ctx, ctx->GetAttr("preprocess_collapse_repeated",
                                     &preprocess_collapse_repeated_));
    OP_REQUIRES_OK(ctx,
                   ctx->GetAttr("ctc_merge_repeated", &ctc_merge_repeated_));
  }

  void Compute(OpKernelContext* ctx) override {

    // Calculate the score analytically

    cudaStream_t stream = ctx->template eigen_device<Eigen::GpuDevice>().stream();
/*
    throw_on_error(cudaStreamCreate(&stream),
                   "cudaStreamCreate");
*/
    const Tensor& input_tensor = ctx->input(0);
    const Tensor& labels_indices_tensor = ctx->input(1);
    const Tensor& labels_values_tensor = ctx->input(2);
    const Tensor& seq_len_tensor = ctx->input(3);
    auto input = input_tensor.tensor<float, 3>();
    float *activations_gpu = (float*)input.data();
    const TensorShape& inputs_shape = input_tensor.shape();
    const int64 max_time = inputs_shape.dim_size(0);
    const int64 batch_size = inputs_shape.dim_size(1);
    const int64 num_classes = inputs_shape.dim_size(2);
    const int alphabet_size = num_classes;
    TensorShape labels_shape({batch_size, max_time});
    std::vector<int64> order{0, 1};
    auto labels_indices = labels_indices_tensor.tensor<int64, 2>();
    auto labels_values = labels_values_tensor.tensor<int, 1>();
    auto seq_len = seq_len_tensor.tensor<int, 1>();
    int64 last_first = 0, sum = 0;
    std::vector<int> labels;
    std::vector<int> label_lengths;
    for(int i = 0; i < labels_indices_tensor.dim_size(0); i++) {
      int64 first;
      int label_of_first;
      cudaMemcpyAsync(&first, &labels_indices(i,0), sizeof(int64), cudaMemcpyDeviceToHost, stream);
      cudaMemcpyAsync(&label_of_first, &labels_values(i), sizeof(int), cudaMemcpyDeviceToHost, stream);
      labels.push_back(label_of_first);
      if (first == last_first) {
        sum++;
      } else {
        label_lengths.push_back(sum);
        last_first = first;
        sum = 1;
      }
    }
    if (sum != 0) {
      label_lengths.push_back(sum);
    }
    int size_of_lengths = seq_len_tensor.dim_size(0);
    int lengths[size_of_lengths];
    cudaMemcpyAsync(lengths, seq_len.data(), size_of_lengths * sizeof(int), cudaMemcpyDeviceToHost, stream);

    float score;

    ctcComputeInfo info;
    info.loc = CTC_GPU;
    info.stream = stream;

    size_t gpu_alloc_bytes;
    throw_on_error(get_workspace_size(label_lengths.data(), lengths,
                                      alphabet_size, size_of_lengths, info,
                                      &gpu_alloc_bytes),
                   "Error: get_workspace_size in small_test");

    char *ctc_gpu_workspace;
    throw_on_error(cudaMalloc(&ctc_gpu_workspace, gpu_alloc_bytes),
                   "cudaMalloc");

    Tensor* loss = nullptr;
    OP_REQUIRES_OK(ctx, ctx->allocate_output("loss", seq_len_tensor.shape(), &loss));
    auto loss_t = loss->vec<float>();

    Tensor* gradient;
    OP_REQUIRES_OK(ctx,
                   ctx->allocate_output("gradient", inputs_shape, &gradient));
    auto gradient_t = gradient->tensor<float, 3>();
    float loss_cpu[size_of_lengths];
    cudaMemset(gradient_t.data(), 0, gradient->NumElements() * sizeof(float));
    throw_on_error(compute_ctc_loss(activations_gpu, gradient_t.data(),
                                    labels.data(), label_lengths.data(),
                                    lengths,
                                    alphabet_size,
                                    size_of_lengths,
                                    loss_cpu,
                                    ctc_gpu_workspace,
                                    info),
                   "Error: compute_ctc_loss in small_test");
/*
    auto d0 = gradient->dim_size(0);
    auto d1 = gradient->dim_size(1);
    auto d2 = gradient->dim_size(2);
    float * g = (float*) malloc(sizeof(float) * d0 * d1 * d2);
    fprintf(stderr, "456\n");
    cudaMemcpyAsync(g, gradient_t.data(), d0 * d1 * d2 * sizeof(float), cudaMemcpyDeviceToHost, stream);
    fprintf(stderr, "123\n");
    for (auto i0 = 0; i0 < d0; ++i0)
      for (auto i1 = 0; i1 < d1; ++i1)
      {
        for (auto i2 =  0; i2 < d2; ++i2)
        fprintf(stderr, "%f\t", g[(i0 * d1 + i1) * d2 + i2]);
        fprintf(stderr, "\n");
      }
*/
    cudaMemcpyAsync(loss_t.data(), loss_cpu, size_of_lengths * sizeof(float), cudaMemcpyHostToDevice, stream);

    throw_on_error(cudaFree(ctc_gpu_workspace),
                   "cudaFree");
/*
    throw_on_error(cudaStreamDestroy(stream),
                   "cudaStreamDestroy");
*/
  }

 private:
  bool preprocess_collapse_repeated_;
  bool ctc_merge_repeated_;
};

REGISTER_KERNEL_BUILDER(Name("WarpCtcLoss").Device(DEVICE_GPU), WarpCTCLossOp<GPUDevice>);

}  // end namespace tensorflow
