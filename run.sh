export SCRIPT_PATH=$(dirname "$(readlink -f "$0")")

cd ${SCRIPT_PATH}/
find . -name "*.sh" | xargs -i chmod +x {}
find . -name "*.sh" | xargs -i dos2unix {}

# bash build/cam/build.sh -c ascend910b4

case $1 in
    0)
        echo "build"
        bash build/cam/build.sh || exit
        bash output/cam/comm_operator/run/CAM_ascend910_93*.run || exit
        bash build.sh -p
        pip install --force-reinstall output/cam/comm_operator/dist/umdk_cam_op*.whl || exit
        ;;
    1)
        echo "run"
        python src/cam/examples/fused_deep_moe_sample.py
        ;;
    *)
        echo "invalid argument"
        ;;
esac