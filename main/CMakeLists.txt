# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico CACHE STRING "Board type")


if (NOT FREERTOS_KERNEL_PATH AND NOT DEFINED ENV{FREERTOS_KERNEL_PATH})
    message("Skipping FreeRTOS examples as FREERTOS_KERNEL_PATH not defined")
    return()
endif()

include(FreeRTOS_Kernel_import.cmake)

# Pastas copiadas do expert de IA
set(MODEL_FOLDER ${CMAKE_SOURCE_DIR}/ei-model)
set(EI_SDK_FOLDER ${MODEL_FOLDER}/edge-impulse-sdk)
set(DRIVERS ${CMAKE_SOURCE_DIR}/Sensors)
set(EI_PLATFORM_FOLDER ${CMAKE_SOURCE_DIR}/edge-impulse)
set(FIRMWARE_SDK_FOLDER ${CMAKE_SOURCE_DIR}/firmware-sdk)


# ── Coleta de fontes do EI SDK ────────────────────────────────────────────────

file(GLOB_RECURSE EI_MODEL_SOURCES
    "${MODEL_FOLDER}/tflite-model/*.cpp"
)
file(GLOB EI_CLASSIFIER_SOURCES
    "${EI_SDK_FOLDER}/classifier/*.cpp"
)
file(GLOB_RECURSE EI_DSP_SOURCES
    "${EI_SDK_FOLDER}/dsp/*.cpp"
)
file(GLOB EI_TF_C_SOURCES
    "${EI_SDK_FOLDER}/tensorflow/lite/c/*.cc"
)
file(GLOB_RECURSE EI_TF_MICRO_SOURCES
    "${EI_SDK_FOLDER}/tensorflow/lite/micro/*.cc"
    "${EI_SDK_FOLDER}/tensorflow/lite/kernels/internal/quantization_util.cc"
)
file(GLOB_RECURSE EI_TF_CORE_SOURCES
    "${EI_SDK_FOLDER}/tensorflow/lite/core/c/*.cc"
)
file(GLOB_RECURSE EI_CMSIS_SOURCES
    "${EI_SDK_FOLDER}/CMSIS/DSP/Source/TransformFunctions/*.c"
    "${EI_SDK_FOLDER}/CMSIS/DSP/Source/CommonTables/*.c"
    "${EI_SDK_FOLDER}/CMSIS/DSP/Source/ComplexMathFunctions/*.c"
    "${EI_SDK_FOLDER}/CMSIS/DSP/Source/BasicMathFunctions/*.c"
    "${EI_SDK_FOLDER}/CMSIS/DSP/Source/FastMathFunctions/*.c"
    "${EI_SDK_FOLDER}/CMSIS/DSP/Source/SupportFunctions/*.c"
    "${EI_SDK_FOLDER}/CMSIS/DSP/Source/StatisticsFunctions/*.c"
    "${EI_SDK_FOLDER}/CMSIS/DSP/Source/MatrixFunctions/*.c"
)
file(GLOB_RECURSE EI_CMSIS_NN_SOURCES
    "${EI_SDK_FOLDER}/CMSIS/NN/Source/*.c"
)
file(GLOB_RECURSE EI_TF_KERNELS_INTERNAL_SOURCES
    "${EI_SDK_FOLDER}/tensorflow/lite/kernels/internal/portable_tensor_utils.cc"
    "${EI_SDK_FOLDER}/tensorflow/lite/kernels/internal/tensor_utils.cc"
    "${EI_SDK_FOLDER}/tensorflow/lite/kernels/internal/reference_portable_tensor_utils.cc"
    "${EI_SDK_FOLDER}/tensorflow/lite/kernels/kernel_util_lite.cc"
)
file(GLOB EI_TF_CORE_API_SOURCES
    "${EI_SDK_FOLDER}/tensorflow/lite/core/api/*.cc"
)

# Remove arquivos de plataformas incompatíveis
foreach(SRC_LIST EI_TF_MICRO_SOURCES EI_TF_CORE_SOURCES EI_DSP_SOURCES EI_CMSIS_SOURCES)
    list(FILTER ${SRC_LIST} EXCLUDE REGEX ".*/zephyr/.*")
    list(FILTER ${SRC_LIST} EXCLUDE REGEX ".*/espressif/.*")
    list(FILTER ${SRC_LIST} EXCLUDE REGEX ".*/ethos.*")
    list(FILTER ${SRC_LIST} EXCLUDE REGEX ".*debug_log.*")
    list(FILTER ${SRC_LIST} EXCLUDE REGEX ".*/examples/.*")
    list(FILTER ${SRC_LIST} EXCLUDE REGEX ".*/test.*")
endforeach()

# ── Executável ────────────────────────────────────────────────────────────────

add_executable(pico_emb
    main.cpp
    hc06.c
    ${EI_MODEL_SOURCES}
    ${EI_CLASSIFIER_SOURCES}
    ${EI_DSP_SOURCES}
    ${EI_TF_C_SOURCES}
    ${EI_TF_MICRO_SOURCES}
    ${EI_TF_CORE_SOURCES}
    ${EI_TF_CORE_API_SOURCES}
    ${EI_CMSIS_SOURCES}
    ${EI_CMSIS_NN_SOURCES}
    ${EI_TF_KERNELS_INTERNAL_SOURCES}
    )

pico_enable_stdio_usb(pico_emb 1)
pico_enable_stdio_uart(pico_emb 0)

target_include_directories(pico_emb PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${MODEL_FOLDER}
    ${EI_SDK_FOLDER}/CMSIS/Core/Include
    ${EI_SDK_FOLDER}/CMSIS/DSP/Include
    ${EI_SDK_FOLDER}/CMSIS/NN/Include
    )

# Linking to FreeRTOS-Kernel-Heap4 means we use a dynamic heap for allocations
target_link_libraries(pico_emb PRIVATE
    pico_async_context_freertos
    FreeRTOS-Kernel-Heap4
    pico_stdlib
    hardware_gpio
    hardware_i2c
    hardware_irq
    hardware_uart
    hardware_adc
    hardware_flash
    hardware_pio
    hardware_dma
    )
# Set the nunber of cores to 1.
# This defaults to 2 in FreeRTOSConfig_examples_common.h if not defined in here
target_compile_definitions(pico_emb PRIVATE
    # SMP — ativa os 2 núcleos do RP2040.
    # O FreeRTOSConfig.h detecta FREE_RTOS_KERNEL_SMP e define:
    #   configNUMBER_OF_CORES=2, configUSE_CORE_AFFINITY=1
    FREE_RTOS_KERNEL_SMP=1
    # Edge Impulse
    EI_CLASSIFIER_COMPILED=1
    EIDSP_USE_CMSIS_DSP=1
    EIDSP_LOAD_CMSIS_DSP_SOURCES=1
    ARM_MATH_CM0PLUS
    __FPU_PRESENT=1
    DISABLEFLOAT16=1
    )
if(PICO_CYW43_SUPPORTED)
    # For led support on pico_w
    target_link_libraries(pico_emb PRIVATE
        pico_cyw43_arch_none
        )
endif()
pico_add_extra_outputs(pico_emb)

set_target_properties(pico_emb PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
