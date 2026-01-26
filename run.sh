export SCRIPT_PATH=$(dirname "$(readink -f "$0")")

cd ${SCRIPT_PATH}/
find . -name "*.sh" | xargs -i chmod +x {}
find . -name "*.sh" | xargs -i dos2unix {}

bash build/cam/build.sh -c ascend910b4