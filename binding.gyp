{
  "targets": [
    {
      "target_name": "qpdf_compress",
      "sources": [
        "src/qpdf_addon.cc",
        "src/jpeg.cc",
        "src/images.cc",
        "src/fonts.cc",
        "src/content.cc",
        "src/strip.cc",
        "src/structure.cc",
        "src/font_subset.cc"
      ],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")",
        "deps/qpdf/include",
        "deps/mozjpeg/include",
        "deps/harfbuzz/include/harfbuzz",
        "src"
      ],
      "defines": [
        "NAPI_VERSION=8"
      ],
      "cflags!": [
        "-fno-exceptions"
      ],
      "cflags": [
        "-O2",
        "-flto",
        "-ffunction-sections",
        "-fdata-sections"
      ],
      "cflags_cc!": [
        "-fno-exceptions"
      ],
      "cflags_cc": [
        "-std=c++17",
        "-fvisibility=hidden"
      ],
      "conditions": [
        [
          "OS=='mac'",
          {
            "include_dirs": [
              "/opt/homebrew/include",
              "/usr/local/include",
              "<(module_root_dir)/deps/mozjpeg/include"
            ],
            "xcode_settings": {
              "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
              "CLANG_CXX_LANGUAGE_STANDARD": "c++17",
              "GCC_SYMBOLS_PRIVATE_EXTERN": "YES",
              "DEAD_CODE_STRIPPING": "YES",
              "LLVM_LTO": "YES",
              "OTHER_CPLUSPLUSFLAGS": [
                "-O2",
                "-flto",
                "-ffunction-sections",
                "-fdata-sections"
              ],
              "OTHER_LDFLAGS": [
                "<(module_root_dir)/deps/qpdf/lib/libqpdf.a",
                "<(module_root_dir)/deps/harfbuzz/lib/libharfbuzz-subset.a",
                "<(module_root_dir)/deps/harfbuzz/lib/libharfbuzz.a",
                "-L/opt/homebrew/lib",
                "-L/usr/local/lib",
                "-lz",
                "<(module_root_dir)/deps/mozjpeg/lib/libjpeg.a",
                "-flto",
                "-Wl,-dead_strip",
                "-Wl,-S"
              ]
            }
          }
        ],
        [
          "OS=='linux'",
          {
            "libraries": [
              "<(module_root_dir)/deps/qpdf/lib/libqpdf.a",
              "<(module_root_dir)/deps/harfbuzz/lib/libharfbuzz-subset.a",
              "<(module_root_dir)/deps/harfbuzz/lib/libharfbuzz.a",
              "-lz",
              "<(module_root_dir)/deps/mozjpeg/lib/libjpeg.a",
              "-static-libstdc++",
              "-flto",
              "-Wl,--allow-multiple-definition",
              "-Wl,--gc-sections",
              "-Wl,-S",
              "-lpthread",
              "-ldl"
            ]
          }
        ],
        [
          "OS=='win'",
          {
            "defines": [
              "_HAS_EXCEPTIONS=1"
            ],
            "msvs_settings": {
              "VCCLCompilerTool": {
                "ExceptionHandling": 1,
                "Optimization": 2,
                "WholeProgramOptimization": "true",
                "RuntimeLibrary": 0,
                "AdditionalOptions": [
                  "/std:c++17"
                ]
              },
              "VCLinkerTool": {
                "LinkTimeCodeGeneration": 1
              }
            },
            "libraries": [
              "<(module_root_dir)/deps/qpdf/lib/qpdf.lib",
              "<(module_root_dir)/deps/qpdf/lib/zlib.lib",
              "<(module_root_dir)/deps/mozjpeg/lib/jpeg-static.lib",
              "<(module_root_dir)/deps/harfbuzz/lib/harfbuzz-subset.lib",
              "<(module_root_dir)/deps/harfbuzz/lib/harfbuzz.lib"
            ]
          }
        ]
      ]
    }
  ]
}