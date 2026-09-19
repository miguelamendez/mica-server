cmake_minimum_required(VERSION 3.24)
project(mica_server VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MICA_SERVER_BUILD_TESTS "Build mica-server tests" ON)

find_path(MICA_LUA_INCLUDE_DIR lua.h
  HINTS
    /opt/homebrew/opt/lua/include/lua
    /opt/homebrew/opt/lua/include/lua5.5
  PATH_SUFFIXES lua5.5 lua5.4 lua54 lua
)
find_library(MICA_LUA_LIBRARY
  NAMES lua lua5.5 lua5.4 lua54
  HINTS /opt/homebrew/opt/lua/lib
)
if(NOT MICA_LUA_INCLUDE_DIR OR NOT MICA_LUA_LIBRARY)
  message(FATAL_ERROR "Lua 5.4+ development headers and library are required")
endif()

set(MICA_LLAMA_VENDOR_HINT "${CMAKE_CURRENT_SOURCE_DIR}/../llama.cpp/vendor" CACHE PATH
    "Optional llama.cpp vendor directory containing nlohmann and cpp-httplib")
find_path(MICA_JSON_INCLUDE_DIR nlohmann/json.hpp HINTS "${MICA_LLAMA_VENDOR_HINT}")
find_path(MICA_HTTPLIB_INCLUDE_DIR httplib.h HINTS "${MICA_LLAMA_VENDOR_HINT}/cpp-httplib")

include(FetchContent)
if(NOT MICA_JSON_INCLUDE_DIR)
  FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68b4e7f541b3967fb4c52a0c6f9c7bf3e6ec8ef52d24f3a33d1
  )
  FetchContent_MakeAvailable(nlohmann_json)
endif()

