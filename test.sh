#!/bin/bash
# 开启调试模式：自动打印每一条执行的指令，等效bash -x
set -x

source /data00/fused_deep_moe/wyb/umdk_smooth/vendors/CAM/bin/set_env.bash
export CAM_BUFFSIZE=1024
export HCCL_BUFFSIZE=1024
export LD_LIBRARY_PATH=/data00/fused_deep_moe/wyb/umdk_smooth/src/build_out/comm_operator/lib/:${LD_LIBRARY_PATH}
export CPATH=/data00/fused_deep_moe/wyb/umdk_smooth/src/cam/third_party/catlass/include:${CPATH}
rm -rf kernel_meta
rm -rf ~/ascend/log/
rm -rf ~/atc_data/kernel_cache/Ascend910_9392/te_fuseddeepmoe_*
# python src/cam/examples/fused_deep_moe_sample.py --with_share --share_expert_intermediate_size 3072 --test_graph --with_mc2_mask --with_smooth| tee fused_deep_moe_sample_logs/local_rank_0.log
# python src/cam/examples/fused_deep_moe_sample.py --with_share --share_expert_intermediate_size 3072 --with_mc2_mask --with_smooth| tee fused_deep_moe_sample_logs/local_rank_0.log



# python src/cam/examples/test_using_actual_data.py --with_mc2_mask --with_smooth| tee fused_deep_moe_sample_logs/local_rank_0.log
# python src/cam/examples/test_using_actual_data.py --test_graph --with_share --share_expert_intermediate_size 3072 --with_mc2_mask --with_smooth| tee fused_deep_moe_sample_logs/local_rank_0.log
python src/cam/examples/test_using_actual_data_bak.py  --test_graph --share_expert_intermediate_size 3072 --with_mc2_mask --with_smooth  --with_share| tee fused_deep_moe_sample_logs/local_rank_0.log
