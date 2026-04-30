#pragma once

#include <torch/types.h>
#include <torch/python.h>
#include <tuple>
#include <vector>
#include <optional>
#include <string>

namespace fused_deep_moe {

class Buffer {
private:
    int rank_;
    int num_ranks_;
    bool initialized_ = false;
    void *ext_info_ptr_ = nullptr;
    void *shmem_workspace_ptr_ = nullptr;
    uint64_t shmem_workspace_size_ = 0;

public:
    Buffer() = default;

    ~Buffer() noexcept(false);

    void init(int rank, int num_ranks, uint64_t memsize, const std::string &ip_port);

    int64_t get_ext_info() const;

    int64_t get_shmem_workspace() const;

    bool is_initialized() const;

    std::vector<at::Tensor> zb_fused_deep_moe(
        const at::Tensor &x,
        const at::Tensor &expert_ids,
        const at::TensorList &gmm1_weight,
        const at::TensorList &gmm1_weight_scale,
        const at::TensorList &gmm2_weight,
        const at::TensorList &gmm2_weight_scale,
        const at::Tensor &expert_scales,
        const c10::optional<at::Tensor> &share_gmm1_weight,
        const c10::optional<at::Tensor> &share_gmm1_weight_scale,
        const c10::optional<at::Tensor> &share_gmm2_weight,
        const c10::optional<at::Tensor> &share_gmm2_weight_scale,
        const c10::optional<at::Tensor> &expert_smooth_scales,
        const c10::optional<at::Tensor> &share_smooth_scales,
        const c10::optional<at::Tensor> &x_active_mask,
        c10::string_view group_ep,
        int64_t ep_rank_size,
        int64_t ep_rank_id,
        int64_t moe_expert_num,
        int64_t quant_mode,
        int64_t global_bs);
};

}  // namespace fused_deep_moe