if(NOT MICA_HTTPLIB_INCLUDE_DIR)
  FetchContent_Declare(
    cpp_httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.18.3
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(cpp_httplib)
endif()

add_library(mica_server_core
  src/catalog.cpp
  src/command.cpp
  src/config.cpp
  src/hardware.cpp
  src/profiles.cpp
  src/scheduler.cpp
  src/setup.cpp
)
target_include_directories(mica_server_core
  PUBLIC include
  PRIVATE ${MICA_LUA_INCLUDE_DIR}
)
target_link_libraries(mica_server_core PUBLIC ${MICA_LUA_LIBRARY} ${CMAKE_DL_LIBS})
if(UNIX)
  target_link_libraries(mica_server_core PUBLIC m)
endif()

if(MICA_JSON_INCLUDE_DIR)
  target_include_directories(mica_server_core PUBLIC "${MICA_JSON_INCLUDE_DIR}")
else()
  target_link_libraries(mica_server_core PUBLIC nlohmann_json::nlohmann_json)
endif()

if(MICA_HTTPLIB_INCLUDE_DIR)
  target_include_directories(mica_server_core PUBLIC "${MICA_HTTPLIB_INCLUDE_DIR}")
else()
  target_link_libraries(mica_server_core PUBLIC httplib::httplib)
endif()

if(APPLE)
  target_link_libraries(mica_server_core PRIVATE "-framework Security")
endif()

target_compile_options(mica_server_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(mica-server src/main.cpp src/server.cpp)
target_link_libraries(mica-server PRIVATE mica_server_core)
target_compile_definitions(mica-server PRIVATE
  MICA_SERVER_VERSION="${PROJECT_VERSION}")

include(GNUInstallDirs)

if(MICA_SERVER_BUILD_TESTS)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  enable_testing()
  add_executable(mica-server-tests tests/test_scheduler.cpp)
  target_link_libraries(mica-server-tests PRIVATE mica_server_core)
  add_test(NAME mica-server-tests COMMAND mica-server-tests)
  add_test(
    NAME mica-server-schema2-gguf-profile
    COMMAND $<TARGET_FILE:mica-server> setup --profile gguf-interactive
            --dry-run --ram-gib 8 --root /tmp/mica-schema2-gguf-test
            --config-dir ${CMAKE_CURRENT_SOURCE_DIR}/config
            --hardware-profile
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/hardware/mac-metal.json
  )
  set_tests_properties(mica-server-schema2-gguf-profile PROPERTIES
    PASS_REGULAR_EXPRESSION "installed_backends.*gguf.*validate native loopback bind"
    FAIL_REGULAR_EXPRESSION "uv venv|environments/tools|environments/mlx|environments/vllm"
  )
  add_test(
    NAME mica-server-schema2-reject-quant-override
    COMMAND $<TARGET_FILE:mica-server> plan --profile interactive --quant q8
            --root /tmp/mica-schema2-quant-test
            --config-dir ${CMAKE_CURRENT_SOURCE_DIR}/config
            --hardware-profile
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/hardware/mac-metal.json
  )
  set_tests_properties(mica-server-schema2-reject-quant-override PROPERTIES
    WILL_FAIL TRUE
  )
  add_test(
    NAME mica-server-auto-profile-mac
    COMMAND $<TARGET_FILE:mica-server> plan --profile auto --ram-gib 8
            --root /tmp/mica-auto-mac-test
            --config-dir ${CMAKE_CURRENT_SOURCE_DIR}/config
            --hardware-profile
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/hardware/mac-metal.json
  )
  set_tests_properties(mica-server-auto-profile-mac PROPERTIES
    PASS_REGULAR_EXPRESSION "mica-assistant-mlx"
  )
  add_test(
    NAME mica-server-auto-profile-linux
    COMMAND $<TARGET_FILE:mica-server> plan --profile auto --ram-gib 8
            --root /tmp/mica-auto-linux-test
            --config-dir ${CMAKE_CURRENT_SOURCE_DIR}/config
            --hardware-profile
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/hardware/linux-cpu.json
  )
  set_tests_properties(mica-server-auto-profile-linux PROPERTIES
    PASS_REGULAR_EXPRESSION "mica-assistant-gguf"
  )
  add_test(
    NAME mica-server-profile-file-plan
    COMMAND $<TARGET_FILE:mica-server> plan --profile-file
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/profiles/custom-assistant.json
            --root /tmp/mica-profile-file-test
            --config-dir ${CMAKE_CURRENT_SOURCE_DIR}/config
            --hardware-profile
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/hardware/mac-metal.json
  )
  set_tests_properties(mica-server-profile-file-plan PROPERTIES
    PASS_REGULAR_EXPRESSION "custom-assistant"
  )
  add_test(
    NAME mica-server-profile-external-gguf
    COMMAND $<TARGET_FILE:mica-server> plan --profile-file
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/profiles/external-gguf.json
            --root /tmp/mica-profile-external-test
            --config-dir ${CMAKE_CURRENT_SOURCE_DIR}/config
            --hardware-profile
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/hardware/linux-cpu.json
  )
  set_tests_properties(mica-server-profile-external-gguf PROPERTIES
    PASS_REGULAR_EXPRESSION "external-gguf"
  )
  add_test(
    NAME mica-server-profile-reject-remote-code
    COMMAND $<TARGET_FILE:mica-server> profile validate
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/profiles/unsafe-remote-code.json
            --root /tmp/mica-profile-unsafe-test
            --config-dir ${CMAKE_CURRENT_SOURCE_DIR}/config
  )
  set_tests_properties(mica-server-profile-reject-remote-code PROPERTIES
    WILL_FAIL TRUE
  )
  foreach(MICA_HARDWARE_CASE IN ITEMS mac-metal linux-cuda linux-rocm linux-xpu)
    add_test(
      NAME mica-server-gguf-build-${MICA_HARDWARE_CASE}
      COMMAND $<TARGET_FILE:mica-server> setup --profile mica-assistant-gguf
              --dry-run --ram-gib 8
              --root /tmp/mica-build-${MICA_HARDWARE_CASE}
              --config-dir ${CMAKE_CURRENT_SOURCE_DIR}/config
              --hardware-profile
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/hardware/${MICA_HARDWARE_CASE}.json
    )
  endforeach()
  set_tests_properties(mica-server-gguf-build-mac-metal PROPERTIES
    PASS_REGULAR_EXPRESSION "GGML_METAL=ON.*ENGINE_ENABLE_METAL=ON.*--parallel 2"
  )
  set_tests_properties(mica-server-gguf-build-linux-cuda PROPERTIES
    PASS_REGULAR_EXPRESSION "GGML_CUDA=ON.*ENGINE_ENABLE_CUDA=ON.*--parallel 2"
  )
  set_tests_properties(mica-server-gguf-build-linux-rocm PROPERTIES
    PASS_REGULAR_EXPRESSION "GGML_HIP=ON.*ENGINE_ENABLE_HIP=ON.*--parallel 2"
  )
  set_tests_properties(mica-server-gguf-build-linux-xpu PROPERTIES
    PASS_REGULAR_EXPRESSION "GGML_SYCL=ON.*ENGINE_ENABLE_VULKAN=ON.*--parallel 2"
  )
  add_test(
    NAME mica-server-quantize-plan
    COMMAND $<TARGET_FILE:mica-server> quantize --model fixture-asr --backend gguf
            --quant q8 --dry-run --root
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/custom-root
  )
  set_tests_properties(mica-server-quantize-plan PROPERTIES
    PASS_REGULAR_EXPRESSION "audiocpp_gguf.*--type q8_0"
  )
  add_test(
    NAME mica-server-builtin-quantize-plan
    COMMAND $<TARGET_FILE:mica-server> quantize --model spark-x25-4b --backend mlx
            --quant q4 --revision 0bcb35678590218655dff3765b9e61c83b35e9c4
            --dry-run --root ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/custom-root
            --config-dir ${CMAKE_CURRENT_SOURCE_DIR}/config
  )
  set_tests_properties(mica-server-builtin-quantize-plan PROPERTIES
    PASS_REGULAR_EXPRESSION "hf download XHToken/Spark-X2.5-4B.*mlx_vlm.convert.*staging/spark-x25-4b/0bcb356"
  )
  add_test(
    NAME mica-server-granite-quantize-plan
    COMMAND $<TARGET_FILE:mica-server> quantize --model granite-speech-5 --backend mlx
            --quant q4 --revision 6c14d3d052a602d850f1bc4aa017f25f4adf6aa0
            --dry-run --root ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/custom-root
            --config-dir ${CMAKE_CURRENT_SOURCE_DIR}/config
  )
  set_tests_properties(mica-server-granite-quantize-plan PROPERTIES
    PASS_REGULAR_EXPRESSION "mlx_audio_convert.py.*--profile granite-speech5-quality.*mlx_audio_profiles.json.*--q-bits 4.*--q-group-size 64.*--source-repo ibm-granite/granite-speech-5.0-470m-turboctc"
  )
  add_test(
    NAME mica-server-audio-profile-metadata
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/mlx_audio_convert.py
            --profile audio8-quality
            --profile-config ${CMAKE_CURRENT_SOURCE_DIR}/config/mlx_audio_profiles.json
            --describe-profile
  )
  set_tests_properties(mica-server-audio-profile-metadata PROPERTIES
    PASS_REGULAR_EXPRESSION "model.fast_output.*codec"
  )
  add_test(
    NAME mica-server-audio8-quantize-plan
    COMMAND $<TARGET_FILE:mica-server> quantize --model audio8-tts-06b --backend mlx
            --quant q4 --revision f07040f3d151f1ba0253bfb92cb2f5dd38b44594
            --dry-run --root ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/custom-root
            --config-dir ${CMAKE_CURRENT_SOURCE_DIR}/config
  )
  set_tests_properties(mica-server-audio8-quantize-plan PROPERTIES
    PASS_REGULAR_EXPRESSION "--profile audio8-quality.*mlx_audio_profiles.json.*--source-repo Audio8/Audio8-TTS-Preview-0.6b"
  )
  add_test(
    NAME mica-server-minicpm-quantize-plan
    COMMAND $<TARGET_FILE:mica-server> quantize --model minicpm-v46-thinking
            --backend mlx --quant q8
            --revision 93d8f4b60ad5d1f763442cf4c19f2a71fa95af4a
            --dry-run --root ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/custom-root
            --config-dir ${CMAKE_CURRENT_SOURCE_DIR}/config
  )
  set_tests_properties(mica-server-minicpm-quantize-plan PROPERTIES
    PASS_REGULAR_EXPRESSION "hf download openbmb/MiniCPM-V-4.6-Thinking.*mlx_vlm.convert.*--q-bits 8.*--q-group-size 64"
    FAIL_REGULAR_EXPRESSION "--mtp"
  )
  add_test(
    NAME mica-server-reject-both-serve
    COMMAND $<TARGET_FILE:mica-server> serve --backend both --root
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/custom-root
  )
  set_tests_properties(mica-server-reject-both-serve PROPERTIES
    WILL_FAIL TRUE
  )
  add_test(
    NAME mica-server-gguf-build-resource-guard
    COMMAND $<TARGET_FILE:mica-server> setup --backend gguf --profile all
            --quant q4 --dry-run --root
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/custom-root
            --config-dir ${CMAKE_CURRENT_SOURCE_DIR}/config
  )
  set_tests_properties(mica-server-gguf-build-resource-guard PROPERTIES
    PASS_REGULAR_EXPRESSION "AUDIOCPP_MODEL_SET=custom.*AUDIOCPP_MODELS=granite5asr,audio8_tts.*audiocpp_server audiocpp_gguf --parallel 2"
  )
  add_test(
    NAME mica-server-alternate-quant-does-not-block-setup
    COMMAND $<TARGET_FILE:mica-server> plan --backend gguf --profile all
            --quant q4,q8 --ram-gib 8 --root
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/custom-root
            --config-dir ${CMAKE_CURRENT_SOURCE_DIR}/config
            --hardware-profile
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/hardware/mac-metal.json
  )
  set_tests_properties(mica-server-alternate-quant-does-not-block-setup PROPERTIES
    PASS_REGULAR_EXPRESSION "required startup baseline exceeds RAM budget"
  )
  add_test(
    NAME mica-server-granite-gguf-name
    COMMAND $<TARGET_FILE:mica-server> quantize --model granite-speech-5
            --backend gguf --quant q8
            --revision 6c14d3d052a602d850f1bc4aa017f25f4adf6aa0
            --dry-run --root ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/custom-root
            --config-dir ${CMAKE_CURRENT_SOURCE_DIR}/config
  )
  set_tests_properties(mica-server-granite-gguf-name PROPERTIES
    PASS_REGULAR_EXPRESSION "granite-speech-5.0-470m-turboctc-q8_0.gguf --type q8_0 --family granite5asr"
  )
  add_test(
    NAME mica-server-audio8-gguf-name
    COMMAND $<TARGET_FILE:mica-server> quantize --model audio8-tts-06b
            --backend gguf --quant q4
            --revision f07040f3d151f1ba0253bfb92cb2f5dd38b44594
            --dry-run --root ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/custom-root
            --config-dir ${CMAKE_CURRENT_SOURCE_DIR}/config
  )
  set_tests_properties(mica-server-audio8-gguf-name PROPERTIES
    PASS_REGULAR_EXPRESSION "convert_audio8_tts_codec.py.*codec.pth.*codec.safetensors.*model_weights=.*codec_weights=.*audio8_tts.json.*audio8-tts-preview-0.6b-q4_0.gguf --type q4_0 --family audio8_tts"
  )
  add_test(
    NAME mica-server-spark-gguf-name
    COMMAND $<TARGET_FILE:mica-server> quantize --model spark-x25-4b
            --backend gguf --quant q4
            --revision 0bcb35678590218655dff3765b9e61c83b35e9c4
            --dry-run --root ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/custom-root
            --config-dir ${CMAKE_CURRENT_SOURCE_DIR}/config
  )
  set_tests_properties(mica-server-spark-gguf-name PROPERTIES
    PASS_REGULAR_EXPRESSION "Spark-X2.5-4B-Q4_K_M.gguf Q4_K_M"
  )
  add_test(
    NAME mica-server-minicpm-gguf-name
    COMMAND $<TARGET_FILE:mica-server> quantize --model minicpm-v46-thinking
            --backend gguf --quant q8
            --revision 93d8f4b60ad5d1f763442cf4c19f2a71fa95af4a
            --dry-run --root ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/custom-root
            --config-dir ${CMAKE_CURRENT_SOURCE_DIR}/config
  )
  set_tests_properties(mica-server-minicpm-gguf-name PROPERTIES
    PASS_REGULAR_EXPRESSION "MiniCPM-V-4_6-Thinking-Q8_0.gguf Q8_0"
  )
  foreach(target_profile IN ITEMS mac-metal linux-cuda linux-rocm linux-xpu linux-cpu)
    add_test(
      NAME mica-server-hardware-${target_profile}-gguf
      COMMAND $<TARGET_FILE:mica-server> setup --backend gguf --profile all
              --quant q4 --dry-run --root
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/custom-root
              --config-dir ${CMAKE_CURRENT_SOURCE_DIR}/config
              --hardware-profile
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/hardware/${target_profile}.json
    )
  endforeach()
  set_tests_properties(mica-server-hardware-mac-metal-gguf PROPERTIES
    PASS_REGULAR_EXPRESSION "GGML_METAL=ON.*ENGINE_ENABLE_METAL=ON"
  )
  set_tests_properties(mica-server-hardware-linux-cuda-gguf PROPERTIES
    PASS_REGULAR_EXPRESSION "GGML_CUDA=ON.*ENGINE_ENABLE_CUDA=ON"
  )
  set_tests_properties(mica-server-hardware-linux-rocm-gguf PROPERTIES
    PASS_REGULAR_EXPRESSION "GGML_HIP=ON.*ENGINE_ENABLE_HIP=ON"
  )
  set_tests_properties(mica-server-hardware-linux-xpu-gguf PROPERTIES
    PASS_REGULAR_EXPRESSION "GGML_SYCL=ON.*ENGINE_ENABLE_VULKAN=ON"
  )
  set_tests_properties(mica-server-hardware-linux-cpu-gguf PROPERTIES
    FAIL_REGULAR_EXPRESSION "GGML_(METAL|CUDA|HIP|SYCL|VULKAN)=ON"
  )
  foreach(target_profile IN ITEMS mac-metal linux-cuda linux-rocm linux-xpu linux-tpu linux-cpu)
    add_test(
      NAME mica-server-hardware-${target_profile}-vllm
      COMMAND $<TARGET_FILE:mica-server> setup --backend vllm --profile all
              --quant q4 --dry-run --root
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/custom-root
              --config-dir ${CMAKE_CURRENT_SOURCE_DIR}/config
              --hardware-profile
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/hardware/${target_profile}.json
    )
  endforeach()
  set_tests_properties(mica-server-hardware-mac-metal-vllm PROPERTIES
    PASS_REGULAR_EXPRESSION "latest compatible stable vLLM.*vllm-metal release wheels"
  )
  set_tests_properties(mica-server-hardware-linux-cuda-vllm PROPERTIES
    PASS_REGULAR_EXPRESSION "torch-backend auto"
  )
  set_tests_properties(mica-server-hardware-linux-rocm-vllm PROPERTIES
    PASS_REGULAR_EXPRESSION "wheels.vllm.ai/rocm"
  )
  set_tests_properties(mica-server-hardware-linux-xpu-vllm PROPERTIES
    PASS_REGULAR_EXPRESSION "wheels.vllm.ai/xpu"
  )
  set_tests_properties(mica-server-hardware-linux-tpu-vllm PROPERTIES
    PASS_REGULAR_EXPRESSION "tpu-inference"
  )
  set_tests_properties(mica-server-hardware-linux-cpu-vllm PROPERTIES
    PASS_REGULAR_EXPRESSION "nightly/cpu"
  )
  add_test(
    NAME mica-server-vllm-autoround-q4-plan
    COMMAND $<TARGET_FILE:mica-server> quantize --model spark-x25-4b
            --backend vllm --quant q4 --algorithm auto_round
            --revision 0bcb35678590218655dff3765b9e61c83b35e9c4
            --dry-run --root
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/vllm-root
            --config-dir ${CMAKE_CURRENT_SOURCE_DIR}/config
            --quant-device cuda
  )
  set_tests_properties(mica-server-vllm-autoround-q4-plan PROPERTIES
    PASS_REGULAR_EXPRESSION "vllm_quantize.py.*--source-repo XHToken/Spark-X2.5-4B.*--algorithm auto_round.*--scheme W4A16.*--calibration-samples 512.*--iterations 1000"
  )
  add_test(
    NAME mica-server-vllm-spark-defaults-to-gptq
    COMMAND $<TARGET_FILE:mica-server> quantize --model spark-x25-4b
            --backend vllm --quant q4
            --revision 0bcb35678590218655dff3765b9e61c83b35e9c4
            --dry-run --root
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/vllm-root
            --config-dir ${CMAKE_CURRENT_SOURCE_DIR}/config
            --quant-device cuda
  )
  set_tests_properties(mica-server-vllm-spark-defaults-to-gptq PROPERTIES
    PASS_REGULAR_EXPRESSION "--with llmcompressor==0.11.0.*--with transformers==4.57.1.*--algorithm gptq.*--scheme W4A16"
  )
  add_test(
    NAME mica-server-vllm-gptq-q8-plan
    COMMAND $<TARGET_FILE:mica-server> quantize --model spark-x25-4b
            --backend vllm --quant q8 --algorithm gptq
            --revision 0bcb35678590218655dff3765b9e61c83b35e9c4
            --dry-run --root
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/vllm-root
            --config-dir ${CMAKE_CURRENT_SOURCE_DIR}/config
            --quant-device cuda
  )
  set_tests_properties(mica-server-vllm-gptq-q8-plan PROPERTIES
    PASS_REGULAR_EXPRESSION "vllm_quantize.py.*--algorithm gptq.*--scheme W8A16.*--calibration-samples 512"
  )
  add_test(
    NAME mica-server-vllm-refuse-text-calibrated-audio
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/vllm_quantize.py
            --profiles ${CMAKE_CURRENT_SOURCE_DIR}/config/vllm_quantization_profiles.json
            --model-id audio8-tts-06b --capability tts
            --source ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/fake-audio8
            --source-repo Audio8/Audio8-TTS-Preview-0.6b
            --source-revision f07040f3d151f1ba0253bfb92cb2f5dd38b44594
            --source-license apache-2.0
            --output ${CMAKE_CURRENT_BINARY_DIR}/rejected-audio8-candidate
            --quant q4 --algorithm auto_round --scheme W4A16 --device cpu
            --calibration-samples 1 --calibration-sequence-length 16
            --batch-size 1 --iterations 1
  )
  set_tests_properties(mica-server-vllm-refuse-text-calibrated-audio PROPERTIES
    WILL_FAIL TRUE
  )
  add_test(
    NAME mica-server-vllm-refuse-audio-before-download
    COMMAND $<TARGET_FILE:mica-server> quantize --model audio8-tts-06b
            --backend vllm --quant q4
            --revision f07040f3d151f1ba0253bfb92cb2f5dd38b44594
            --dry-run --root
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/vllm-root
            --config-dir ${CMAKE_CURRENT_SOURCE_DIR}/config
  )
  set_tests_properties(mica-server-vllm-refuse-audio-before-download PROPERTIES
    WILL_FAIL TRUE
  )
  add_test(
    NAME mica-server-vllm-minicpm-multimodal-plan
    COMMAND $<TARGET_FILE:mica-server> quantize --model minicpm-v46-thinking
            --backend vllm --quant q4 --quant-device cuda
            --calibration-dataset
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/calibration/minicpm.jsonl
            --revision 93d8f4b60ad5d1f763442cf4c19f2a71fa95af4a
            --dry-run --root ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/vllm-root
            --config-dir ${CMAKE_CURRENT_SOURCE_DIR}/config
  )
  set_tests_properties(mica-server-vllm-minicpm-multimodal-plan PROPERTIES
    PASS_REGULAR_EXPRESSION "--with pillow==12.3.0.*--with torchvision==0.26.0.*--algorithm gptq.*--calibration-samples 512.*--batch-size 1.*--dataset .*minicpm.jsonl"
  )
  add_test(
    NAME mica-server-vllm-minicpm-q8-multimodal-plan
    COMMAND $<TARGET_FILE:mica-server> quantize --model minicpm-v46-thinking
            --backend vllm --quant q8 --quant-device cuda
            --calibration-dataset
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/calibration/minicpm.jsonl
            --revision 93d8f4b60ad5d1f763442cf4c19f2a71fa95af4a
            --dry-run --root ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/vllm-root
            --config-dir ${CMAKE_CURRENT_SOURCE_DIR}/config
  )
  set_tests_properties(mica-server-vllm-minicpm-q8-multimodal-plan PROPERTIES
    PASS_REGULAR_EXPRESSION "--algorithm gptq.*--scheme W8A16.*--calibration-samples 512.*--batch-size 1.*--dataset .*minicpm.jsonl"
  )
  add_test(
    NAME mica-server-vllm-audio8-modality-plan
    COMMAND $<TARGET_FILE:mica-server> quantize --model audio8-tts-06b
            --backend vllm --quant q8 --quant-device cuda
            --calibration-dataset /calibration/audio8.jsonl
            --revision f07040f3d151f1ba0253bfb92cb2f5dd38b44594
            --dry-run --root ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/vllm-root
            --config-dir ${CMAKE_CURRENT_SOURCE_DIR}/config
  )
  set_tests_properties(mica-server-vllm-audio8-modality-plan PROPERTIES
    PASS_REGULAR_EXPRESSION "--with soundfile==0.14.0.*--algorithm gptq.*--scheme W8A16.*--batch-size 1.*--dataset /calibration/audio8.jsonl"
  )
  add_test(
    NAME mica-server-vllm-audio8-q4-modality-plan
    COMMAND $<TARGET_FILE:mica-server> quantize --model audio8-tts-06b
            --backend vllm --quant q4 --quant-device cuda
            --calibration-dataset /calibration/audio8.jsonl
            --revision f07040f3d151f1ba0253bfb92cb2f5dd38b44594
            --dry-run --root ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/vllm-root
            --config-dir ${CMAKE_CURRENT_SOURCE_DIR}/config
  )
  set_tests_properties(mica-server-vllm-audio8-q4-modality-plan PROPERTIES
    PASS_REGULAR_EXPRESSION "--algorithm gptq.*--scheme W4A16.*--batch-size 1.*--dataset /calibration/audio8.jsonl"
  )
  add_test(
    NAME mica-server-vllm-granite-modality-plan
    COMMAND $<TARGET_FILE:mica-server> quantize --model granite-speech-5
            --backend vllm --quant q4 --quant-device cuda
            --calibration-dataset /calibration/granite.jsonl
            --revision 6c14d3d052a602d850f1bc4aa017f25f4adf6aa0
            --dry-run --root ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/vllm-root
            --config-dir ${CMAKE_CURRENT_SOURCE_DIR}/config
  )
  set_tests_properties(mica-server-vllm-granite-modality-plan PROPERTIES
    PASS_REGULAR_EXPRESSION "--with llmcompressor==0.13.1a20260824.*--with transformers==5.17.0.*--with soundfile==0.14.0.*--with torchaudio==2.11.0.*--algorithm gptq.*--scheme W4A16.*--batch-size 1.*--dataset /calibration/granite.jsonl"
  )
  add_test(
    NAME mica-server-vllm-granite-q8-modality-plan
    COMMAND $<TARGET_FILE:mica-server> quantize --model granite-speech-5
            --backend vllm --quant q8 --quant-device cuda
            --calibration-dataset /calibration/granite.jsonl
            --revision 6c14d3d052a602d850f1bc4aa017f25f4adf6aa0
            --dry-run --root ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/vllm-root
            --config-dir ${CMAKE_CURRENT_SOURCE_DIR}/config
  )
  set_tests_properties(mica-server-vllm-granite-q8-modality-plan PROPERTIES
    PASS_REGULAR_EXPRESSION "--algorithm gptq.*--scheme W8A16.*--batch-size 1.*--dataset /calibration/granite.jsonl"
  )
  add_test(
    NAME mica-server-vllm-calibration-manifest
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/vllm_calibration.py
            --manifest ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/calibration/minicpm.jsonl
            --capability vision --samples 2
  )
  set_tests_properties(mica-server-vllm-calibration-manifest PROPERTIES
    PASS_REGULAR_EXPRESSION "image.*video"
  )
  add_test(
    NAME mica-server-vllm-batch-smoke-help
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/openai_batch_smoke.py
            --help
  )
  set_tests_properties(mica-server-vllm-batch-smoke-help PROPERTIES
    PASS_REGULAR_EXPRESSION "batch-sizes"
  )
  add_test(
    NAME mica-server-playground-auth
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_playground_auth.py
  )
  add_test(
    NAME mica-server-api-auth
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_api_auth.sh
            $<TARGET_FILE:mica-server> ${CMAKE_CURRENT_SOURCE_DIR}/config
  )
endif()

install(TARGETS mica-server RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY config/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/mica-server/config
        FILES_MATCHING PATTERN "*.lua" PATTERN "*.json")
install(DIRECTORY scripts/ DESTINATION ${CMAKE_INSTALL_DATADIR}/mica-server/scripts
        FILES_MATCHING PATTERN "*.py" PATTERN "__pycache__" EXCLUDE)
install(FILES profiles/catalog.json
        DESTINATION ${CMAKE_INSTALL_DATADIR}/mica-server/profiles)
install(FILES apps/mica_playground.py apps/mica-logo.svg apps/mica-readme-header.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/mica-server/apps)
install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DATADIR}/mica-server/docs
        FILES_MATCHING PATTERN "*.md")
